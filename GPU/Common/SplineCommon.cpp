// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <string.h>
#include <algorithm>

#include "profiler/profiler.h"

#include "Common/CPUDetect.h"
#include "Common/MemoryUtil.h"
#include "Core/Config.h"

#include "GPU/Common/GPUStateUtils.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"  // only needed for UVScale stuff

static void CopyQuadIndex(u16 *&indices, GEPatchPrimType type, const int idx0, const int idx1, const int idx2, const int idx3) {
	if (type == GE_PATCHPRIM_LINES) {
		*(indices++) = idx0;
		*(indices++) = idx2;
		*(indices++) = idx1;
		*(indices++) = idx3;
		*(indices++) = idx1;
		*(indices++) = idx2;
	} else {
		*(indices++) = idx0;
		*(indices++) = idx2;
		*(indices++) = idx1;
		*(indices++) = idx1;
		*(indices++) = idx2;
		*(indices++) = idx3;
	}
}

static void BuildIndex(u16 *indices, int &count, int num_u, int num_v, GEPatchPrimType prim_type, int total = 0) {
	for (int v = 0; v < num_v; ++v) {
		for (int u = 0; u < num_u; ++u) {
			int idx0 = v * (num_u + 1) + u + total; // Top left
			int idx2 = (v + 1) * (num_u + 1) + u + total; // Bottom left

			CopyQuadIndex(indices, prim_type, idx0, idx0 + 1, idx2, idx2 + 1);
			count += 6;
		}
	}
}

// Bernstein basis functions
inline float bern0(float x) { return (1 - x) * (1 - x) * (1 - x); }
inline float bern1(float x) { return 3 * x * (1 - x) * (1 - x); }
inline float bern2(float x) { return 3 * x * x * (1 - x); }
inline float bern3(float x) { return x * x * x; }

inline float bern0deriv(float x) { return -3 * (x - 1) * (x - 1); }
inline float bern1deriv(float x) { return 9 * x * x - 12 * x + 3; }
inline float bern2deriv(float x) { return 3 * (2 - 3 * x) * x; }
inline float bern3deriv(float x) { return 3 * x * x; }

// http://en.wikipedia.org/wiki/Bernstein_polynomial
template<class T>
static T Bernstein3D(const T& p0, const T& p1, const T& p2, const T& p3, float x) {
	if (x == 0) return p0;
	if (x == 1) return p3;
	return p0 * bern0(x) + p1 * bern1(x) + p2 * bern2(x) + p3 * bern3(x);
}

static Vec3f Bernstein3DDerivative(const Vec3f& p0, const Vec3f& p1, const Vec3f& p2, const Vec3f& p3, float x) {
	return p0 * bern0deriv(x) + p1 * bern1deriv(x) + p2 * bern2deriv(x) + p3 * bern3deriv(x);
}

struct KnotDiv {
	float _3_0 = 1.0f / 3.0f;
	float _4_1 = 1.0f / 3.0f;
	float _5_2 = 1.0f / 3.0f;
	float _3_1 = 1.0f / 2.0f;
	float _4_2 = 1.0f / 2.0f;
	float _3_2 = 1.0f; // Always 1
};

static void spline_n_4(int i, float t, float *knot, const KnotDiv &div, float *splineVal, float *derivs) {
	knot += i;

#ifdef _M_SSE
	const __m128 knot012 = _mm_loadu_ps(knot);
	const __m128 t012 = _mm_sub_ps(_mm_set_ps1(t), knot012);
	const __m128 f30_41_52 = _mm_mul_ps(t012, _mm_loadu_ps(&div._3_0));
	const __m128 f52_31_42 = _mm_mul_ps(t012, _mm_loadu_ps(&div._5_2));
	const float &f32 = t012.m128_f32[2];

	// Following comments are for explains order of the multiply.
//	float a = (1-f30)*(1-f31);
//	float c = (1-f41)*(1-f42);
//	float b = (  f31 *   f41);
//	float d = (  f42 *   f52);
	const __m128 f30_41_31_42 = _mm_shuffle_ps(f30_41_52, f52_31_42, _MM_SHUFFLE(2, 1, 1, 0));
	const __m128 f31_42_41_52 = _mm_shuffle_ps(f52_31_42, f30_41_52, _MM_SHUFFLE(2, 1, 2, 1));
	const __m128 c1_1_0_0 = { 1, 1, 0, 0 };
	const __m128 acbd = _mm_mul_ps(_mm_sub_ps(c1_1_0_0, f30_41_31_42), _mm_sub_ps(c1_1_0_0, f31_42_41_52));
	const float &a = acbd.m128_f32[0];
	const float &b = acbd.m128_f32[2];
	const float &c = acbd.m128_f32[1];
	const float &d = acbd.m128_f32[3];

	// For derivative
	const float &f31 = f30_41_31_42.m128_f32[2];
	const float &f42 = f30_41_31_42.m128_f32[3];
#else
	// TODO: Maybe compilers could be coaxed into vectorizing this code without the above explicitly...
	float t0 = (t - knot[0]);
	float t1 = (t - knot[1]);
	float t2 = (t - knot[2]);

	float f30 = t0 * div._3_0;
	float f41 = t1 * div._4_1;
	float f52 = t2 * div._5_2;
	float f31 = t1 * div._3_1;
	float f42 = t2 * div._4_2;
	float f32 = t2 * div._3_2;

	float a = (1-f30)*(1-f31);
	float b = (f31*f41);
	float c = (1-f41)*(1-f42);
	float d = (f42*f52);
#endif

	splineVal[0] = a-(a*f32);
	splineVal[1] = 1-a-b+((a+b+c-1)*f32);
	splineVal[2] = b+((1-b-c-d)*f32);
	splineVal[3] = d*f32;

	// Derivative
	float i1 = (1 - f31) * (1 - f32);
	float i2 = f31 * (1 - f32) + (1 - f42) * f32;
	float i3 = f42 * f32;

	float f130 = i1 * div._3_0;
	float f241 = i2 * div._4_1;
	float f352 = i3 * div._5_2;

	derivs[0] = 3 * (0 - f130);
	derivs[1] = 3 * (f130 - f241);
	derivs[2] = 3 * (f241 - f352);
	derivs[3] = 3 * (f352 - 0);
}

// knot should be an array sized n + 5  (n + 1 + 1 + degree (cubic))
static void spline_knot(int n, int type, float *knots, KnotDiv *divs) {
		// Basic theory (-2 to +3), optimized with KnotDiv (-2 to +0) 
	//	for (int i = 0; i < n + 5; ++i) {
		for (int i = 0; i < n + 2; ++i) {
			knots[i] = (float)i - 2;
		}

		// The first edge is open
		if ((type & 1) != 0) {
			knots[0] = 0;
			knots[1] = 0;

			divs[0]._3_0 = 1.0f;
			divs[0]._4_1 = 1.0f / 2.0f;
			divs[0]._3_1 = 1.0f;
			if (n > 1)
				divs[1]._3_0 = 1.0f / 2.0f;
		}
		// The last edge is open
		if ((type & 2) != 0) {
		//	knots[n + 2] = (float)n; // Got rid of this line optimized with KnotDiv
		//	knots[n + 3] = (float)n; // Got rid of this line optimized with KnotDiv
		//	knots[n + 4] = (float)n; // Got rid of this line optimized with KnotDiv
			divs[n - 1]._4_1 = 1.0f / 2.0f;
			divs[n - 1]._5_2 = 1.0f;
			divs[n - 1]._4_2 = 1.0f;
			if (n > 1)
				divs[n - 2]._5_2 = 1.0f / 2.0f;
		}
}

bool CanUseHardwareTessellation(GEPatchPrimType prim) {
	if (g_Config.bHardwareTessellation && !g_Config.bSoftwareRendering) {
		return CanUseHardwareTransform(PatchPrimToPrim(prim));
	}
	return false;
}

// Prepare mesh of one patch for "Instanced Tessellation".
static void TessellateSplinePatchHardware(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch) {
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	float inv_u = 1.0f / (float)spatch.tess_u;
	float inv_v = 1.0f / (float)spatch.tess_v;

	// Generating simple input vertices for the spline-computing vertex shader.
	for (int tile_v = 0; tile_v < spatch.tess_v + 1; ++tile_v) {
		for (int tile_u = 0; tile_u < spatch.tess_u + 1; ++tile_u) {
			SimpleVertex &vert = vertices[tile_v * (spatch.tess_u + 1) + tile_u];
			vert.pos.x = (float)tile_u * inv_u;
			vert.pos.y = (float)tile_v * inv_v;

			// TODO: Move to shader uniform and unify this method spline and bezier if necessary.
			// For compute normal
			vert.nrm.x = inv_u;
			vert.nrm.y = inv_v;
		}
	}

	BuildIndex(indices, count, spatch.tess_u, spatch.tess_v, spatch.primType);
}

template <bool origNrm, bool origCol, bool origTc, bool useSSE4>
static void SplinePatchFullQuality(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	// Full (mostly) correct tessellation of spline patches.
	// Not very fast.

	float *knot_u = new float[spatch.count_u + 4];
	float *knot_v = new float[spatch.count_v + 4];
	KnotDiv *divs_u = new KnotDiv[spatch.count_u - 3];
	KnotDiv *divs_v = new KnotDiv[spatch.count_v - 3];
	spline_knot(spatch.count_u - 3, spatch.type_u, knot_u, divs_u);
	spline_knot(spatch.count_v - 3, spatch.type_v, knot_v, divs_v);

	// Increase tessellation based on the size. Should be approximately right?
	int patch_div_s = (spatch.count_u - 3) * spatch.tess_u;
	int patch_div_t = (spatch.count_v - 3) * spatch.tess_v;
	if (quality == 0) {
		// Low quality
		patch_div_s = (spatch.count_u - 3) * 2;
		patch_div_t = (spatch.count_v - 3) * 2;
	}
	if (quality > 1) {
		// Don't cut below 2, though.
		if (patch_div_s > 2) {
			patch_div_s /= quality;
		}
		if (patch_div_t > 2) {
			patch_div_t /= quality;
		}
	}

	// Downsample until it fits, in case crazy tessellation factors are sent.
	while ((patch_div_s + 1) * (patch_div_t + 1) > maxVertices) {
		patch_div_s /= 2;
		patch_div_t /= 2;
	}

	if (patch_div_s < 1) patch_div_s = 1;
	if (patch_div_t < 1) patch_div_t = 1;

	// First compute all the vertices and put them in an array
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	float tu_width = (float)spatch.count_u - 3.0f;
	float tv_height = (float)spatch.count_v - 3.0f;

	// int max_idx = spatch.count_u * spatch.count_v;

	bool computeNormals = spatch.computeNormals;

	float one_over_patch_div_s = 1.0f / (float)(patch_div_s);
	float one_over_patch_div_t = 1.0f / (float)(patch_div_t);

	for (int tile_v = 0; tile_v < patch_div_t + 1; tile_v++) {
		float v = (float)tile_v * (float)(spatch.count_v - 3) * one_over_patch_div_t;
		if (v < 0.0f)
			v = 0.0f;
		for (int tile_u = 0; tile_u < patch_div_s + 1; tile_u++) {
			float u = (float)tile_u * (float)(spatch.count_u - 3) * one_over_patch_div_s;
			if (u < 0.0f)
				u = 0.0f;
			SimpleVertex *vert = &vertices[tile_v * (patch_div_s + 1) + tile_u];
			Vec4f vert_color(0, 0, 0, 0);
			Vec3f vert_pos;
			vert_pos.SetZero();
			Vec3f du, dv;
			Vec2f vert_tex;
			if (origNrm) {
				du.SetZero();
				dv.SetZero();
			}
			if (origCol) {
				vert_color.SetZero();
			} else {
				vert->color_32 = spatch.defcolor;
			}
			if (origTc) {
				vert_tex.SetZero();
			} else {
				vert->uv[0] = tu_width * ((float)tile_u * one_over_patch_div_s);
				vert->uv[1] = tv_height * ((float)tile_v * one_over_patch_div_t);
			}


			// Collect influences from surrounding control points.
			float u_weights[4];
			float v_weights[4];
			float u_derivs[4];
			float v_derivs[4];

			int iu = (int)u;
			int iv = (int)v;

			// TODO: Would really like to fix the surrounding logic somehow to get rid of these but I can't quite get it right..
			// Without the previous epsilons and with large count_u, we will end up doing an out of bounds access later without these.
			if (iu >= spatch.count_u - 3) iu = spatch.count_u - 4;
			if (iv >= spatch.count_v - 3) iv = spatch.count_v - 4;

			spline_n_4(iu, u, knot_u, divs_u[iu], u_weights, u_derivs);
			spline_n_4(iv, v, knot_v, divs_v[iv], v_weights, v_derivs);

			// Handle degenerate patches. without this, spatch.points[] may read outside the number of initialized points.
			int patch_w = std::min(spatch.count_u - iu, 4);
			int patch_h = std::min(spatch.count_v - iv, 4);

			for (int ii = 0; ii < patch_w; ++ii) {
				for (int jj = 0; jj < patch_h; ++jj) {
					float u_spline = u_weights[ii];
					float v_spline = v_weights[jj];
					float f = u_spline * v_spline;

					if (f > 0.0f) {
						int idx = spatch.count_u * (iv + jj) + (iu + ii);
						/*
						if (idx >= max_idx) {
							char temp[512];
							snprintf(temp, sizeof(temp), "count_u: %d count_v: %d patch_w: %d patch_h: %d  ii: %d  jj: %d  iu: %d  iv: %d  patch_div_s: %d  patch_div_t: %d\n", spatch.count_u, spatch.count_v, patch_w, patch_h, ii, jj, iu, iv, patch_div_s, patch_div_t);
							OutputDebugStringA(temp);
							Crash();
						}*/
						vert_pos += spatch.pos[idx] * f;
						if (origTc) {
							vert_tex += spatch.tex[idx] * f;
						}
						if (origCol) {
							vert_color += spatch.col[idx] * f;
						}
						if (origNrm) {
							du += spatch.pos[idx] * (u_derivs[ii] * v_weights[jj]);
							dv += spatch.pos[idx] * (u_weights[ii] * v_derivs[jj]);
						}
					}
				}
			}
			vert->pos = vert_pos;
			if (origNrm) {
				vert->nrm = Cross(du, dv).Normalized(useSSE4);
			} else {
				vert->nrm.SetZero();
				vert->nrm.z = 1.0f;
			}
			if (origCol) {
				vert->color_32 = vert_color.ToRGBA();
			}
			if (origTc) {
				vert_tex.Write(vert->uv);
			}
		}
	}

	delete[] knot_u;
	delete[] knot_v;
	delete[] divs_u;
	delete[] divs_v;

	BuildIndex(indices, count, patch_div_s, patch_div_t, spatch.primType);
}

template <bool origNrm, bool origCol, bool origTc>
static inline void SplinePatchFullQualityDispatch4(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	if (cpu_info.bSSE4_1)
		SplinePatchFullQuality<origNrm, origCol, origTc, true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQuality<origNrm, origCol, origTc, false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

template <bool origNrm, bool origCol>
static inline void SplinePatchFullQualityDispatch3(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	bool origTc = (origVertType & GE_VTYPE_TC_MASK) != 0;

	if (origTc)
		SplinePatchFullQualityDispatch4<origNrm, origCol, true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQualityDispatch4<origNrm, origCol, false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

template <bool origNrm>
static inline void SplinePatchFullQualityDispatch2(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	bool origCol = (origVertType & GE_VTYPE_COL_MASK) != 0;

	if (origCol)
		SplinePatchFullQualityDispatch3<origNrm, true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQualityDispatch3<origNrm, false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

static void SplinePatchFullQualityDispatch(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	bool origNrm = (origVertType & GE_VTYPE_NRM_MASK) != 0;

	if (origNrm)
		SplinePatchFullQualityDispatch2<true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQualityDispatch2<false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

void TessellateSplinePatch(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int maxVertexCount) {
	switch (g_Config.iSplineBezierQuality) {
	case LOW_QUALITY:
		SplinePatchFullQualityDispatch(dest, indices, count, spatch, origVertType, 0, maxVertexCount);
		break;
	case MEDIUM_QUALITY:
		SplinePatchFullQualityDispatch(dest, indices, count, spatch, origVertType, 2, maxVertexCount);
		break;
	case HIGH_QUALITY:
		SplinePatchFullQualityDispatch(dest, indices, count, spatch, origVertType, 1, maxVertexCount);
		break;
	}
}

template <typename T>
struct PrecomputedCurves {
	PrecomputedCurves(int count) {
		horiz1 = (T *)AllocateAlignedMemory(count * 4 * sizeof(T), 16);
		horiz2 = horiz1 + count * 1;
		horiz3 = horiz1 + count * 2;
		horiz4 = horiz1 + count * 3;
	}
	~PrecomputedCurves() {
		FreeAlignedMemory(horiz1);
	}

	T Bernstein3D(int u, float bv) {
		return ::Bernstein3D(horiz1[u], horiz2[u], horiz3[u], horiz4[u], bv);
	}

	T Bernstein3DDerivative(int u, float bv) {
		return ::Bernstein3DDerivative(horiz1[u], horiz2[u], horiz3[u], horiz4[u], bv);
	}

	T *horiz1;
	T *horiz2;
	T *horiz3;
	T *horiz4;
};

static void _BezierPatchHighQuality(u8 *&dest, u16 *&indices, int &count, int tess_u, int tess_v, const BezierPatch &patch, u32 origVertType) {
	const float third = 1.0f / 3.0f;

	// First compute all the vertices and put them in an array
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	PrecomputedCurves<Vec3f> prepos(tess_u + 1);
	PrecomputedCurves<Vec4f> precol(tess_u + 1);
	PrecomputedCurves<Vec2f> pretex(tess_u + 1);
	PrecomputedCurves<Vec3f> prederivU(tess_u + 1);

	const bool computeNormals = patch.computeNormals;
	const bool sampleColors = (origVertType & GE_VTYPE_COL_MASK) != 0;
	const bool sampleTexcoords = (origVertType & GE_VTYPE_TC_MASK) != 0;

	int num_patches_u = (patch.count_u - 1) / 3;
	int num_patches_v = (patch.count_v - 1) / 3;
	for (int patch_u = 0; patch_u < num_patches_u; ++patch_u) {
		for (int patch_v = 0; patch_v < num_patches_v; ++patch_v) {
			// Precompute the horizontal curves to we only have to evaluate the vertical ones.
			Vec3f *_pos[16];
			Vec4f *_col[16];
			Vec2f *_tex[16];
			for (int point = 0; point < 16; ++point) {
				int idx = (patch_u * 3 + point % 4) + (patch_v * 3 + point / 4) * patch.count_u;
				_pos[point] = &patch.pos[idx];
				_col[point] = &patch.col[idx];
				_tex[point] = &patch.tex[idx];
			}
			for (int i = 0; i < tess_u + 1; i++) {
				float u = ((float)i / (float)tess_u);
				prepos.horiz1[i] = Bernstein3D(*_pos[0], *_pos[1], *_pos[2], *_pos[3], u);
				prepos.horiz2[i] = Bernstein3D(*_pos[4], *_pos[5], *_pos[6], *_pos[7], u);
				prepos.horiz3[i] = Bernstein3D(*_pos[8], *_pos[9], *_pos[10], *_pos[11], u);
				prepos.horiz4[i] = Bernstein3D(*_pos[12], *_pos[13], *_pos[14], *_pos[15], u);

				if (sampleColors) {
					precol.horiz1[i] = Bernstein3D(*_col[0], *_col[1], *_col[2], *_col[3], u);
					precol.horiz2[i] = Bernstein3D(*_col[4], *_col[5], *_col[6], *_col[7], u);
					precol.horiz3[i] = Bernstein3D(*_col[8], *_col[9], *_col[10], *_col[11], u);
					precol.horiz4[i] = Bernstein3D(*_col[12], *_col[13], *_col[14], *_col[15], u);
				}
				if (sampleTexcoords) {
					pretex.horiz1[i] = Bernstein3D(*_tex[0], *_tex[1], *_tex[2], *_tex[3], u);
					pretex.horiz2[i] = Bernstein3D(*_tex[4], *_tex[5], *_tex[6], *_tex[7], u);
					pretex.horiz3[i] = Bernstein3D(*_tex[8], *_tex[9], *_tex[10], *_tex[11], u);
					pretex.horiz4[i] = Bernstein3D(*_tex[12], *_tex[13], *_tex[14], *_tex[15], u);
				}

				if (computeNormals) {
					prederivU.horiz1[i] = Bernstein3DDerivative(*_pos[0], *_pos[1], *_pos[2], *_pos[3], u);
					prederivU.horiz2[i] = Bernstein3DDerivative(*_pos[4], *_pos[5], *_pos[6], *_pos[7], u);
					prederivU.horiz3[i] = Bernstein3DDerivative(*_pos[8], *_pos[9], *_pos[10], *_pos[11], u);
					prederivU.horiz4[i] = Bernstein3DDerivative(*_pos[12], *_pos[13], *_pos[14], *_pos[15], u);
				}
			}

			for (int tile_v = 0; tile_v < tess_v + 1; ++tile_v) {
				for (int tile_u = 0; tile_u < tess_u + 1; ++tile_u) {
					float u = ((float)tile_u / (float)tess_u);
					float v = ((float)tile_v / (float)tess_v);
					float bv = v;

					SimpleVertex &vert = vertices[tile_v * (tess_u + 1) + tile_u];

					if (computeNormals) {
						const Vec3f derivU = prederivU.Bernstein3D(tile_u, bv);
						const Vec3f derivV = prepos.Bernstein3DDerivative(tile_u, bv);

						vert.nrm = Cross(derivU, derivV).Normalized();
						if (patch.patchFacing)
							vert.nrm *= -1.0f;
					} else {
						vert.nrm.SetZero();
					}

					vert.pos = prepos.Bernstein3D(tile_u, bv);

					if (!sampleTexcoords) {
						// Generate texcoord
						vert.uv[0] = u + patch_u * third;
						vert.uv[1] = v + patch_u * third;
					} else {
						// Sample UV from control points
						const Vec2f res = pretex.Bernstein3D(tile_u, bv);
						vert.uv[0] = res.x;
						vert.uv[1] = res.y;
					}

					if (sampleColors) {
						vert.color_32 = precol.Bernstein3D(tile_u, bv).ToRGBA();
					} else {
						vert.color_32 = patch.defcolor;
					}
				}
			}

			int patch_index = patch_v * num_patches_u + patch_u;
			int total = patch_index * (tess_u + 1) * (tess_v + 1);
			BuildIndex(indices + count, count, tess_u, tess_v, patch.primType, total);

			dest += (tess_u + 1) * (tess_v + 1) * sizeof(SimpleVertex);
		}
	}
}

// Prepare mesh of one patch for "Instanced Tessellation".
static void TessellateBezierPatchHardware(u8 *&dest, u16 *indices, int &count, int tess_u, int tess_v, GEPatchPrimType primType) {
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	float inv_u = 1.0f / (float)tess_u;
	float inv_v = 1.0f / (float)tess_v;

	// Generating simple input vertices for the bezier-computing vertex shader.
	for (int tile_v = 0; tile_v < tess_v + 1; ++tile_v) {
		for (int tile_u = 0; tile_u < tess_u + 1; ++tile_u) {
			SimpleVertex &vert = vertices[tile_v * (tess_u + 1) + tile_u];

			vert.pos.x = (float)tile_u * inv_u;
			vert.pos.y = (float)tile_v * inv_v;
		}
	}

	BuildIndex(indices, count, tess_u, tess_v, primType);
}

void TessellateBezierPatch(u8 *&dest, u16 *&indices, int &count, int tess_u, int tess_v, const BezierPatch &patch, u32 origVertType) {
	switch (g_Config.iSplineBezierQuality) {
	case LOW_QUALITY:
		_BezierPatchHighQuality(dest, indices, count, 2, 2, patch, origVertType);
		break;
	case MEDIUM_QUALITY:
		_BezierPatchHighQuality(dest, indices, count, std::max(tess_u / 2, 1), std::max(tess_v / 2, 1), patch, origVertType);
		break;
	case HIGH_QUALITY:
		_BezierPatchHighQuality(dest, indices, count, tess_u, tess_v, patch, origVertType);
		break;
	}
}

static void CopyControlPoints(const SimpleVertex *const *points, float *pos, float *tex, float *col, int posStride, int texStride, int colStride, int size, bool hasColor, bool hasTexCoords) {
	for (int idx = 0; idx < size; idx++) {
		memcpy(pos, points[idx]->pos.AsArray(), 3 * sizeof(float));
		pos += posStride;
		if (hasTexCoords) {
			memcpy(tex, points[idx]->uv, 2 * sizeof(float));
			tex += texStride;
		}
		if (hasColor) {
			memcpy(col, Vec4f::FromRGBA(points[idx]->color_32).AsArray(), 4 * sizeof(float));
			col += colStride;
		}
	}
	if (!hasColor)
		memcpy(col, Vec4f::FromRGBA(points[0]->color_32).AsArray(), 4 * sizeof(float));
}

class SimpleBufferManager {
private:
	u8 *buf_;
	size_t totalSize, maxSize_;
public:
	SimpleBufferManager(u8 *buf, size_t maxSize)
		: buf_(buf), totalSize(0), maxSize_(maxSize) {}

	u8 *Allocate(size_t size) {
		size = (size + 15) & ~15; // Align for 16 bytes

		if ((totalSize + size) > maxSize_)
			return nullptr; // No more memory

		size_t tmp = totalSize;
		totalSize += size;
		return buf_ + tmp;
	}
};

// This maps GEPatchPrimType to GEPrimitiveType.
const GEPrimitiveType primType[] = { GE_PRIM_TRIANGLES, GE_PRIM_LINES, GE_PRIM_POINTS, GE_PRIM_POINTS };

void DrawEngineCommon::SubmitSpline(const void *control_points, const void *indices, int tess_u, int tess_v, int count_u, int count_v, int type_u, int type_v, GEPatchPrimType prim_type, bool computeNormals, bool patchFacing, u32 vertType, int *bytesRead) {
	PROFILE_THIS_SCOPE("spline");
	DispatchFlush();

	// Real hardware seems to draw nothing when given < 4 either U or V.
	if (count_u < 4 || count_v < 4)
		return;

	SimpleBufferManager managedBuf(decoded, DECODED_VERTEX_BUFFER_SIZE);

	u16 index_lower_bound = 0;
	u16 index_upper_bound = count_u * count_v - 1;
	IndexConverter ConvertIndex(vertType, indices);
	if (indices)
		GetIndexBounds(indices, count_u * count_v, vertType, &index_lower_bound, &index_upper_bound);

	VertexDecoder *origVDecoder = GetVertexDecoder((vertType & 0xFFFFFF) | (gstate.getUVGenMode() << 24));
	*bytesRead = count_u * count_v * origVDecoder->VertexSize();

	// Simplify away bones and morph before proceeding
	SimpleVertex *simplified_control_points = (SimpleVertex *)managedBuf.Allocate(sizeof(SimpleVertex) * (index_upper_bound + 1));
	u8 *temp_buffer = managedBuf.Allocate(sizeof(SimpleVertex) * count_u * count_v);

	u32 origVertType = vertType;
	vertType = NormalizeVertices((u8 *)simplified_control_points, temp_buffer, (u8 *)control_points, index_lower_bound, index_upper_bound, vertType);

	VertexDecoder *vdecoder = GetVertexDecoder(vertType);

	int vertexSize = vdecoder->VertexSize();
	if (vertexSize != sizeof(SimpleVertex)) {
		ERROR_LOG(G3D, "Something went really wrong, vertex size: %i vs %i", vertexSize, (int)sizeof(SimpleVertex));
	}

	// Make an array of pointers to the control points, to get rid of indices.
	const SimpleVertex **points = (const SimpleVertex **)managedBuf.Allocate(sizeof(SimpleVertex *) * count_u * count_v);
	for (int idx = 0; idx < count_u * count_v; idx++)
		points[idx] = simplified_control_points + (indices ? ConvertIndex(idx) : idx);

	int count = 0;
	u8 *dest = splineBuffer;

	SplinePatchLocal patch;
	patch.tess_u = tess_u;
	patch.tess_v = tess_v;
	patch.type_u = type_u;
	patch.type_v = type_v;
	patch.count_u = count_u;
	patch.count_v = count_v;
	patch.computeNormals = computeNormals;
	patch.primType = prim_type;
	patch.patchFacing = patchFacing;
	patch.defcolor = points[0]->color_32;

	if (CanUseHardwareTessellation(prim_type)) {
		tessDataTransfer->SendDataToShader(points, count_u * count_v, origVertType);
		TessellateSplinePatchHardware(dest, quadIndices_, count, patch);
		numPatches = (count_u - 3) * (count_v - 3);
	} else {
		patch.pos = (Vec3f *)managedBuf.Allocate(sizeof(Vec3f) * count_u * count_v);
		patch.tex = (Vec2f *)managedBuf.Allocate(sizeof(Vec2f) * count_u * count_v);
		patch.col = (Vec4f *)managedBuf.Allocate(sizeof(Vec4f) * count_u * count_v);
		for (int idx = 0; idx < count_u * count_v; idx++) {
			patch.pos[idx] = Vec3f(points[idx]->pos);
			patch.tex[idx] = Vec2f(points[idx]->uv);
			patch.col[idx] = Vec4f::FromRGBA(points[idx]->color_32);
		}
		int maxVertexCount = SPLINE_BUFFER_SIZE / vertexSize;
		TessellateSplinePatch(dest, quadIndices_, count, patch, origVertType, maxVertexCount);
	}

	u32 vertTypeWithIndex16 = (vertType & ~GE_VTYPE_IDX_MASK) | GE_VTYPE_IDX_16BIT;

	UVScale prevUVScale;
	if ((origVertType & GE_VTYPE_TC_MASK) != 0) {
		// We scaled during Normalize already so let's turn it off when drawing.
		prevUVScale = gstate_c.uv;
		gstate_c.uv.uScale = 1.0f;
		gstate_c.uv.vScale = 1.0f;
		gstate_c.uv.uOff = 0.0f;
		gstate_c.uv.vOff = 0.0f;
	}

	uint32_t vertTypeID = GetVertTypeID(vertTypeWithIndex16, gstate.getUVGenMode());

	int generatedBytesRead;
	DispatchSubmitPrim(splineBuffer, quadIndices_, PatchPrimToPrim(prim_type), count, vertTypeID, &generatedBytesRead);

	DispatchFlush();

	if ((origVertType & GE_VTYPE_TC_MASK) != 0) {
		gstate_c.uv = prevUVScale;
	}
}

void DrawEngineCommon::SubmitBezier(const void *control_points, const void *indices, int tess_u, int tess_v, int count_u, int count_v, GEPatchPrimType prim_type, bool computeNormals, bool patchFacing, u32 vertType, int *bytesRead) {
	PROFILE_THIS_SCOPE("bezier");
	DispatchFlush();

	// Real hardware seems to draw nothing when given < 4 either U or V.
	// This would result in num_patches_u / num_patches_v being 0.
	if (count_u < 4 || count_v < 4)
		return;

	SimpleBufferManager managedBuf(decoded, DECODED_VERTEX_BUFFER_SIZE);

	u16 index_lower_bound = 0;
	u16 index_upper_bound = count_u * count_v - 1;
	IndexConverter ConvertIndex(vertType, indices);
	if (indices)
		GetIndexBounds(indices, count_u*count_v, vertType, &index_lower_bound, &index_upper_bound);

	VertexDecoder *origVDecoder = GetVertexDecoder((vertType & 0xFFFFFF) | (gstate.getUVGenMode() << 24));
	*bytesRead = count_u * count_v * origVDecoder->VertexSize();

	// Simplify away bones and morph before proceeding
	// There are normally not a lot of control points so just splitting decoded should be reasonably safe, although not great.
	SimpleVertex *simplified_control_points = (SimpleVertex *)managedBuf.Allocate(sizeof(SimpleVertex) * (index_upper_bound + 1));
	u8 *temp_buffer = managedBuf.Allocate(sizeof(SimpleVertex) * count_u * count_v);

	u32 origVertType = vertType;
	vertType = NormalizeVertices((u8 *)simplified_control_points, temp_buffer, (u8 *)control_points, index_lower_bound, index_upper_bound, vertType);

	VertexDecoder *vdecoder = GetVertexDecoder(vertType);

	int vertexSize = vdecoder->VertexSize();
	if (vertexSize != sizeof(SimpleVertex)) {
		ERROR_LOG(G3D, "Something went really wrong, vertex size: %i vs %i", vertexSize, (int)sizeof(SimpleVertex));
	}

	// If specified as 0, uses 1.
	if (tess_u < 1) tess_u = 1;
	if (tess_v < 1) tess_v = 1;

	// Make an array of pointers to the control points, to get rid of indices.
	const SimpleVertex **points = (const SimpleVertex **)managedBuf.Allocate(sizeof(SimpleVertex *) * count_u * count_v);
	for (int idx = 0; idx < count_u * count_v; idx++)
		points[idx] = simplified_control_points + (indices ? ConvertIndex(idx) : idx);

	int count = 0;
	u8 *dest = splineBuffer;
	u16 *inds = quadIndices_;

	// Bezier patches share less control points than spline patches. Otherwise they are pretty much the same (except bezier don't support the open/close thing)
	int num_patches_u = (count_u - 1) / 3;
	int num_patches_v = (count_v - 1) / 3;
	if (CanUseHardwareTessellation(prim_type)) {
		tessDataTransfer->SendDataToShader(points, count_u * count_v, origVertType);
		TessellateBezierPatchHardware(dest, inds, count, tess_u, tess_v, prim_type);
		numPatches = num_patches_u * num_patches_v;
	} else {
		BezierPatch patch;
		patch.count_u = count_u;
		patch.count_v = count_v;
		patch.primType = prim_type;
		patch.computeNormals = computeNormals;
		patch.patchFacing = patchFacing;
		patch.defcolor = points[0]->color_32;
		patch.pos = (Vec3f *)managedBuf.Allocate(sizeof(Vec3f) * count_u * count_v);
		patch.tex = (Vec2f *)managedBuf.Allocate(sizeof(Vec2f) * count_u * count_v);
		patch.col = (Vec4f *)managedBuf.Allocate(sizeof(Vec4f) * count_u * count_v);
		for (int idx = 0; idx < count_u * count_v; idx++) {
			patch.pos[idx] = Vec3f(points[idx]->pos);
			patch.tex[idx] = Vec2f(points[idx]->uv);
			patch.col[idx] = Vec4f::FromRGBA(points[idx]->color_32);
		}
		int maxVertices = SPLINE_BUFFER_SIZE / vertexSize;
		// Downsample until it fits, in case crazy tessellation factors are sent.
		while ((tess_u + 1) * (tess_v + 1) * num_patches_u * num_patches_v > maxVertices) {
			tess_u /= 2;
			tess_v /= 2;
		}
		TessellateBezierPatch(dest, inds, count, tess_u, tess_v, patch, origVertType);
	}

	u32 vertTypeWithIndex16 = (vertType & ~GE_VTYPE_IDX_MASK) | GE_VTYPE_IDX_16BIT;

	UVScale prevUVScale;
	if (origVertType & GE_VTYPE_TC_MASK) {
		// We scaled during Normalize already so let's turn it off when drawing.
		prevUVScale = gstate_c.uv;
		gstate_c.uv.uScale = 1.0f;
		gstate_c.uv.vScale = 1.0f;
		gstate_c.uv.uOff = 0;
		gstate_c.uv.vOff = 0;
	}

	uint32_t vertTypeID = GetVertTypeID(vertTypeWithIndex16, gstate.getUVGenMode());
	int generatedBytesRead;
	DispatchSubmitPrim(splineBuffer, quadIndices_, PatchPrimToPrim(prim_type), count, vertTypeID, &generatedBytesRead);

	DispatchFlush();

	if (origVertType & GE_VTYPE_TC_MASK) {
		gstate_c.uv = prevUVScale;
	}
}

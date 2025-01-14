// Copyright (c) 2012- PPSSPP Project.

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

#include <map>
#include <vector>
#include <mutex>

#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Serialize/SerializeList.h"
#include "Common/Serialize/SerializeMap.h"
#include "Common/Data/Collections/ThreadSafeList.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/MIPS/MIPS.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/MemMapHelpers.h"
#include "Core/Reporting.h"
#include "Core/System.h"
#include "Core/HLE/sceGe.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HLE/KernelWaitHelpers.h"
#include "GPU/GPUState.h"
#include "GPU/GPUInterface.h"

static const int LIST_ID_MAGIC = 0x35000000;

static PspGeCallbackData ge_callback_data[16];
static bool ge_used_callbacks[16] = {0};

typedef std::vector<SceUID> WaitingThreadList;
static std::map<int, WaitingThreadList> listWaitingThreads;
static WaitingThreadList drawWaitingThreads;

struct GeInterruptData {
	int listid;
	u32 pc;
	u32 cmd;
};

static ThreadSafeList<GeInterruptData> ge_pending_cb;
static int geSyncEvent;
static int geInterruptEvent;
static int geCycleEvent;

class GeIntrHandler : public IntrHandler {
public:
	GeIntrHandler() : IntrHandler(PSP_GE_INTR) {}

	bool run(PendingInterrupt& pend) override {
		if (ge_pending_cb.empty()) {
			ERROR_LOG_REPORT(SCEGE, "Unable to run GE interrupt: no pending interrupt");
			return false;
		}

		GeInterruptData intrdata = ge_pending_cb.front();
		DisplayList* dl = gpu->getList(intrdata.listid);

		if (dl == NULL) {
			WARN_LOG(SCEGE, "Unable to run GE interrupt: list doesn't exist: %d", intrdata.listid);
			return false;
		}

		if (!dl->interruptsEnabled) {
			ERROR_LOG_REPORT(SCEGE, "Unable to run GE interrupt: list has interrupts disabled, should not happen");
			return false;
		}

		gpu->InterruptStart(intrdata.listid);

		const u32 cmd = intrdata.cmd;
		int subintr = -1;
		if (dl->subIntrBase >= 0) {
			switch (dl->signal) {
			case PSP_GE_SIGNAL_SYNC:
			case PSP_GE_SIGNAL_JUMP:
			case PSP_GE_SIGNAL_CALL:
			case PSP_GE_SIGNAL_RET:
				// Do nothing.
				break;

			case PSP_GE_SIGNAL_HANDLER_PAUSE:
				if (cmd == GE_CMD_FINISH)
					subintr = dl->subIntrBase | PSP_GE_SUBINTR_SIGNAL;
				break;

			default:
				if (cmd == GE_CMD_SIGNAL)
					subintr = dl->subIntrBase | PSP_GE_SUBINTR_SIGNAL;
				else
					subintr = dl->subIntrBase | PSP_GE_SUBINTR_FINISH;
				break;
			}
		}

		// Set the list as complete once the interrupt starts.
		// In other words, not before another interrupt finishes.
		if (dl->signal != PSP_GE_SIGNAL_HANDLER_PAUSE && cmd == GE_CMD_FINISH) {
			dl->state = PSP_GE_DL_STATE_COMPLETED;
		}

		SubIntrHandler* handler = get(subintr);
		if (handler != NULL) {
			DEBUG_LOG(CPU, "Entering GE interrupt handler %08x", handler->handlerAddress);
			currentMIPS->pc = handler->handlerAddress;
			u32 data = dl->subIntrToken;
			currentMIPS->r[MIPS_REG_A0] = data & 0xFFFF;
			currentMIPS->r[MIPS_REG_A1] = handler->handlerArg;
			currentMIPS->r[MIPS_REG_A2] = sceKernelGetCompiledSdkVersion() <= 0x02000010 ? 0 : intrdata.pc + 4;
			// RA is already taken care of in __RunOnePendingInterrupt

			return true;
		}

		if (dl->signal == PSP_GE_SIGNAL_HANDLER_SUSPEND) {
			if (sceKernelGetCompiledSdkVersion() <= 0x02000010) {
				if (dl->state != PSP_GE_DL_STATE_NONE && dl->state != PSP_GE_DL_STATE_COMPLETED) {
					dl->state = PSP_GE_DL_STATE_QUEUED;
				}
			}
		}

		ge_pending_cb.pop_front();
		gpu->InterruptEnd(intrdata.listid);

		// Seen in GoW.
		if (subintr >= 0)
			DEBUG_LOG(SCEGE, "Ignoring interrupt for display list %d, already been released.", intrdata.listid);
		return false;
	}

	void handleResult(PendingInterrupt& pend) override {
		GeInterruptData intrdata = ge_pending_cb.front();
		ge_pending_cb.pop_front();

		DisplayList* dl = gpu->getList(intrdata.listid);
		if (!dl->interruptsEnabled) {
			ERROR_LOG_REPORT(SCEGE, "Unable to finish GE interrupt: list has interrupts disabled, should not happen");
			return;
		}

		switch (dl->signal) {
		case PSP_GE_SIGNAL_HANDLER_SUSPEND:
			if (sceKernelGetCompiledSdkVersion() <= 0x02000010) {
				// uofw says dl->state = endCmd & 0xFF;
				DisplayListState newState = static_cast<DisplayListState>(Memory::ReadUnchecked_U32(intrdata.pc - 4) & 0xFF);
				//dl->status = static_cast<DisplayListStatus>(Memory::ReadUnchecked_U32(intrdata.pc) & 0xFF);
				//if(dl->status < 0 || dl->status > PSP_GE_LIST_PAUSED)
				//	ERROR_LOG(SCEGE, "Weird DL status after signal suspend %x", dl->status);
				if (newState != PSP_GE_DL_STATE_RUNNING) {
					DEBUG_LOG_REPORT(SCEGE, "GE Interrupt: newState might be %d", newState);
				}

				if (dl->state != PSP_GE_DL_STATE_NONE && dl->state != PSP_GE_DL_STATE_COMPLETED) {
					dl->state = PSP_GE_DL_STATE_QUEUED;
				}
			}
			break;
		default:
			break;
		}

		gpu->InterruptEnd(intrdata.listid);
	}
};

static void __GeExecuteSync(u64 userdata, int cyclesLate) {
	int listid = userdata >> 32;
	GPUSyncType type = (GPUSyncType) (userdata & 0xFFFFFFFF);
	bool wokeThreads = __GeTriggerWait(type, listid);
	gpu->SyncEnd(type, listid, wokeThreads);
}

static void __GeExecuteInterrupt(u64 userdata, int cyclesLate) {
	__TriggerInterrupt(PSP_INTR_IMMEDIATE, PSP_GE_INTR, PSP_INTR_SUB_NONE);
}

static void __GeCheckCycles(u64 userdata, int cyclesLate) {
	// Deprecated
}

void __GeInit() {
	memset(&ge_used_callbacks, 0, sizeof(ge_used_callbacks));
	memset(&ge_callback_data, 0, sizeof(ge_callback_data));
	ge_pending_cb.clear();
	__RegisterIntrHandler(PSP_GE_INTR, new GeIntrHandler());

	geSyncEvent = CoreTiming::RegisterEvent("GeSyncEvent", &__GeExecuteSync);
	geInterruptEvent = CoreTiming::RegisterEvent("GeInterruptEvent", &__GeExecuteInterrupt);

	// Deprecated
	geCycleEvent = CoreTiming::RegisterEvent("GeCycleEvent", &__GeCheckCycles);

	listWaitingThreads.clear();
	drawWaitingThreads.clear();
}

struct GeInterruptData_v1 {
	int listid;
	u32 pc;
};

void __GeDoState(PointerWrap &p) {
	auto s = p.Section("sceGe", 1, 2);
	if (!s)
		return;

	DoArray(p, ge_callback_data, ARRAY_SIZE(ge_callback_data));
	DoArray(p, ge_used_callbacks, ARRAY_SIZE(ge_used_callbacks));

	if (s >= 2) {
		Do(p, ge_pending_cb);
	} else {
		std::list<GeInterruptData_v1> old;
		Do(p, old);
		ge_pending_cb.clear();
		for (auto it = old.begin(), end = old.end(); it != end; ++it) {
			GeInterruptData intrdata = {it->listid, it->pc};
			intrdata.cmd = Memory::ReadUnchecked_U32(it->pc - 4) >> 24;
			ge_pending_cb.push_back(intrdata);
		}
	}

	Do(p, geSyncEvent);
	CoreTiming::RestoreRegisterEvent(geSyncEvent, "GeSyncEvent", &__GeExecuteSync);
	Do(p, geInterruptEvent);
	CoreTiming::RestoreRegisterEvent(geInterruptEvent, "GeInterruptEvent", &__GeExecuteInterrupt);
	Do(p, geCycleEvent);
	CoreTiming::RestoreRegisterEvent(geCycleEvent, "GeCycleEvent", &__GeCheckCycles);

	Do(p, listWaitingThreads);
	Do(p, drawWaitingThreads);

	// Everything else is done in sceDisplay.
}

void __GeShutdown() {
}

bool __GeTriggerSync(GPUSyncType type, int id, u64 atTicks) {
	u64 userdata = (u64)id << 32 | (u64)type;
	s64 future = atTicks - CoreTiming::GetTicks();
	if (type == GPU_SYNC_DRAW) {
		s64 left = CoreTiming::UnscheduleEvent(geSyncEvent, userdata);
		if (left > future)
			future = left;
	}
	CoreTiming::ScheduleEvent(future, geSyncEvent, userdata);
	return true;
}

bool __GeTriggerInterrupt(int listid, u32 pc, u64 atTicks) {
	GeInterruptData intrdata;
	intrdata.listid = listid;
	intrdata.pc = pc;
	intrdata.cmd = Memory::ReadUnchecked_U32(pc - 4) >> 24;

	ge_pending_cb.push_back(intrdata);

	u64 userdata = (u64)listid << 32 | (u64) pc;
	CoreTiming::ScheduleEvent(atTicks - CoreTiming::GetTicks(), geInterruptEvent, userdata);
	return true;
}

void __GeWaitCurrentThread(GPUSyncType type, SceUID waitId, const char *reason) {
	WaitType waitType;
	if (type == GPU_SYNC_DRAW) {
		drawWaitingThreads.push_back(__KernelGetCurThread());
		waitType = WAITTYPE_GEDRAWSYNC;
	} else if (type == GPU_SYNC_LIST) {
		listWaitingThreads[waitId].push_back(__KernelGetCurThread());
		waitType = WAITTYPE_GELISTSYNC;
	} else {
		ERROR_LOG_REPORT(SCEGE, "__GeWaitCurrentThread: bad wait type");
		return;
	}

	__KernelWaitCurThread(waitType, waitId, 0, 0, false, reason);
}

static bool __GeTriggerWait(WaitType waitType, SceUID waitId, WaitingThreadList &waitingThreads) {
	// TODO: Do they ever get a result other than 0?
	bool wokeThreads = false;
	for (auto it = waitingThreads.begin(), end = waitingThreads.end(); it != end; ++it)
		wokeThreads |= HLEKernel::ResumeFromWait(*it, waitType, waitId, 0);
	waitingThreads.clear();
	return wokeThreads;
}

bool __GeTriggerWait(GPUSyncType type, SceUID waitId) {
	// We check for the old type for old savestate compatibility.
	if (type == GPU_SYNC_DRAW || (WaitType)type == WAITTYPE_GEDRAWSYNC)
		return __GeTriggerWait(WAITTYPE_GEDRAWSYNC, waitId, drawWaitingThreads);
	else if (type == GPU_SYNC_LIST || (WaitType)type == WAITTYPE_GELISTSYNC)
		return __GeTriggerWait(WAITTYPE_GELISTSYNC, waitId, listWaitingThreads[waitId]);
	else
		ERROR_LOG_REPORT(SCEGE, "__GeTriggerWait: bad wait type");
	return false;
}

static u32 sceGeEdramGetAddr() {
	u32 retVal = 0x04000000;
	DEBUG_LOG(SCEGE, "%08x = sceGeEdramGetAddr", retVal);
	hleEatCycles(150);
	return retVal;
}

// TODO: Return a different value for the PS3 enhanced-emulator games?
static u32 sceGeEdramGetSize() {
	const u32 retVal = 0x00200000;
	DEBUG_LOG(SCEGE, "%08x = sceGeEdramGetSize()", retVal);
	return retVal;
}

static int __GeSubIntrBase(int callbackId) {
	return callbackId * 2;
}

u32 sceGeListEnQueue(u32 listAddress, u32 stallAddress, int callbackId, u32 optParamAddr) {
	DEBUG_LOG(SCEGE,
			"sceGeListEnQueue(addr=%08x, stall=%08x, cbid=%08x, param=%08x)",
			listAddress, stallAddress, callbackId, optParamAddr);
	auto optParam = PSPPointer<PspGeListArgs>::Create(optParamAddr);

	u32 listID = gpu->EnqueueList(listAddress, stallAddress, __GeSubIntrBase(callbackId), optParam, false);
	if ((int)listID >= 0)
		listID = LIST_ID_MAGIC ^ listID;

	hleEatCycles(490);
	CoreTiming::ForceCheck();
	return hleLogSuccessX(SCEGE, listID);
}

u32 sceGeListEnQueueHead(u32 listAddress, u32 stallAddress, int callbackId, u32 optParamAddr) {
	DEBUG_LOG(SCEGE,
			"sceGeListEnQueueHead(addr=%08x, stall=%08x, cbid=%08x, param=%08x)",
			listAddress, stallAddress, callbackId, optParamAddr);
	auto optParam = PSPPointer<PspGeListArgs>::Create(optParamAddr);

	u32 listID = gpu->EnqueueList(listAddress, stallAddress, __GeSubIntrBase(callbackId), optParam, true);
	if ((int)listID >= 0)
		listID = LIST_ID_MAGIC ^ listID;

	hleEatCycles(480);
	CoreTiming::ForceCheck();
	return hleLogSuccessX(SCEGE, listID);
}

static int sceGeListDeQueue(u32 listID) {
	WARN_LOG(SCEGE, "sceGeListDeQueue(%08x)", listID);
	int result = gpu->DequeueList(LIST_ID_MAGIC ^ listID);
	hleReSchedule("dlist dequeued");
	return result;
}

static int sceGeListUpdateStallAddr(u32 displayListID, u32 stallAddress) {
	// Advance() might cause an interrupt, so defer the Advance but do it ASAP.
	// Final Fantasy Type-0 has a graphical artifact without this (timing issue.)
	hleEatCycles(190);
	CoreTiming::ForceCheck();

	DEBUG_LOG(SCEGE, "sceGeListUpdateStallAddr(dlid=%i, stalladdr=%08x)", displayListID, stallAddress);
	return gpu->UpdateStall(LIST_ID_MAGIC ^ displayListID, stallAddress);
}

// 0 : wait for completion. 1:check and return
int sceGeListSync(u32 displayListID, u32 mode) {
	DEBUG_LOG(SCEGE, "sceGeListSync(dlid=%08x, mode=%08x)", displayListID, mode);
	return gpu->ListSync(LIST_ID_MAGIC ^ displayListID, mode);
}

static u32 sceGeDrawSync(u32 mode) {
	//wait/check entire drawing state
	if (PSP_CoreParameter().compat.flags().DrawSyncEatCycles)
		hleEatCycles(500000); //HACK(?) : Potential fix for Crash Tag Team Racing and a few Gundam games
	else
		hleEatCycles(1240);
	DEBUG_LOG(SCEGE, "sceGeDrawSync(mode=%d)  (0=wait for completion, 1=peek)", mode);
	return gpu->DrawSync(mode);
}

int sceGeContinue() {
	DEBUG_LOG(SCEGE, "sceGeContinue");
	int ret = gpu->Continue();
	hleEatCycles(220);
	hleReSchedule("ge continue");
	return ret;
}

static int sceGeBreak(u32 mode, u32 unknownPtr) {
	if (mode > 1) {
		WARN_LOG(SCEGE, "sceGeBreak(mode=%d, unknown=%08x): invalid mode", mode, unknownPtr);
		return SCE_KERNEL_ERROR_INVALID_MODE;
	}
	// Not sure what this is supposed to be for...
	if ((int)unknownPtr < 0 || (int)(unknownPtr + 16) < 0) {
		WARN_LOG_REPORT(SCEGE, "sceGeBreak(mode=%d, unknown=%08x): invalid ptr", mode, unknownPtr);
		return SCE_KERNEL_ERROR_PRIV_REQUIRED;
	} else if (unknownPtr != 0) {
		WARN_LOG_REPORT(SCEGE, "sceGeBreak(mode=%d, unknown=%08x): unknown ptr (%s)", mode, unknownPtr, Memory::IsValidAddress(unknownPtr) ? "valid" : "invalid");
	}

	//mode => 0 : current dlist 1: all drawing
	DEBUG_LOG(SCEGE, "sceGeBreak(mode=%d, unknown=%08x)", mode, unknownPtr);
	int result = gpu->Break(mode);
	if (result >= 0 && mode == 0) {
		return LIST_ID_MAGIC ^ result;
	}
	return result;
}

static u32 sceGeSetCallback(u32 structAddr) {
	DEBUG_LOG(SCEGE, "sceGeSetCallback(struct=%08x)", structAddr);

	int cbID = -1;
	for (size_t i = 0; i < ARRAY_SIZE(ge_used_callbacks); ++i) {
		if (!ge_used_callbacks[i]) {
			cbID = (int) i;
			break;
		}
	}

	if (cbID == -1) {
		WARN_LOG(SCEGE, "sceGeSetCallback(): out of callback ids");
		return SCE_KERNEL_ERROR_OUT_OF_MEMORY;
	}

	ge_used_callbacks[cbID] = true;
	Memory::ReadStruct(structAddr, &ge_callback_data[cbID]);

	int subIntrBase = __GeSubIntrBase(cbID);

	if (ge_callback_data[cbID].finish_func != 0) {
		sceKernelRegisterSubIntrHandler(PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_FINISH,
				ge_callback_data[cbID].finish_func, ge_callback_data[cbID].finish_arg);
		sceKernelEnableSubIntr(PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_FINISH);
	}
	if (ge_callback_data[cbID].signal_func != 0) {
		sceKernelRegisterSubIntrHandler(PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_SIGNAL,
				ge_callback_data[cbID].signal_func, ge_callback_data[cbID].signal_arg);
		sceKernelEnableSubIntr(PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_SIGNAL);
	}

	return cbID;
}

static int sceGeUnsetCallback(u32 cbID) {
	DEBUG_LOG(SCEGE, "sceGeUnsetCallback(cbid=%08x)", cbID);

	if (cbID >= ARRAY_SIZE(ge_used_callbacks)) {
		WARN_LOG(SCEGE, "sceGeUnsetCallback(cbid=%08x): invalid callback id", cbID);
		return SCE_KERNEL_ERROR_INVALID_ID;
	}

	if (ge_used_callbacks[cbID]) {
		int subIntrBase = __GeSubIntrBase(cbID);

		sceKernelReleaseSubIntrHandler(PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_FINISH);
		sceKernelReleaseSubIntrHandler(PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_SIGNAL);
	} else {
		WARN_LOG(SCEGE, "sceGeUnsetCallback(cbid=%08x): ignoring unregistered callback id", cbID);
	}

	ge_used_callbacks[cbID] = false;
	return 0;
}

// Points to 512 32-bit words, where we can probably layout the context however we want
// unless some insane game pokes it and relies on it...
u32 sceGeSaveContext(u32 ctxAddr) {
	DEBUG_LOG(SCEGE, "sceGeSaveContext(%08x)", ctxAddr);

	if (gpu->BusyDrawing()) {
		WARN_LOG(SCEGE, "sceGeSaveContext(%08x): lists in process, aborting", ctxAddr);
		// Real error code.
		return -1;
	}

	// Let's just dump gstate.
	if (Memory::IsValidAddress(ctxAddr)) {
		gstate.Save((u32_le *)Memory::GetPointer(ctxAddr));
	}

	// This action should probably be pushed to the end of the queue of the display thread -
	// when we have one.
	return 0;
}

u32 sceGeRestoreContext(u32 ctxAddr) {
	DEBUG_LOG(SCEGE, "sceGeRestoreContext(%08x)", ctxAddr);

	if (gpu->BusyDrawing()) {
		WARN_LOG(SCEGE, "sceGeRestoreContext(%08x): lists in process, aborting", ctxAddr);
		return SCE_KERNEL_ERROR_BUSY;
	}

	if (Memory::IsValidAddress(ctxAddr)) {
		gstate.Restore((u32_le *)Memory::GetPointer(ctxAddr));
	}

	gpu->ReapplyGfxState();
	return 0;
}

static void __GeCopyMatrix(u32 matrixPtr, float *mtx, u32 size) {
	for (u32 i = 0; i < size / sizeof(float); ++i) {
		Memory::Write_U32(toFloat24(mtx[i]), matrixPtr + i * sizeof(float));
	}
}

static int sceGeGetMtx(int type, u32 matrixPtr) {
	if (!Memory::IsValidAddress(matrixPtr)) {
		ERROR_LOG(SCEGE, "sceGeGetMtx(%d, %08x) - bad matrix ptr", type, matrixPtr);
		return -1;
	}

	INFO_LOG(SCEGE, "sceGeGetMtx(%d, %08x)", type, matrixPtr);
	switch (type) {
	case GE_MTX_BONE0:
	case GE_MTX_BONE1:
	case GE_MTX_BONE2:
	case GE_MTX_BONE3:
	case GE_MTX_BONE4:
	case GE_MTX_BONE5:
	case GE_MTX_BONE6:
	case GE_MTX_BONE7:
		{
			int n = type - GE_MTX_BONE0;
			__GeCopyMatrix(matrixPtr, gstate.boneMatrix + n * 12, 12 * sizeof(float));
		}
		break;
	case GE_MTX_TEXGEN:
		__GeCopyMatrix(matrixPtr, gstate.tgenMatrix, 12 * sizeof(float));
		break;
	case GE_MTX_WORLD:
		__GeCopyMatrix(matrixPtr, gstate.worldMatrix, 12 * sizeof(float));
		break;
	case GE_MTX_VIEW:
		__GeCopyMatrix(matrixPtr, gstate.viewMatrix, 12 * sizeof(float));
		break;
	case GE_MTX_PROJECTION:
		__GeCopyMatrix(matrixPtr, gstate.projMatrix, 16 * sizeof(float));
		break;
	default:
		return SCE_KERNEL_ERROR_INVALID_INDEX;
	}
	return 0;
}

static u32 sceGeGetCmd(int cmd) {
	if (cmd >= 0 && cmd < (int)ARRAY_SIZE(gstate.cmdmem)) {
		// Does not mask away the high bits.
		return hleLogSuccessInfoX(SCEGE, gstate.cmdmem[cmd]);
	}
	return hleLogError(SCEGE, SCE_KERNEL_ERROR_INVALID_INDEX);
}

static int sceGeGetStack(int index, u32 stackPtr) {
	WARN_LOG_REPORT(SCEGE, "sceGeGetStack(%i, %08x)", index, stackPtr);
	return gpu->GetStack(index, stackPtr);
}

static u32 sceGeEdramSetAddrTranslation(int new_size) {
	bool outsideRange = new_size != 0 && (new_size < 0x200 || new_size > 0x1000);
	bool notPowerOfTwo = (new_size & (new_size - 1)) != 0;
	if (outsideRange || notPowerOfTwo) {
		WARN_LOG(SCEGE, "sceGeEdramSetAddrTranslation(%i): invalid value", new_size);
		return SCE_KERNEL_ERROR_INVALID_VALUE;
	}

	DEBUG_LOG(SCEGE, "sceGeEdramSetAddrTranslation(%i)", new_size);

	// TODO: This isn't safe. EDRamWidth should be global and saved.
	static int EDRamWidth = 0x400;
	int last = EDRamWidth;
	EDRamWidth = new_size;
	return last;
}

const HLEFunction sceGe_user[] = {
	{0XE47E40E4, &WrapU_V<sceGeEdramGetAddr>,            "sceGeEdramGetAddr",            'x', ""    },
	{0XAB49E76A, &WrapU_UUIU<sceGeListEnQueue>,          "sceGeListEnQueue",             'x', "xxip"},
	{0X1C0D95A6, &WrapU_UUIU<sceGeListEnQueueHead>,      "sceGeListEnQueueHead",         'x', "xxip"},
	{0XE0D68148, &WrapI_UU<sceGeListUpdateStallAddr>,    "sceGeListUpdateStallAddr",     'i', "xx"  },
	{0X03444EB4, &WrapI_UU<sceGeListSync>,               "sceGeListSync",                'i', "xx"  },
	{0XB287BD61, &WrapU_U<sceGeDrawSync>,                "sceGeDrawSync",                'x', "x"   },
	{0XB448EC0D, &WrapI_UU<sceGeBreak>,                  "sceGeBreak",                   'i', "xx"  },
	{0X4C06E472, &WrapI_V<sceGeContinue>,                "sceGeContinue",                'i', ""    },
	{0XA4FC06A4, &WrapU_U<sceGeSetCallback>,             "sceGeSetCallback",             'x', "x"   },
	{0X05DB22CE, &WrapI_U<sceGeUnsetCallback>,           "sceGeUnsetCallback",           'i', "x"   },
	{0X1F6752AD, &WrapU_V<sceGeEdramGetSize>,            "sceGeEdramGetSize",            'x', ""    },
	{0XB77905EA, &WrapU_I<sceGeEdramSetAddrTranslation>, "sceGeEdramSetAddrTranslation", 'x', "i"   },
	{0XDC93CFEF, &WrapU_I<sceGeGetCmd>,                  "sceGeGetCmd",                  'x', "i"   },
	{0X57C8945B, &WrapI_IU<sceGeGetMtx>,                 "sceGeGetMtx",                  'i', "ix"  },
	{0X438A385A, &WrapU_U<sceGeSaveContext>,             "sceGeSaveContext",             'x', "x"   },
	{0X0BF608FB, &WrapU_U<sceGeRestoreContext>,          "sceGeRestoreContext",          'x', "x"   },
	{0X5FB86AB0, &WrapI_U<sceGeListDeQueue>,             "sceGeListDeQueue",             'i', "x"   },
	{0XE66CB92E, &WrapI_IU<sceGeGetStack>,               "sceGeGetStack",                'i', "ix"  },
};

void Register_sceGe_user() {
	RegisterModule("sceGe_user", ARRAY_SIZE(sceGe_user), sceGe_user);
}

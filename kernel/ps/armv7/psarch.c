/*++

Copyright (c) 2013 Minoca Corp. All Rights Reserved

Module Name:

    psarch.c

Abstract:

    This module implements architecture specific functionality for the process
    and thread library.

Author:

    Evan Green 28-Mar-2013

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel.h>
#include <minoca/arm.h>
#include <minoca/dbgproto.h>
#include "../processp.h"

//
// ---------------------------------------------------------------- Definitions
//

//
// ------------------------------------------------------ Data Type Definitions
//

//
// ----------------------------------------------- Internal Function Prototypes
//

KSTATUS
PspArchGetNextPcReadMemory (
    PVOID Address,
    ULONG Size,
    PVOID Data
    );

//
// -------------------------------------------------------------------- Globals
//

//
// Define the initial architecture-specific contents of the thread pointer data
// for a newly created thread.
//

ULONGLONG PsInitialThreadPointer = 0;

//
// ------------------------------------------------------------------ Functions
//

ULONG
PsDispatchPendingSignalsOnCurrentThread (
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine dispatches any pending signals that should be run on the
    current thread.

Arguments:

    TrapFrame - Supplies a pointer to the current trap frame. If this trap frame
        is not destined for user mode, this function exits immediately.

Return Value:

    Returns a signal number if a signal was queued.

    -1 if no signal was dispatched.

--*/

{

    BOOL SignalHandled;
    ULONG SignalNumber;
    SIGNAL_PARAMETERS SignalParameters;

    //
    // If the trap frame is not destined for user mode, then forget it.
    //

    if (IS_TRAP_FRAME_FROM_PRIVILEGED_MODE(TrapFrame) != FALSE) {
        return -1;
    }

    do {
        SignalNumber = PspDequeuePendingSignal(&SignalParameters, TrapFrame);
        if (SignalNumber == -1) {
            return -1;
        }

        SignalHandled = PspSignalAttemptDefaultProcessing(SignalNumber);

    } while (SignalHandled != FALSE);

    PsApplySynchronousSignal(TrapFrame, &SignalParameters);
    return SignalNumber;
}

VOID
PsApplySynchronousSignal (
    PTRAP_FRAME TrapFrame,
    PSIGNAL_PARAMETERS SignalParameters
    )

/*++

Routine Description:

    This routine applies the given signal onto the current thread. It is
    required that no signal is already in progress, nor will any other signals
    be applied for the duration of the system call.

Arguments:

    TrapFrame - Supplies a pointer to the current trap frame. This trap frame
        must be destined for user mode.

    SignalParameters - Supplies a pointer to the signal information to apply.

Return Value:

    None.

--*/

{

    PKTHREAD Thread;

    ASSERT((TrapFrame->Cpsr & ARM_MODE_MASK) == ARM_MODE_USER);

    Thread = KeGetCurrentThread();

    ASSERT(Thread->SignalInProgress == FALSE);

    //
    // Copy the original trap frame into the saved area.
    //

    RtlCopyMemory(Thread->SavedSignalContext, TrapFrame, sizeof(TRAP_FRAME));

    //
    // Modify the trap frame to make the signal handler run. Shove the
    // parameters in registers to avoid having to write to user mode
    // memory.
    //

    TrapFrame->Pc = (ULONG)(Thread->OwningProcess->SignalHandlerRoutine);
    TrapFrame->R0 = SignalParameters->SignalNumber |
                    (SignalParameters->SignalCode <<
                     (sizeof(USHORT) * BITS_PER_BYTE));

    TrapFrame->R1 = SignalParameters->ErrorNumber;

    //
    // The faulting address, sending process, and band event parameters are
    // all unioned together.
    //

    TrapFrame->R2 = (UINTN)SignalParameters->FaultingAddress;
    TrapFrame->R3 = SignalParameters->SendingUserId;

    //
    // The value parameter and exit status are unioned together.
    //

    TrapFrame->R4 = SignalParameters->ValueParameter;
    Thread->SignalInProgress = TRUE;
    return;
}

VOID
PspRestorePreSignalTrapFrame (
    PKTHREAD Thread,
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine restores the original user mode thread context for the thread
    before the signal was invoked.

Arguments:

    Thread - Supplies a pointer to this thread.

    TrapFrame - Supplies a pointer to the trap frame generated by this jump
        from user mode to kernel mode.

Return Value:

    STATUS_SUCCESS on success.

    STATUS_NOT_HANDLED if a signal was not in process.

--*/

{

    UINTN OriginalSvcLink;
    UINTN OriginalSvcSp;

    ASSERT(Thread->SignalInProgress != FALSE);

    //
    // Copy the saved trap frame over the trap frame of this system call.
    // Avoid clobbering the SVC stack pointer and link.
    //

    OriginalSvcLink = TrapFrame->SvcLink;
    OriginalSvcSp = TrapFrame->SvcSp;
    RtlCopyMemory(TrapFrame, Thread->SavedSignalContext, sizeof(TRAP_FRAME));
    TrapFrame->SvcLink = OriginalSvcLink;
    TrapFrame->SvcSp = OriginalSvcSp;
    return;
}

VOID
PspPrepareThreadForFirstRun (
    PKTHREAD Thread,
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine performs any architecture specific initialization to prepare a
    thread for being context swapped for the first time.

Arguments:

    Thread - Supplies a pointer to the thread being prepared for its first run.

    TrapFrame - Supplies an optional pointer for the thread to restore on its
        first run.

Return Value:

    None.

--*/

{

    UINTN EntryPoint;
    ULONG Flags;
    PULONG StackPointer;
    PTRAP_FRAME StackTrapFrame;
    UINTN UserStackPointer;

    //
    // Get the initial stack pointer, and word align it.
    //

    StackPointer = Thread->KernelStack + Thread->KernelStackSize;

    //
    // Determine the appropriate flags value.
    //

    Flags = 0;
    if ((Thread->Flags & THREAD_FLAG_USER_MODE) != 0) {
        Flags |= ARM_MODE_USER;
        EntryPoint = (UINTN)Thread->ThreadRoutine;
        UserStackPointer = (UINTN)Thread->UserStack + Thread->UserStackSize;

    } else {
        Flags |= ARM_MODE_SVC;
        EntryPoint = (UINTN)PspKernelThreadStart;
        UserStackPointer = 0x66666666;
    }

    if (((UINTN)EntryPoint & ARM_THUMB_BIT) != 0) {
        Flags |= PSR_FLAG_THUMB;
    }

    //
    // Make room for a trap frame to be restored.
    //

    StackPointer = (PUINTN)((PUCHAR)StackPointer -
                            ALIGN_RANGE_UP(sizeof(TRAP_FRAME), 8));

    StackTrapFrame = (PTRAP_FRAME)StackPointer;
    if (TrapFrame != NULL) {
        RtlCopyMemory(StackTrapFrame, TrapFrame, sizeof(TRAP_FRAME));
        StackTrapFrame->SvcSp = (UINTN)StackPointer;

    } else {
        RtlZeroMemory(StackTrapFrame, sizeof(TRAP_FRAME));
        StackTrapFrame->SvcSp = (UINTN)StackPointer;
        StackTrapFrame->UserSp = (UINTN)UserStackPointer;
        StackTrapFrame->R0 = (UINTN)Thread->ThreadParameter;
        StackTrapFrame->Cpsr = Flags;
        StackTrapFrame->Pc = EntryPoint;
    }

    Thread->KernelStackPointer = StackPointer;
    return;
}

VOID
PspArchResetThreadContext (
    PKTHREAD Thread,
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine sets up the given trap frame as if the user mode portion of
    the thread was running for the first time.

Arguments:

    Thread - Supplies a pointer to the thread being reset.

    TrapFrame - Supplies a pointer where the initial thread's trap frame will
        be returned.

Return Value:

    None.

--*/

{

    ULONG OldSvcLink;
    ULONG OldSvcStackPointer;
    PUINTN UserStackPointer;

    UserStackPointer = Thread->UserStack + Thread->UserStackSize;
    OldSvcLink = TrapFrame->SvcLink;
    OldSvcStackPointer = TrapFrame->SvcSp;
    RtlZeroMemory(TrapFrame, sizeof(TRAP_FRAME));
    TrapFrame->SvcLink = OldSvcLink;
    TrapFrame->SvcSp = OldSvcStackPointer;
    TrapFrame->UserSp = (UINTN)UserStackPointer;
    TrapFrame->R0 = (UINTN)Thread->ThreadParameter;
    TrapFrame->Cpsr = ARM_MODE_USER;
    TrapFrame->Pc = (UINTN)Thread->ThreadRoutine;
    if ((TrapFrame->Pc & ARM_THUMB_BIT) != 0) {
        TrapFrame->Cpsr |= PSR_FLAG_THUMB;
    }

    return;
}

KSTATUS
PspArchGetDebugBreakInformation (
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine gets the current debug break information.

Arguments:

    TrapFrame - Supplies a pointer to the user mode trap frame that caused the
        break.

Return Value:

    Status code.

--*/

{

    PBREAK_NOTIFICATION Break;
    PPROCESS_DEBUG_DATA DebugData;
    PKPROCESS Process;
    PKTHREAD Thread;

    Thread = KeGetCurrentThread();
    Process = Thread->OwningProcess;
    DebugData = Process->DebugData;

    ASSERT(DebugData != NULL);
    ASSERT(DebugData->DebugLeaderThread == Thread);
    ASSERT(DebugData->DebugCommand.Command == DebugCommandGetBreakInformation);
    ASSERT(DebugData->DebugCommand.Size == sizeof(BREAK_NOTIFICATION));

    Break = DebugData->DebugCommand.Data;
    Break->Exception = ExceptionSignal;
    Break->ProcessorOrThreadNumber = Thread->ThreadId;
    Break->ProcessorOrThreadCount = Process->ThreadCount;
    Break->Process = Process->Identifiers.ProcessId;
    Break->ProcessorBlock = (UINTN)NULL;
    Break->ErrorCode = 0;
    Break->LoadedModuleCount = Process->ImageCount;
    Break->LoadedModuleSignature = Process->ImageListSignature;
    Break->InstructionPointer = TrapFrame->Pc;
    RtlZeroMemory(Break->InstructionStream, sizeof(Break->InstructionStream));
    MmCopyFromUserMode(Break->InstructionStream,
                       (PVOID)REMOVE_THUMB_BIT(TrapFrame->Pc),
                       ARM_INSTRUCTION_LENGTH);

    Break->Registers.Arm.R0 = TrapFrame->R0;
    Break->Registers.Arm.R1 = TrapFrame->R1;
    Break->Registers.Arm.R2 = TrapFrame->R2;
    Break->Registers.Arm.R3 = TrapFrame->R3;
    Break->Registers.Arm.R4 = TrapFrame->R4;
    Break->Registers.Arm.R5 = TrapFrame->R5;
    Break->Registers.Arm.R6 = TrapFrame->R6;
    Break->Registers.Arm.R7 = TrapFrame->R7;
    Break->Registers.Arm.R8 = TrapFrame->R8;
    Break->Registers.Arm.R9 = TrapFrame->R9;
    Break->Registers.Arm.R10 = TrapFrame->R10;
    Break->Registers.Arm.R11Fp = TrapFrame->R11;
    Break->Registers.Arm.R12Ip = TrapFrame->R12;
    Break->Registers.Arm.R13Sp = TrapFrame->UserSp;
    Break->Registers.Arm.R14Lr = TrapFrame->UserLink;
    Break->Registers.Arm.R15Pc = TrapFrame->Pc;
    Break->Registers.Arm.Cpsr = TrapFrame->Cpsr;
    return STATUS_SUCCESS;
}

KSTATUS
PspArchSetDebugBreakInformation (
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine sets the current debug break information, mostly just the
    register.

Arguments:

    TrapFrame - Supplies a pointer to the user mode trap frame that caused the
        break.

Return Value:

    Status code.

--*/

{

    PBREAK_NOTIFICATION Break;
    PPROCESS_DEBUG_DATA DebugData;
    PKPROCESS Process;
    PKTHREAD Thread;

    Thread = KeGetCurrentThread();
    Process = Thread->OwningProcess;
    DebugData = Process->DebugData;

    ASSERT(DebugData != NULL);
    ASSERT(DebugData->DebugLeaderThread == Thread);
    ASSERT(DebugData->DebugCommand.Command == DebugCommandSetBreakInformation);
    ASSERT(DebugData->DebugCommand.Size == sizeof(BREAK_NOTIFICATION));

    Break = DebugData->DebugCommand.Data;
    TrapFrame->R0 = Break->Registers.Arm.R0;
    TrapFrame->R1 = Break->Registers.Arm.R1;
    TrapFrame->R2 = Break->Registers.Arm.R2;
    TrapFrame->R3 = Break->Registers.Arm.R3;
    TrapFrame->R4 = Break->Registers.Arm.R4;
    TrapFrame->R5 = Break->Registers.Arm.R5;
    TrapFrame->R6 = Break->Registers.Arm.R6;
    TrapFrame->R7 = Break->Registers.Arm.R7;
    TrapFrame->R8 = Break->Registers.Arm.R8;
    TrapFrame->R9 = Break->Registers.Arm.R9;
    TrapFrame->R10 = Break->Registers.Arm.R10;
    TrapFrame->R11 = Break->Registers.Arm.R11Fp;
    TrapFrame->R12 = Break->Registers.Arm.R12Ip;
    TrapFrame->UserSp = Break->Registers.Arm.R13Sp;
    TrapFrame->UserLink = Break->Registers.Arm.R14Lr;
    TrapFrame->Pc = Break->Registers.Arm.R15Pc;
    TrapFrame->Cpsr = (Break->Registers.Arm.Cpsr & ~ARM_MODE_MASK) |
                      ARM_MODE_USER;

    return STATUS_SUCCESS;
}

KSTATUS
PspArchSetOrClearSingleStep (
    PTRAP_FRAME TrapFrame,
    BOOL Set
    )

/*++

Routine Description:

    This routine sets the current thread into single step mode.

Arguments:

    TrapFrame - Supplies a pointer to the user mode trap frame that caused the
        break.

    Set - Supplies a boolean indicating whether to set single step mode (TRUE)
        or clear single step mode (FALSE).

Return Value:

    Status code.

--*/

{

    PVOID Address;
    PVOID BreakingAddress;
    ULONG BreakInstruction;
    PPROCESS_DEBUG_DATA DebugData;
    BOOL FunctionReturning;
    ULONG Length;
    PVOID NextPc;
    PKPROCESS Process;
    KSTATUS Status;

    Process = PsGetCurrentProcess();
    DebugData = Process->DebugData;

    ASSERT(DebugData != NULL);

    Status = STATUS_SUCCESS;
    BreakingAddress = (PVOID)REMOVE_THUMB_BIT(TrapFrame->Pc);
    if ((TrapFrame->Cpsr & PSR_FLAG_THUMB) != 0) {
        BreakingAddress -= THUMB16_INSTRUCTION_LENGTH;

    } else {
        BreakingAddress -= ARM_INSTRUCTION_LENGTH;
    }

    //
    // Always clear the current single step address if there is one.
    //

    if (DebugData->DebugSingleStepAddress != NULL) {
        Address = (PVOID)REMOVE_THUMB_BIT(
                                   (UINTN)(DebugData->DebugSingleStepAddress));

        if (((UINTN)(DebugData->DebugSingleStepAddress) & ARM_THUMB_BIT) != 0) {
            Length = THUMB16_INSTRUCTION_LENGTH;

        } else {
            Length = ARM_INSTRUCTION_LENGTH;
        }

        //
        // If the debugger broke in because of the single step
        // breakpoint, set the PC back so the correct instruction gets
        // executed.
        //

        if (DebugData->DebugSingleStepAddress == BreakingAddress) {
            TrapFrame->Pc -= Length;
            ArBackUpIfThenState(TrapFrame);
        }

        Status = MmCopyToUserMode(DebugData->DebugSingleStepAddress,
                                  &(DebugData->DebugSingleStepOriginalContents),
                                  Length);

        if (Address == DebugData->DebugSingleStepAddress + Length) {
            TrapFrame->Pc -= Length;
            ArBackUpIfThenState(TrapFrame);
        }

        DebugData->DebugSingleStepAddress = NULL;
        if (!KSUCCESS(Status)) {
            return Status;
        }

        MmFlushInstructionCache(Address, Length);
    }

    //
    // Now set a new one if desired.
    //

    if (Set != FALSE) {

        ASSERT(DebugData->DebugSingleStepAddress == NULL);

        //
        // First determine where to put this new breakpoint.
        //

        Status = ArGetNextPc(TrapFrame,
                             PspArchGetNextPcReadMemory,
                             &FunctionReturning,
                             &NextPc);

        if (!KSUCCESS(Status)) {
            return Status;
        }

        Address = (PVOID)REMOVE_THUMB_BIT((UINTN)NextPc);
        if (((UINTN)NextPc & ARM_THUMB_BIT) != 0) {
            BreakInstruction = THUMB_BREAK_INSTRUCTION;
            Length = THUMB16_INSTRUCTION_LENGTH;

        } else {
            BreakInstruction = ARM_BREAK_INSTRUCTION;
            Length = ARM_INSTRUCTION_LENGTH;
        }

        //
        // Read the original contents of memory there so it can be put back.
        //

        Status = MmCopyFromUserMode(
                                 &(DebugData->DebugSingleStepOriginalContents),
                                 Address,
                                 Length);

        if (!KSUCCESS(Status)) {
            return Status;
        }

        //
        // Write the break instruction in there.
        //

        Status = MmCopyToUserMode(NextPc, &BreakInstruction, Length);
        if (!KSUCCESS(Status)) {
            return Status;
        }

        MmFlushInstructionCache(Address, Length);
        DebugData->DebugSingleStepAddress = NextPc;
        Status = STATUS_SUCCESS;
    }

    return Status;
}

//
// --------------------------------------------------------- Internal Functions
//

KSTATUS
PspArchGetNextPcReadMemory (
    PVOID Address,
    ULONG Size,
    PVOID Data
    )

/*++

Routine Description:

    This routine attempts to read memory on behalf of the function trying to
    figure out what the next instruction will be.

Arguments:

    Address - Supplies the virtual address that needs to be read.

    Size - Supplies the number of bytes to be read.

    Data - Supplies a pointer to the buffer where the read data will be
        returned on success.

Return Value:

    Status code. STATUS_SUCCESS will only be returned if all the requested
    bytes could be read.

--*/

{

    return MmCopyFromUserMode(Data, Address, Size);
}

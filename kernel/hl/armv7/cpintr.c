/*++

Copyright (c) 2012 Minoca Corp. All Rights Reserved

Module Name:

    cpintr.c

Abstract:

    This module implements interrupt controller support for the Integrator/CP
    board.

Author:

    Evan Green 22-Aug-2012

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel.h>
#include "integcp.h"

//
// ---------------------------------------------------------------- Definitions
//

//
// --------------------------------------------------------------------- Macros
//

//
// This macro reads from the Integrator/CP interrupt controller. The parameter
// should be CP_INTERRUPT_REGISTER value.
//

#define READ_INTERRUPT_REGISTER(_Register) \
    HlCpKernelServices->ReadRegister32(    \
                                (PULONG)HlCpInterruptController + (_Register))

//
// This macro writes to the Integrator/CP interrupt controller. _Register
// should be CP_INTERRUPT_REGISTER value, and _Value should be a ULONG.
//

#define WRITE_INTERRUPT_REGISTER(_Register, _Value) \
    HlCpKernelServices->WriteRegister32(            \
                               (PULONG)HlCpInterruptController + (_Register), \
                               (_Value))

//
// ----------------------------------------------- Internal Function Prototypes
//

KSTATUS
HlpCpInterruptInitializeIoUnit (
    PVOID Context
    );

INTERRUPT_CAUSE
HlpCpInterruptBegin (
    PVOID Context,
    PINTERRUPT_LINE FiringLine,
    PULONG MagicCandy
    );

VOID
HlpCpInterruptEndOfInterrupt (
    PVOID Context,
    ULONG MagicCandy
    );

KSTATUS
HlpCpInterruptRequestInterrupt (
    PVOID Context,
    PINTERRUPT_LINE Line,
    ULONG Vector,
    PINTERRUPT_HARDWARE_TARGET Target
    );

KSTATUS
HlpCpInterruptSetLineState (
    PVOID Context,
    PINTERRUPT_LINE Line,
    PINTERRUPT_LINE_STATE State
    );

KSTATUS
HlpCpInterruptDescribeLines (
    VOID
    );

//
// ------------------------------------------------------ Data Type Definitions
//

//
// Define the offsets to interrupt controller registers, in ULONGs.
//

typedef enum _CP_INTERRUPT_REGISTER {
    CpInterruptIrqStatus              = 0x0,
    CpInterruptIrqRawStatus           = 0x1,
    CpInterruptIrqEnable              = 0x2,
    CpInterruptIrqDisable             = 0x3,
    CpInterruptSoftwareInterruptSet   = 0x4,
    CpInterruptSoftwareInterruptClear = 0x5,
    CpInterruptFiqStatus              = 0x8,
    CpInterruptFiqRawStatus           = 0x9,
    CpInterruptFiqEnable              = 0xA,
    CpInterruptFiqDisable             = 0xB,
} CP_INTERRUPT_REGISTER, *PCP_INTERRUPT_REGISTER;

/*++

Structure Description:

    This structure describes the Integrator/CP private interrupt controller
    state.

Members:

    PhysicalAddress - Stores the physical address of this controller.

    LineRunLevel - Stores the run level for each interrupt line.

    RunLevelIrqMask - Stores the mask of interrupts to disable when an
        interrupt of each priority (run level) fires.

--*/

typedef struct _INTEGRATORCP_INTERRUPT_DATA {
    PHYSICAL_ADDRESS PhysicalAddress;
    RUNLEVEL LineRunLevel[INTEGRATORCP_INTERRUPT_LINE_COUNT];
    ULONG RunLevelIrqMask[MaxRunLevel];
} INTEGRATORCP_INTERRUPT_DATA, *PINTEGRATORCP_INTERRUPT_DATA;

//
// -------------------------------------------------------------------- Globals
//

//
// Store the virtual address of the mapped interrupt controller.
//

PVOID HlCpInterruptController = NULL;

//
// Store a pointer to the provided hardware layer services.
//

PHARDWARE_MODULE_KERNEL_SERVICES HlCpKernelServices = NULL;

//
// Store a pointer to the Integrator/CP ACPI table, if found.
//

PINTEGRATORCP_TABLE HlCpIntegratorTable = NULL;

//
// ------------------------------------------------------------------ Functions
//

VOID
HlpCpInterruptModuleEntry (
    PHARDWARE_MODULE_KERNEL_SERVICES Services
    )

/*++

Routine Description:

    This routine is the entry point for the Integrator/CP Interrupt hardware
    module. Its role is to detect and report the prescense of an Integrator/CP
    interrupt controller.

Arguments:

    Services - Supplies a pointer to the services/APIs made available by the
        kernel to the hardware module.

Return Value:

    None.

--*/

{

    PINTEGRATORCP_TABLE IntegratorTable;
    PINTEGRATORCP_INTERRUPT_DATA InterruptData;
    INTERRUPT_CONTROLLER_DESCRIPTION NewController;
    KSTATUS Status;

    //
    // Attempt to find the Integrator/CP ACPI table.
    //

    IntegratorTable = Services->GetAcpiTable(INTEGRATORCP_SIGNATURE, NULL);
    if (IntegratorTable == NULL) {
        goto IntegratorCpInterruptModuleEntryEnd;
    }

    HlCpIntegratorTable = IntegratorTable;
    HlCpKernelServices = Services;

    //
    // Zero out the controller description.
    //

    Services->ZeroMemory(&NewController,
                         sizeof(INTERRUPT_CONTROLLER_DESCRIPTION));

    if (IntegratorTable->InterruptControllerPhysicalAddress !=
                                                    INVALID_PHYSICAL_ADDRESS) {

        //
        // Allocate context needed for this Interrupt Controller.
        //

        InterruptData =
                   Services->AllocateMemory(sizeof(INTEGRATORCP_INTERRUPT_DATA),
                                            INTEGRATOR_ALLOCATION_TAG,
                                            FALSE,
                                            NULL);

        if (InterruptData == NULL) {
            goto IntegratorCpInterruptModuleEntryEnd;
        }

        Services->ZeroMemory(InterruptData,
                             sizeof(INTEGRATORCP_INTERRUPT_DATA));

        InterruptData->PhysicalAddress =
                           IntegratorTable->InterruptControllerPhysicalAddress;

        //
        // Initialize the new controller structure.
        //

        NewController.TableVersion = INTERRUPT_CONTROLLER_DESCRIPTION_VERSION;
        NewController.FunctionTable.EnumerateProcessors = NULL;
        NewController.FunctionTable.InitializeLocalUnit = NULL;
        NewController.FunctionTable.InitializeIoUnit =
                                                HlpCpInterruptInitializeIoUnit;

        NewController.FunctionTable.SetLocalUnitAddressing = NULL;
        NewController.FunctionTable.BeginInterrupt = HlpCpInterruptBegin;
        NewController.FunctionTable.FastEndOfInterrupt = NULL;
        NewController.FunctionTable.EndOfInterrupt =
                                                  HlpCpInterruptEndOfInterrupt;

        NewController.FunctionTable.RequestInterrupt =
                                                HlpCpInterruptRequestInterrupt;

        NewController.FunctionTable.StartProcessor = NULL;
        NewController.FunctionTable.SetLineState = HlpCpInterruptSetLineState;
        NewController.Context = InterruptData;
        NewController.Identifier = 0;
        NewController.ProcessorCount = 0;
        NewController.PriorityCount = 0;

        //
        // Register the controller with the system.
        //

        Status = Services->Register(HardwareModuleInterruptController,
                                    &NewController);

        if (!KSUCCESS(Status)) {
            goto IntegratorCpInterruptModuleEntryEnd;
        }
    }

IntegratorCpInterruptModuleEntryEnd:
    return;
}

//
// --------------------------------------------------------- Internal Functions
//

KSTATUS
HlpCpInterruptInitializeIoUnit (
    PVOID Context
    )

/*++

Routine Description:

    This routine initializes an interrupt controller. It's responsible for
    masking all interrupt lines on the controller and setting the current
    priority to the lowest (allow all interrupts). Once completed successfully,
    it is expected that interrupts can be enabled at the processor core with
    no interrupts occurring.

Arguments:

    Context - Supplies the pointer to the controller's context, provided by the
        hardware module upon initialization.

Return Value:

    STATUS_SUCCESS on success.

    Other status codes on failure.

--*/

{

    PINTEGRATORCP_INTERRUPT_DATA InterruptData;
    KSTATUS Status;

    InterruptData = (PINTEGRATORCP_INTERRUPT_DATA)Context;
    if (HlCpInterruptController == NULL) {
        HlCpInterruptController = HlCpKernelServices->MapPhysicalAddress(
                                       InterruptData->PhysicalAddress,
                                       INTEGRATORCP_INTERRUPT_CONTROLLER_SIZE,
                                       TRUE);

        if (HlCpInterruptController == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto CpInterruptInitializeIoUnitEnd;
        }

        //
        // Describe the interrupt lines on this controller.
        //

        Status = HlpCpInterruptDescribeLines();
        if (!KSUCCESS(Status)) {
            goto CpInterruptInitializeIoUnitEnd;
        }
    }

    //
    // Disable all FIQ and IRQ lines.
    //

    WRITE_INTERRUPT_REGISTER(CpInterruptIrqDisable, 0xFFFFFFFF);
    WRITE_INTERRUPT_REGISTER(CpInterruptFiqDisable, 0xFFFFFFFF);
    Status = STATUS_SUCCESS;

CpInterruptInitializeIoUnitEnd:
    return Status;
}

INTERRUPT_CAUSE
HlpCpInterruptBegin (
    PVOID Context,
    PINTERRUPT_LINE FiringLine,
    PULONG MagicCandy
    )

/*++

Routine Description:

    This routine is called when an interrupt fires. Its role is to determine
    if an interrupt has fired on the given controller, accept it, and determine
    which line fired if any. This routine will always be called with interrupts
    disabled at the processor core.

Arguments:

    Context - Supplies the pointer to the controller's context, provided by the
        hardware module upon initialization.

    FiringLine - Supplies a pointer where the interrupt hardware module will
        fill in which line fired, if applicable.

    MagicCandy - Supplies a pointer where the interrupt hardware module can
        store 32 bits of private information regarding this interrupt. This
        information will be returned to it when the End Of Interrupt routine
        is called.

Return Value:

    Returns an interrupt cause indicating whether or not an interrupt line,
    spurious interrupt, or no interrupt fired on this controller.

--*/

{

    ULONG Index;
    PINTEGRATORCP_INTERRUPT_DATA InterruptData;
    ULONG Mask;
    RUNLEVEL RunLevel;
    ULONG Status;

    InterruptData = (PINTEGRATORCP_INTERRUPT_DATA)Context;
    Status = READ_INTERRUPT_REGISTER(CpInterruptIrqStatus);
    if (Status == 0) {
        return InterruptCauseNoInterruptHere;
    }

    //
    // Find the first firing index.
    //

    Index = 0;
    while ((Status & 0x1) == 0) {
        Status = Status >> 1;
        Index += 1;
    }

    //
    // Disable all interrupts at or below this run level.
    //

    RunLevel = InterruptData->LineRunLevel[Index];
    Mask = InterruptData->RunLevelIrqMask[RunLevel];
    WRITE_INTERRUPT_REGISTER(CpInterruptIrqDisable, Mask);

    //
    // Save the run level as the magic candy to re-enable these interrupts.
    //

    *MagicCandy = RunLevel;

    //
    // Return the interrupting line's information.
    //

    FiringLine->Type = InterruptLineControllerSpecified;
    FiringLine->Controller = 0;
    FiringLine->Line = Index;
    return InterruptCauseLineFired;
}

VOID
HlpCpInterruptEndOfInterrupt (
    PVOID Context,
    ULONG MagicCandy
    )

/*++

Routine Description:

    This routine is called after an interrupt has fired and been serviced. Its
    role is to tell the interrupt controller that processing has completed.
    This routine will always be called with interrupts disabled at the
    processor core.

Arguments:

    Context - Supplies the pointer to the controller's context, provided by the
        hardware module upon initialization.

    MagicCandy - Supplies the magic candy that that the interrupt hardware
        module stored when the interrupt began.

Return Value:

    None.

--*/

{

    PINTEGRATORCP_INTERRUPT_DATA InterruptData;
    ULONG Mask;

    //
    // Enable all interrupts at or below this priority level.
    //

    InterruptData = (PINTEGRATORCP_INTERRUPT_DATA)Context;
    Mask = InterruptData->RunLevelIrqMask[MagicCandy];
    WRITE_INTERRUPT_REGISTER(CpInterruptIrqEnable, Mask);
    return;
}

KSTATUS
HlpCpInterruptRequestInterrupt (
    PVOID Context,
    PINTERRUPT_LINE Line,
    ULONG Vector,
    PINTERRUPT_HARDWARE_TARGET Target
    )

/*++

Routine Description:

    This routine requests a hardware interrupt on the given line.

Arguments:

    Context - Supplies the pointer to the controller's context, provided by the
        hardware module upon initialization.

    Line - Supplies a pointer to the interrupt line to spark.

    Vector - Supplies the vector to generate the interrupt on (for vectored
        architectures only).

    Target - Supplies a pointer to the set of processors to target.

Return Value:

    STATUS_SUCCESS on success.

    Error code on failure.

--*/

{

    //
    // This feature will be implemented when it is required (probably by
    // power management).
    //

    return STATUS_NOT_IMPLEMENTED;
}

KSTATUS
HlpCpInterruptSetLineState (
    PVOID Context,
    PINTERRUPT_LINE Line,
    PINTERRUPT_LINE_STATE State
    )

/*++

Routine Description:

    This routine enables or disables and configures an interrupt line.

Arguments:

    Context - Supplies the pointer to the controller's context, provided by the
        hardware module upon initialization.

    Line - Supplies a pointer to the line to set up. This will always be a
        controller specified line.

    State - Supplies a pointer to the new configuration of the line.

Return Value:

    Status code.

--*/

{

    ULONG BitMask;
    ULONG Index;
    PINTEGRATORCP_INTERRUPT_DATA InterruptData;
    RUNLEVEL RunLevel;
    KSTATUS Status;

    InterruptData = (PINTEGRATORCP_INTERRUPT_DATA)Context;
    if ((Line->Type != InterruptLineControllerSpecified) ||
        (Line->Controller != 0) ||
        (Line->Line >= INTEGRATORCP_INTERRUPT_LINE_COUNT)) {

        Status = STATUS_INVALID_PARAMETER;
        goto CpInterruptSetLineStateEnd;
    }

    if ((State->Output.Type != InterruptLineControllerSpecified) ||
        (State->Output.Controller != INTERRUPT_CPU_IDENTIFIER) ||
        (State->Output.Line != INTERRUPT_CPU_IRQ_PIN)) {

        Status = STATUS_INVALID_PARAMETER;
        goto CpInterruptSetLineStateEnd;
    }

    //
    // Determine which run level this interrupt belongs to.
    //

    RunLevel = VECTOR_TO_RUN_LEVEL(State->Vector);

    //
    // Calculate the bit to flip and flip it.
    //

    BitMask = 1 << Line->Line;
    if ((State->Flags & INTERRUPT_LINE_STATE_FLAG_ENABLED) != 0) {
        InterruptData->LineRunLevel[Line->Line] = RunLevel;
        for (Index = 0; Index <= RunLevel; Index += 1) {
            InterruptData->RunLevelIrqMask[Index] |= BitMask;
        }

        WRITE_INTERRUPT_REGISTER(CpInterruptIrqEnable, BitMask);

    } else {
        WRITE_INTERRUPT_REGISTER(CpInterruptIrqDisable, BitMask);

        ASSERT(InterruptData->LineRunLevel[Line->Line] == RunLevel);

        for (Index = 0; Index <= RunLevel; Index += 1) {
            InterruptData->RunLevelIrqMask[Index] &= ~BitMask;
        }

        InterruptData->LineRunLevel[Line->Line] = 0;
    }

    Status = STATUS_SUCCESS;

CpInterruptSetLineStateEnd:
    return Status;
}

KSTATUS
HlpCpInterruptDescribeLines (
    VOID
    )

/*++

Routine Description:

    This routine describes all interrupt lines to the system.

Arguments:

    None.

Return Value:

    Status code.

--*/

{

    INTERRUPT_LINES_DESCRIPTION Lines;
    KSTATUS Status;

    HlCpKernelServices->ZeroMemory(&Lines, sizeof(INTERRUPT_LINES_DESCRIPTION));
    Lines.Version = INTERRUPT_LINES_DESCRIPTION_VERSION;

    //
    // Describe the normal lines on the Integrator/CP.
    //

    Lines.Type = InterruptLinesStandardPin;
    Lines.Controller = 0;
    Lines.LineStart = 0;
    Lines.LineEnd = INTEGRATORCP_INTERRUPT_LINE_COUNT;
    Lines.Gsi = HlCpIntegratorTable->InterruptControllerGsiBase;
    Status = HlCpKernelServices->Register(HardwareModuleInterruptLines, &Lines);
    if (!KSUCCESS(Status)) {
        goto CpInterruptDescribeLinesEnd;
    }

    //
    // Register the output lines.
    //

    Lines.Type = InterruptLinesOutput;
    Lines.OutputControllerIdentifier = INTERRUPT_CPU_IDENTIFIER;
    Lines.LineStart = INTERRUPT_ARM_MIN_CPU_LINE;
    Lines.LineEnd = INTERRUPT_ARM_MAX_CPU_LINE;
    Status = HlCpKernelServices->Register(HardwareModuleInterruptLines, &Lines);
    if (!KSUCCESS(Status)) {
        goto CpInterruptDescribeLinesEnd;
    }

CpInterruptDescribeLinesEnd:
    return Status;
}

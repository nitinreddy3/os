/*++

Copyright (c) 2014 Minoca Corp. All Rights Reserved

Module Name:

    intr.c

Abstract:

    This module implements platform interrupt support for the TI PandaBoard.

Author:

    Evan Green 3-Mar-2014

Environment:

    Firmware

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <uefifw.h>
#include "dev/gic.h"
#include "pandafw.h"

//
// ---------------------------------------------------------------- Definitions
//

//
// ------------------------------------------------------ Data Type Definitions
//

//
// ----------------------------------------------- Internal Function Prototypes
//

VOID
EfipPlatformBeginInterrupt (
    UINT32 *InterruptNumber,
    VOID **InterruptContext
    );

VOID
EfipPlatformEndInterrupt (
    UINT32 InterruptNumber,
    VOID *InterruptContext
    );

//
// -------------------------------------------------------------------- Globals
//

GIC_CONTEXT EfiPandaGic;

//
// ------------------------------------------------------------------ Functions
//

EFI_STATUS
EfiPlatformInitializeInterrupts (
    EFI_PLATFORM_BEGIN_INTERRUPT *BeginInterruptFunction,
    EFI_PLATFORM_HANDLE_INTERRUPT *HandleInterruptFunction,
    EFI_PLATFORM_END_INTERRUPT *EndInterruptFunction
    )

/*++

Routine Description:

    This routine initializes support for platform interrupts. Interrupts are
    assumed to be disabled at the processor now. This routine should enable
    interrupts at the procesor core.

Arguments:

    BeginInterruptFunction - Supplies a pointer where a pointer to a function
        will be returned that is called when an interrupt occurs.

    HandleInterruptFunction - Supplies a pointer where a pointer to a function
        will be returned that is called to handle a platform-specific interurpt.
        NULL may be returned here.

    EndInterruptFunction - Supplies a pointer where a pointer to a function
        will be returned that is called to complete an interrupt.

Return Value:

    EFI Status code.

--*/

{

    EFI_STATUS Status;

    EfiPandaGic.DistributorBase = (VOID *)OMAP4430_GIC_DISTRIBUTOR_BASE;
    EfiPandaGic.CpuInterfaceBase = (VOID *)OMAP4430_GIC_CPU_INTERFACE_BASE;
    Status = EfipGicInitialize(&EfiPandaGic);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    *BeginInterruptFunction = EfipPlatformBeginInterrupt;
    *HandleInterruptFunction = NULL;
    *EndInterruptFunction = EfipPlatformEndInterrupt;
    EfiEnableInterrupts();
    return EFI_SUCCESS;
}

VOID
EfiPlatformTerminateInterrupts (
    VOID
    )

/*++

Routine Description:

    This routine terminates interrupt services in preparation for transitioning
    out of boot services.

Arguments:

    None.

Return Value:

    None.

--*/

{

    return;
}

EFI_STATUS
EfipPlatformSetInterruptLineState (
    UINT32 LineNumber,
    BOOLEAN Enabled,
    BOOLEAN EdgeTriggered
    )

/*++

Routine Description:

    This routine enables or disables an interrupt line.

Arguments:

    LineNumber - Supplies the line number to enable or disable.

    Enabled - Supplies a boolean indicating if the line should be enabled or
        disabled.

    EdgeTriggered - Supplies a boolean indicating if the interrupt is edge
        triggered (TRUE) or level triggered (FALSE).

Return Value:

    EFI Status code.

--*/

{

    EFI_STATUS Status;

    Status = EfipGicSetLineState(&EfiPandaGic,
                                 LineNumber,
                                 Enabled,
                                 EdgeTriggered);

    return Status;
}

//
// --------------------------------------------------------- Internal Functions
//

VOID
EfipPlatformBeginInterrupt (
    UINT32 *InterruptNumber,
    VOID **InterruptContext
    )

/*++

Routine Description:

    This routine is called when an interrupts comes in. The platform code is
    responsible for reporting the interrupt number. Interrupts are disabled at
    the processor core at this point.

Arguments:

    InterruptNumber - Supplies a pointer where interrupt line number will be
        returned.

    InterruptContext - Supplies a pointer where the platform can store a
        pointer's worth of context that will be passed back when ending the
        interrupt.

Return Value:

    None.

--*/

{

    EfipGicBeginInterrupt(&EfiPandaGic, InterruptNumber, InterruptContext);
    return;
}

VOID
EfipPlatformEndInterrupt (
    UINT32 InterruptNumber,
    VOID *InterruptContext
    )

/*++

Routine Description:

    This routine is called to finish handling of a platform interrupt. This is
    where the End-Of-Interrupt would get sent to the interrupt controller.

Arguments:

    InterruptNumber - Supplies the interrupt number that occurred.

    InterruptContext - Supplies the context returned by the interrupt
        controller when the interrupt began.

Return Value:

    None.

--*/

{

    EfipGicEndInterrupt(&EfiPandaGic, InterruptNumber, InterruptContext);
    return;
}

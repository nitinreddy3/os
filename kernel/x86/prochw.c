/*++

Copyright (c) 2012 Minoca Corp. All Rights Reserved

Module Name:

    prochw.c

Abstract:

    This module implements support functionality for hardware that is specific
    to the x86 architecture.

Author:

    Evan Green 3-Jul-2012

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel.h>
#include <minoca/x86.h>
#include <minoca/ioport.h>
#include <minoca/kdebug.h>

//
// ---------------------------------------------------------------- Definitions
//

//
// Define the number and size of alternate stacks. The TSS structures share
// these regions of memory. This stack size should be a multiple of a page
// size, since TSS segments should not cross page boundaries.
//

#define ALTERNATE_STACK_COUNT 2
#define ALTERNATE_STACK_SIZE 4096

//
// ----------------------------------------------- Internal Function Prototypes
//

//
// Declare any still-undefined built in interrupt handlers.
//

VOID
ArSingleStepExceptionHandlerAsm (
    ULONG ReturnEip,
    ULONG ReturnCodeSelector,
    ULONG ReturnEflags
    );

VOID
ArBreakExceptionHandlerAsm (
    ULONG ReturnEip,
    ULONG ReturnCodeSelector,
    ULONG ReturnEflags
    );

VOID
ArDivideByZeroExceptionHandlerAsm (
    ULONG ReturnEip,
    ULONG ReturnCodeSelector,
    ULONG ReturnEflags
    );

VOID
ArFpuAccessExceptionHandlerAsm (
    ULONG ReturnEip,
    ULONG ReturnCodeSelector,
    ULONG ReturnEflags
    );

VOID
HlSpuriousInterruptHandlerAsm (
    ULONG ReturnEip,
    ULONG ReturnCodeSelector,
    ULONG ReturnEflags
    );

VOID
ArpCreateGate (
    PPROCESSOR_GATE Gate,
    PVOID HandlerRoutine,
    USHORT Selector,
    UCHAR Type,
    UCHAR Privilege
    );

VOID
ArpInitializeTss (
    PTSS Task
    );

VOID
ArpInitializeGdt (
    PGDT_ENTRY GdtTable,
    PPROCESSOR_BLOCK ProcessorBlock,
    PTSS KernelTss,
    PTSS DoubleFaultTss,
    PTSS NmiTss
    );

VOID
ArpInitializeInterrupts (
    BOOL PhysicalMode,
    BOOL BootProcessor,
    PVOID Idt
    );

VOID
ArpSetProcessorFeatures (
    VOID
    );

//
// ------------------------------------------------------ Data Type Definitions
//

//
// -------------------------------------------------------------------- Globals
//

//
// Store pointers to functions used to save and restore floating point state.
//

PAR_SAVE_RESTORE_FPU_CONTEXT ArSaveFpuState;
PAR_SAVE_RESTORE_FPU_CONTEXT ArRestoreFpuState;

//
// Store globals for the per-processor data structures used by P0.
//

TSS ArP0Tss;
GDT_ENTRY ArP0Gdt[GDT_ENTRIES];
PROCESSOR_GATE ArP0Idt[IDT_SIZE];
PROCESSOR_BLOCK ArP0ProcessorBlock;
PVOID ArP0InterruptTable[MAXIMUM_VECTOR - MINIMUM_VECTOR + 1] = {NULL};

//
// Remember whether the processor was initialized with translation enabled or
// not.
//

BOOL ArTranslationEnabled = FALSE;

//
// Pointers to the interrupt dispatch code, which is repeated from the minimum
// to maximum device IDT entries.
//

extern UCHAR HlVectorStart;
extern UCHAR HlVectorMidpoint;
extern UCHAR HlVectorEnd;

//
// ------------------------------------------------------------------ Functions
//

ULONG
ArGetDataCacheLineSize (
    VOID
    )

/*++

Routine Description:

    This routine gets the size of a line in the L1 data cache.

Arguments:

    None.

Return Value:

    Returns the L1 data cache line size, in bytes.

--*/

{

    //
    // Since x86 architectures are always cache coherent, return the most fine
    // granularity possible.
    //

    return 1;
}

VOID
ArCleanCacheRegion (
    PVOID Address,
    UINTN Size
    )

/*++

Routine Description:

    This routine cleans the given region of virtual address space in the first
    level data cache.

Arguments:

    Address - Supplies the virtual address of the region to clean.

    Size - Supplies the number of bytes to clean.

Return Value:

    None.

--*/

{

    return;
}

VOID
ArCleanInvalidateCacheRegion (
    PVOID Address,
    UINTN Size
    )

/*++

Routine Description:

    This routine cleans and invalidates the given region of virtual address
    space in the first level data cache.

Arguments:

    Address - Supplies the virtual address of the region to clean.

    Size - Supplies the number of bytes to clean.

Return Value:

    None.

--*/

{

    return;
}

VOID
ArInvalidateCacheRegion (
    PVOID Address,
    UINTN Size
    )

/*++

Routine Description:

    This routine invalidates the region of virtual address space in the first
    level data cache. This routine is very dangerous, as any dirty data in the
    cache will be lost and gone.

Arguments:

    Address - Supplies the virtual address of the region to clean.

    Size - Supplies the number of bytes to clean.

Return Value:

    None.

--*/

{

    return;
}

VOID
ArInitializeProcessor (
    BOOL PhysicalMode,
    PVOID ProcessorStructures
    )

/*++

Routine Description:

    This routine initializes processor-specific structures. In the case of x86,
    it initializes the GDT and TSS.

Arguments:

    PhysicalMode - Supplies a boolean indicating whether or not the processor
        is operating in physical mode.

    ProcessorStructures - Supplies a pointer to the memory to use for basic
        processor structures, as returned by the allocate processor structures
        routine. For the boot processor, supply NULL here to use this routine's
        internal resources.

Return Value:

    None.

--*/

{

    UINTN Address;
    BOOL BootProcessor;
    ULONG Cr0;
    PVOID DoubleFaultStack;
    PTSS DoubleFaultTss;
    PGDT_ENTRY Gdt;
    PPROCESSOR_GATE Idt;
    PVOID InterruptTable;
    PVOID NmiStack;
    PTSS NmiTss;
    UINTN PageSize;
    PPROCESSOR_BLOCK ProcessorBlock;
    PTSS Tss;

    BootProcessor = TRUE;
    DoubleFaultStack = NULL;
    DoubleFaultTss = NULL;
    NmiStack = NULL;
    NmiTss = NULL;
    if (PhysicalMode == FALSE) {
        ArTranslationEnabled = TRUE;
    }

    //
    // Physical mode implies P0.
    //

    if (PhysicalMode != FALSE) {
        Gdt = ArP0Gdt;
        Idt = ArP0Idt;
        InterruptTable = ArP0InterruptTable;
        ProcessorBlock = &ArP0ProcessorBlock;
        Tss = &ArP0Tss;

    } else {

        //
        // Use the globals if this is the boot processor because the memory
        // subsystem is not yet online.
        //

        if (ProcessorStructures == NULL) {
            Gdt = ArP0Gdt;
            Idt = ArP0Idt;
            InterruptTable = ArP0InterruptTable;
            ProcessorBlock = &ArP0ProcessorBlock;
            Tss = &ArP0Tss;

        } else {
            BootProcessor = FALSE;
            PageSize = MmPageSize();
            Address = ALIGN_RANGE_UP((UINTN)ProcessorStructures, PageSize);
            DoubleFaultTss =
                         (PVOID)(Address + ALTERNATE_STACK_SIZE - sizeof(TSS));

            DoubleFaultStack = (PVOID)(DoubleFaultTss) - sizeof(PVOID);
            Address += ALTERNATE_STACK_SIZE;
            NmiTss = (PVOID)(Address + ALTERNATE_STACK_SIZE - sizeof(TSS));
            NmiStack = (PVOID)(NmiTss) - sizeof(PVOID);
            Gdt = (PGDT_ENTRY)(Address + ALTERNATE_STACK_SIZE);

            ASSERT(ALIGN_RANGE_DOWN((UINTN)Gdt, 8) == (UINTN)Gdt);

            //
            // Use a global IDT space.
            //

            Idt = ArP0Idt;
            ProcessorBlock = (PPROCESSOR_BLOCK)((PUCHAR)Gdt + sizeof(ArP0Gdt));
            Tss = (PTSS)(ProcessorBlock + 1);
            Address = ALIGN_RANGE_UP((UINTN)(Tss + 1), 8);
            InterruptTable = ArP0InterruptTable;
        }
    }

    //
    // Initialize the pointer to the processor block.
    //

    ProcessorBlock->Self = ProcessorBlock;
    ProcessorBlock->Idt = Idt;
    ProcessorBlock->InterruptTable = InterruptTable;
    ProcessorBlock->Tss = Tss;
    ProcessorBlock->Gdt = Gdt;

    //
    // Initialize and load the GDT and Tasks.
    //

    ArpInitializeTss(Tss);
    Tss->Cr3 = (UINTN)ArGetCurrentPageDirectory();
    if (DoubleFaultTss != NULL) {
        ArpInitializeTss(DoubleFaultTss);
        DoubleFaultTss->Esp0 = (UINTN)DoubleFaultStack;
        DoubleFaultTss->Esp = DoubleFaultTss->Esp0;
        DoubleFaultTss->Eip = (UINTN)ArDoubleFaultHandlerAsm;
        DoubleFaultTss->Cr3 = Tss->Cr3;

        //
        // Squirrel away the double fault stack into the kernel TSS' Esp1,
        // which is otherwise unused.
        //

        Tss->Esp1 = (UINTN)DoubleFaultStack;
    }

    if (NmiTss != NULL) {
        ArpInitializeTss(NmiTss);
        NmiTss->Esp0 = (UINTN)NmiStack;
        NmiTss->Esp = NmiTss->Esp0;
        NmiTss->Eip = (UINTN)KdNmiHandlerAsm;
        NmiTss->Cr3 = Tss->Cr3;
    }

    ArpInitializeGdt(Gdt, ProcessorBlock, Tss, DoubleFaultTss, NmiTss);
    ArLoadTr(KERNEL_TSS);
    ArpInitializeInterrupts(PhysicalMode, BootProcessor, Idt);
    ArpSetProcessorFeatures();

    //
    // Initialize the FPU, then disable access to it again.
    //

    Cr0 = ArGetControlRegister0();
    ArEnableFpu();
    ArInitializeFpu();
    ArSetControlRegister0(Cr0);
    return;
}

KSTATUS
ArFinishBootProcessorInitialization (
    VOID
    )

/*++

Routine Description:

    This routine performs additional initialization steps for processor 0 that
    were put off in pre-debugger initialization.

Arguments:

    None.

Return Value:

    Status code.

--*/

{

    UINTN Address;
    PVOID Allocation;
    UINTN AllocationSize;
    ULONG Cr3;
    PGDT_ENTRY GdtTable;
    PTSS MainTss;
    UINTN PageSize;
    PPROCESSOR_BLOCK ProcessorBlock;
    PVOID Stack;
    PTSS Tss;

    Cr3 = (UINTN)ArGetCurrentPageDirectory();
    PageSize = MmPageSize();
    GdtTable = ArP0Gdt;
    ProcessorBlock = KeGetCurrentProcessorBlock();
    MainTss = ProcessorBlock->Tss;

    //
    // Allocate and initialize double fault and NMI stacks now that MM is up
    // and running. Allocate extra for alignment purposes, as TSS structures
    // must not cross a page boundary.
    //

    AllocationSize = (ALTERNATE_STACK_SIZE * ALTERNATE_STACK_COUNT) + PageSize;
    Allocation = MmAllocateNonPagedPool(AllocationSize, ARCH_POOL_TAG);
    if (Allocation == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Address = ALIGN_RANGE_UP((UINTN)Allocation, PageSize);

    //
    // Initialize the double fault TSS and stack. Squirrel away the double
    // fault stack in Esp1 of the main TSS, which is otherwise unused.
    //

    Tss = (PVOID)(Address + ALTERNATE_STACK_SIZE - sizeof(TSS));
    Stack = (PVOID)(Tss) - sizeof(PVOID);
    ArpInitializeTss(Tss);
    Tss->Esp0 = (UINTN)Stack;
    MainTss->Esp1 = (UINTN)Stack;
    Tss->Esp = Tss->Esp0;
    Tss->Eip = (UINTN)ArDoubleFaultHandlerAsm;
    Tss->Cr3 = Cr3;
    ArpCreateSegmentDescriptor(
                             &(GdtTable[DOUBLE_FAULT_TSS / sizeof(GDT_ENTRY)]),
                             Tss,
                             sizeof(TSS),
                             GdtByteGranularity,
                             Gdt32BitTss,
                             SEGMENT_PRIVILEGE_KERNEL,
                             TRUE);

    //
    // Initialize the NMI TSS and stack (separate stack needed to avoid
    // vulnerable window during/before sysret instruction.
    //

    Address += ALTERNATE_STACK_SIZE;
    Tss = (PVOID)(Address + ALTERNATE_STACK_SIZE - sizeof(TSS));
    Stack = (PVOID)(Tss) - sizeof(PVOID);
    ArpInitializeTss(Tss);
    Tss->Esp0 = (UINTN)Stack;
    Tss->Esp = Tss->Esp0;
    Tss->Eip = (UINTN)KdNmiHandlerAsm;
    Tss->Cr3 = Cr3;
    ArpCreateSegmentDescriptor(&(GdtTable[NMI_TSS / sizeof(GDT_ENTRY)]),
                               Tss,
                               sizeof(TSS),
                               GdtByteGranularity,
                               Gdt32BitTss,
                               SEGMENT_PRIVILEGE_KERNEL,
                               TRUE);

    return STATUS_SUCCESS;
}

PVOID
ArAllocateProcessorStructures (
    ULONG ProcessorNumber
    )

/*++

Routine Description:

    This routine attempts to allocate and initialize early structures needed by
    a new processor.

Arguments:

    ProcessorNumber - Supplies the number of the processor that these resources
        will go to.

Return Value:

    Returns a pointer to the new processor resources on success.

    NULL on failure.

--*/

{

    UINTN Address;
    PVOID Allocation;
    UINTN AllocationSize;
    UINTN PageSize;
    PPROCESSOR_BLOCK ProcessorBlock;

    //
    // Allocate an extra page for alignment purposes, as TSS structures are
    // not supposed to cross page boundaries.
    //

    PageSize = MmPageSize();
    AllocationSize = ALTERNATE_STACK_COUNT * ALTERNATE_STACK_SIZE;
    AllocationSize += sizeof(ArP0Gdt) + sizeof(PROCESSOR_BLOCK) +
                      sizeof(ArP0Tss) + PageSize;

    Allocation = MmAllocateNonPagedPool(AllocationSize, ARCH_POOL_TAG);
    if (Allocation == NULL) {
        return NULL;
    }

    RtlZeroMemory(Allocation, AllocationSize);
    Address = ALIGN_RANGE_UP((UINTN)Allocation, PageSize);
    ProcessorBlock = (PPROCESSOR_BLOCK)((PUCHAR)Address +
                                        (ALTERNATE_STACK_COUNT *
                                         ALTERNATE_STACK_SIZE) +
                                        sizeof(ArP0Gdt));

    ProcessorBlock->ProcessorNumber = ProcessorNumber;
    return Allocation;
}

VOID
ArFreeProcessorStructures (
    PVOID ProcessorStructures
    )

/*++

Routine Description:

    This routine destroys a set of processor structures that have been
    allocated. It should go without saying, but obviously a processor must not
    be actively using these resources.

Arguments:

    ProcessorStructures - Supplies the pointer returned by the allocation
        routine.

Return Value:

    None.

--*/

{

    MmFreeNonPagedPool(ProcessorStructures);
    return;
}

BOOL
ArIsTranslationEnabled (
    VOID
    )

/*++

Routine Description:

    This routine determines if the processor was initialized with virtual-to-
    physical address translation enabled or not.

Arguments:

    None.

Return Value:

    TRUE if the processor is using a layer of translation between CPU accessible
    addresses and physical memory.

    FALSE if the processor was initialized in physical mode.

--*/

{

    return ArTranslationEnabled;
}

ULONG
ArGetIoPortCount (
    VOID
    )

/*++

Routine Description:

    This routine returns the number of I/O port addresses architecturally
    available.

Arguments:

    None.

Return Value:

    Returns the number of I/O port address supported by the architecture.

--*/

{

    return IO_PORT_COUNT;
}

ULONG
ArGetInterruptVectorCount (
    VOID
    )

/*++

Routine Description:

    This routine returns the number of interrupt vectors in the system, either
    architecturally defined or artificially created.

Arguments:

    None.

Return Value:

    Returns the number of interrupt vectors in use by the system.

--*/

{

    return INTERRUPT_VECTOR_COUNT;
}

ULONG
ArGetMinimumDeviceVector (
    VOID
    )

/*++

Routine Description:

    This routine returns the first interrupt vector that can be used by
    devices.

Arguments:

    None.

Return Value:

    Returns the minimum interrupt vector available for use by devices.

--*/

{

    return MINIMUM_VECTOR;
}

ULONG
ArGetMaximumDeviceVector (
    VOID
    )

/*++

Routine Description:

    This routine returns the last interrupt vector that can be used by
    devices.

Arguments:

    None.

Return Value:

    Returns the maximum interrupt vector available for use by devices.

--*/

{

    return MAXIMUM_DEVICE_VECTOR;
}

ULONG
ArGetTrapFrameSize (
    VOID
    )

/*++

Routine Description:

    This routine returns the size of the trap frame structure, in bytes.

Arguments:

    None.

Return Value:

    Returns the size of the trap frame structure, in bytes.

--*/

{

    return sizeof(TRAP_FRAME);
}

PVOID
ArGetInstructionPointer (
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine returns the instruction pointer out of the trap frame.

Arguments:

    TrapFrame - Supplies the trap frame from which the instruction pointer
        will be returned.

Return Value:

    Returns the instruction pointer the trap frame is pointing to.

--*/

{

    return (PVOID)TrapFrame->Eip;
}

BOOL
ArIsTrapFrameFromPrivilegedMode (
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine determines if the given trap frame occurred in a privileged
    environment or not.

Arguments:

    TrapFrame - Supplies the trap frame.

Return Value:

    TRUE if the execution environment of the trap frame is privileged.

    FALSE if the execution environment of the trap frame is not privileged.

--*/

{

    return IS_TRAP_FRAME_FROM_PRIVILEGED_MODE(TrapFrame);
}

VOID
ArSetSingleStep (
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine modifies the given trap frame registers so that a single step
    exception will occur. This is only supported on some architectures.

Arguments:

    TrapFrame - Supplies a pointer to the trap frame not modify.

Return Value:

    None.

--*/

{

    TrapFrame->Eflags |= IA32_EFLAG_TF;
    return;
}

VOID
ArInvalidateInstructionCacheRegion (
    PVOID Address,
    ULONG Size
    )

/*++

Routine Description:

    This routine invalidates the given region of virtual address space in the
    instruction cache.

Arguments:

    Address - Supplies the virtual address of the region to invalidate.

    Size - Supplies the number of bytes to invalidate.

Return Value:

    None.

--*/

{

    return;
}

VOID
ArGetKernelTssTrapFrame (
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine converts the kernel TSS to a trap frame.

Arguments:

    TrapFrame - Supplies a pointer where the filled out trap frame information
        will be returned.

Return Value:

    None.

--*/

{

    PTSS KernelTask;
    PPROCESSOR_BLOCK ProcessorBlock;

    //
    // Attempt to build the trap frame out of the kernel TSS. This code does
    // not take into account potential nesting of tasks, it always assumes the
    // kernel task was the one executing. If for example a double fault
    // occurred during an NMI handler, the wrong registers would be displayed.
    //

    RtlZeroMemory(TrapFrame, sizeof(TRAP_FRAME));
    ProcessorBlock = KeGetCurrentProcessorBlock();
    if (ProcessorBlock != NULL) {
        KernelTask = ProcessorBlock->Tss;
        if (KernelTask != NULL) {
            TrapFrame->Ds = KernelTask->Ds;
            TrapFrame->Es = KernelTask->Es;
            TrapFrame->Fs = KernelTask->Fs;
            TrapFrame->Gs = KernelTask->Gs;
            TrapFrame->Ss = KernelTask->Ss;
            TrapFrame->Eax = KernelTask->Eax;
            TrapFrame->Ebx = KernelTask->Ebx;
            TrapFrame->Ecx = KernelTask->Ecx;
            TrapFrame->Edx = KernelTask->Edx;
            TrapFrame->Esi = KernelTask->Esi;
            TrapFrame->Edi = KernelTask->Edi;
            TrapFrame->Ebp = KernelTask->Ebp;
            TrapFrame->Eip = KernelTask->Eip;
            TrapFrame->Cs = KernelTask->Cs;
            TrapFrame->Eflags = KernelTask->Eflags;
            TrapFrame->Esp = KernelTask->Esp;
        }
    }

    return;
}

VOID
ArSetKernelTssTrapFrame (
    PTRAP_FRAME TrapFrame
    )

/*++

Routine Description:

    This routine converts writes the given trap frame into the kernel TSS.

Arguments:

    TrapFrame - Supplies a pointer to the trap frame data to write.

Return Value:

    None.

--*/

{

    PTSS KernelTask;
    PPROCESSOR_BLOCK ProcessorBlock;

    //
    // Just like above, this routine assumes the kernel task was actually the
    // previous task. If it was not, these writes would be going to the wrong
    // place.
    //

    ProcessorBlock = KeGetCurrentProcessorBlock();
    if (ProcessorBlock != NULL) {
        KernelTask = ProcessorBlock->Tss;
        if (KernelTask != NULL) {
            KernelTask->Ds = TrapFrame->Ds;
            KernelTask->Es = TrapFrame->Es;
            KernelTask->Fs = TrapFrame->Fs;
            KernelTask->Gs = TrapFrame->Gs;
            KernelTask->Ss = TrapFrame->Ss;
            KernelTask->Eax = TrapFrame->Eax;
            KernelTask->Ebx = TrapFrame->Ebx;
            KernelTask->Ecx = TrapFrame->Ecx;
            KernelTask->Edx = TrapFrame->Edx;
            KernelTask->Esi = TrapFrame->Esi;
            KernelTask->Edi = TrapFrame->Edi;
            KernelTask->Ebp = TrapFrame->Ebp;
            KernelTask->Eip = TrapFrame->Eip;
            KernelTask->Cs = TrapFrame->Cs;
            KernelTask->Eflags = TrapFrame->Eflags;
            KernelTask->Esp = TrapFrame->Esp;
        }
    }

    return;
}

VOID
ArpCreateSegmentDescriptor (
    PGDT_ENTRY GdtEntry,
    PVOID Base,
    ULONG Limit,
    GDT_GRANULARITY Granularity,
    GDT_SEGMENT_TYPE Access,
    UCHAR PrivilegeLevel,
    BOOL System
    )

/*++

Routine Description:

    This routine initializes a GDT entry given the parameters.

Arguments:

    GdtEntry - Supplies a pointer to the GDT entry that will be initialized.

    Base - Supplies the base address where this segment begins.

    Limit - Supplies the size of the segment, either in bytes or kilobytes,
        depending on the Granularity parameter.

    Granularity - Supplies the granularity of the segment. Valid values are byte
        granularity or kilobyte granularity.

    Access - Supplies the access permissions on the segment.

    PrivilegeLevel - Supplies the privilege level that this segment requires.
        Valid values are 0 (most privileged, kernel) to 3 (user mode, least
        privileged).

    System - Supplies a flag indicating whether this is a system segment (TRUE)
        or a code/data segment.

Return Value:

    None.

--*/

{

    //
    // If all these magic numbers seem cryptic, see the comment above the
    // definition for the GDT_ENTRY structure.
    //

    GdtEntry->LimitLow = Limit & 0xFFFF;
    GdtEntry->BaseLow = (ULONG)Base & 0xFFFF;
    GdtEntry->BaseMiddle = ((ULONG)Base >> 16) & 0xFF;
    GdtEntry->Access = DEFAULT_GDT_ACCESS |
                       ((PrivilegeLevel & 0x3) << 5) |
                       (Access & 0xF);

    if (System != FALSE) {
        GdtEntry->Access |= GDT_SYSTEM_SEGMENT;

    } else {
        GdtEntry->Access |= GDT_CODE_DATA_SEGMENT;
    }

    GdtEntry->Granularity = DEFAULT_GDT_GRANULARITY |
                            Granularity |
                            ((Limit >> 16) & 0xF);

    GdtEntry->BaseHigh = ((ULONG)Base >> 24) & 0xFF;
    return;
}

//
// --------------------------------------------------------- Internal Functions
//

VOID
ArpHandleDoubleFault (
    VOID
    )

/*++

Routine Description:

    This routine handles double faults as gracefully as possible.

Arguments:

    None.

Return Value:

    This routine does not return, double faults are not recoverable.

--*/

{

    TRAP_FRAME TrapFrame;

    ArGetKernelTssTrapFrame(&TrapFrame);
    KdDebugExceptionHandler(EXCEPTION_DOUBLE_FAULT, NULL, &TrapFrame);
    KeCrashSystem(CRASH_KERNEL_STACK_EXCEPTION, (UINTN)&TrapFrame, 0, 0, 0);
    return;
}

VOID
ArpCreateGate (
    PPROCESSOR_GATE Gate,
    PVOID HandlerRoutine,
    USHORT Selector,
    UCHAR Type,
    UCHAR Privilege
    )

/*++

Routine Description:

    This routine initializes a task, call, trap, or interrupt gate with the
    given values.

Arguments:

    Gate - Supplies a pointer to the structure where the gate will be returned.
        It is assumed this structure is already allocated.

    HandlerRoutine - Supplies a pointer to the destination routine of this gate.

    Selector - Supplies the code selector this gate should run in.

    Type - Supplies the type of the gate. Set this to CALL_GATE_TYPE,
        INTERRUPT_GATE_TYPE, TASK_GATE_TYPE, or TRAP_GATE_TYPE.

    Privilege - Supplies the privilege level this gate should run in. 0 is the
        most privileged level, and 3 is the least privileged.

Return Value:

    None.

--*/

{

    Gate->LowOffset = (USHORT)((ULONG)HandlerRoutine & 0xFFFF);
    Gate->HighOffset = (USHORT)((ULONG)HandlerRoutine >> 16);
    Gate->Selector = Selector;

    //
    // Set bit 5-7 of the count to 0. Bits 4-0 are reserved and need to be set
    // to 0 as well.
    //

    Gate->Count = 0;

    //
    // Access is programmed as follows:
    //     Bit 7: Present. Set to 1 to indicate that this gate is present.
    //     Bits 5-6: Privilege level.
    //     Bit 4: Set to 0 to indicate it's a system gate.
    //     Bits 3-0: Type.
    //

    Gate->Access = Type | (Privilege << 5) | (1 << 7);
    return;
}

VOID
ArpInitializeTss (
    PTSS Task
    )

/*++

Routine Description:

    This routine initializes and loads the kernel Task State Segment (TSS).

Arguments:

    Task - Supplies a pointer to the task to initialize and load.

Return Value:

    None.

--*/

{

    RtlZeroMemory(Task, sizeof(TSS));

    //
    // Initialize the ring 0 stack. This will be set to a more reasonable value
    // before a privilege level switch.
    //

    Task->Esp0 = 0;
    Task->Ss0 = KERNEL_DS;
    Task->Ss = KERNEL_DS;
    Task->Cs = KERNEL_CS;
    Task->Ds = KERNEL_DS;
    Task->Es = KERNEL_DS;
    Task->Fs = GDT_PROCESSOR;
    Task->Gs = KERNEL_DS;
    Task->Eflags = IA32_EFLAG_ALWAYS_1;
    Task->IoMapBase = sizeof(TSS);
    return;
}

VOID
ArpInitializeGdt (
    PGDT_ENTRY GdtTable,
    PPROCESSOR_BLOCK ProcessorBlock,
    PTSS KernelTss,
    PTSS DoubleFaultTss,
    PTSS NmiTss
    )

/*++

Routine Description:

    This routine initializes and loads the kernel's Global Descriptor Table
    (GDT).

Arguments:

    GdtTable - Supplies a pointer to the global descriptor table to use. It is
        assumed this table contains enough entries to hold all the segment
        descriptors.

    ProcessorBlock - Supplies a pointer to the processor block to use for this
        processor.

    KernelTss - Supplies a pointer to the main kernel task.

    DoubleFaultTss - Supplies a pointer to the double fault TSS.

    NmiTss - Supplies a pointer to the NMI TSS.

Return Value:

    None.

--*/

{

    TABLE_REGISTER Gdt;

    //
    // The first segment descriptor must be unused. Set it to zero.
    //

    GdtTable[0].LimitLow = 0;
    GdtTable[0].BaseLow = 0;
    GdtTable[0].BaseMiddle = 0;
    GdtTable[0].Access = 0;
    GdtTable[0].Granularity = 0;
    GdtTable[0].BaseHigh = 0;

    //
    // Initialize the kernel code segment. Initialize the entry to cover all
    // 4GB of memory, with read/write permissions, and only on ring 0. This is
    // not a system segment.
    //

    ArpCreateSegmentDescriptor(&(GdtTable[KERNEL_CS / sizeof(GDT_ENTRY)]),
                               NULL,
                               MAX_GDT_LIMIT,
                               GdtKilobyteGranularity,
                               GdtCodeExecuteOnly,
                               SEGMENT_PRIVILEGE_KERNEL,
                               FALSE);

    //
    // Initialize the kernel data segment. Initialize the entry to cover
    // all 4GB of memory, with read/write permissions, and only on ring 0. This
    // is not a system segment.
    //

    ArpCreateSegmentDescriptor(&(GdtTable[KERNEL_DS / sizeof(GDT_ENTRY)]),
                               NULL,
                               MAX_GDT_LIMIT,
                               GdtKilobyteGranularity,
                               GdtDataReadWrite,
                               SEGMENT_PRIVILEGE_KERNEL,
                               FALSE);

    //
    // Initialize the user mode code segment. Initialize the entry to cover
    // the first 2GB of memory, with execute permissions, in ring 3. This is
    // not a system segment.
    //

    ArpCreateSegmentDescriptor(&(GdtTable[USER_CS / sizeof(GDT_ENTRY)]),
                               (PVOID)0,
                               (ULONG)KERNEL_VA_START >> PAGE_SHIFT,
                               GdtKilobyteGranularity,
                               GdtCodeExecuteOnly,
                               SEGMENT_PRIVILEGE_USER,
                               FALSE);

    //
    // Initialize the user mode data segment. Initialize the entry to cover
    // the first 2GB of memory, with read/write permissions, in ring 3. This is
    // not a system segment.
    //

    ArpCreateSegmentDescriptor(&(GdtTable[USER_DS / sizeof(GDT_ENTRY)]),
                               (PVOID)0,
                               (ULONG)KERNEL_VA_START >> PAGE_SHIFT,
                               GdtKilobyteGranularity,
                               GdtDataReadWrite,
                               SEGMENT_PRIVILEGE_USER,
                               FALSE);

    //
    // Initialize the processor block segment.
    //

    ArpCreateSegmentDescriptor(&(GdtTable[GDT_PROCESSOR / sizeof(GDT_ENTRY)]),
                               (PVOID)ProcessorBlock,
                               sizeof(PROCESSOR_BLOCK),
                               GdtByteGranularity,
                               GdtDataReadWrite,
                               SEGMENT_PRIVILEGE_KERNEL,
                               FALSE);

    //
    // Initialize the thread context segment, which can be programmed by
    // user mode.
    //

    ArpCreateSegmentDescriptor(&(GdtTable[GDT_THREAD / sizeof(GDT_ENTRY)]),
                               NULL,
                               sizeof(PROCESSOR_BLOCK),
                               GdtByteGranularity,
                               GdtDataReadWrite,
                               SEGMENT_PRIVILEGE_USER,
                               FALSE);

    //
    // Initialize the kernel TSS segments. The entry covers only the TSS
    // segment. This is a system segment (a 32-bit Free TSS to be exact).
    //

    ArpCreateSegmentDescriptor(&(GdtTable[KERNEL_TSS / sizeof(GDT_ENTRY)]),
                               KernelTss,
                               sizeof(TSS),
                               GdtByteGranularity,
                               Gdt32BitTss,
                               SEGMENT_PRIVILEGE_KERNEL,
                               TRUE);

    ArpCreateSegmentDescriptor(
                             &(GdtTable[DOUBLE_FAULT_TSS / sizeof(GDT_ENTRY)]),
                             DoubleFaultTss,
                             sizeof(TSS),
                             GdtByteGranularity,
                             Gdt32BitTss,
                             SEGMENT_PRIVILEGE_KERNEL,
                             TRUE);

    //
    // NMIs need a TSS so they can have their own stack, which is needed on
    // systems that use the "syscall" instruction. Because sysret doesn't
    // change stacks, there's a moment where kernel mode is running with a
    // user mode ESP. An NMI at that moment would mean executing kernel code
    // on a user mode stack, bad news.
    //

    ArpCreateSegmentDescriptor(&(GdtTable[NMI_TSS / sizeof(GDT_ENTRY)]),
                               NmiTss,
                               sizeof(TSS),
                               GdtByteGranularity,
                               Gdt32BitTss,
                               SEGMENT_PRIVILEGE_KERNEL,
                               TRUE);

    //
    // Install the new GDT table.
    //

    Gdt.Limit = sizeof(GDT_ENTRY) * GDT_ENTRIES;
    Gdt.Base = (ULONG)GdtTable;
    ArLoadGdtr(Gdt);
    ArLoadKernelDataSegments();
    return;
}

VOID
ArpInitializeInterrupts (
    BOOL PhysicalMode,
    BOOL BootProcessor,
    PVOID Idt
    )

/*++

Routine Description:

    This routine initializes and enables interrupts.

Arguments:

    PhysicalMode - Supplies a flag indicating that the processor is running
        with translation disabled.

    BootProcessor - Supplies a flag indicating whether this is processor 0 or
        an AP.

    Idt - Supplies a pointer to the Interrrupt Descriptor Table for this
        processor.

Return Value:

    None.

--*/

{

    ULONG DispatchCodeLength;
    ULONG IdtIndex;
    TABLE_REGISTER IdtRegister;
    PPROCESSOR_GATE IdtTable;
    PVOID ServiceRoutine;

    IdtTable = Idt;
    if (BootProcessor != FALSE) {

        //
        // Initialize the device vectors of the IDT. The vector dispatch code
        // is a bunch of copies of the same code, the only difference is which
        // vector number they push as a parameter. This has to be done because
        // the code length for a vector changes after 0x80 (push 0x80 is larger
        // than push 0x7f).
        //

        DispatchCodeLength = (ULONG)(&HlVectorMidpoint - &HlVectorStart) /
                             (MIDPOINT_VECTOR - MINIMUM_VECTOR);

        for (IdtIndex = MINIMUM_VECTOR;
             IdtIndex < MIDPOINT_VECTOR;
             IdtIndex += 1) {

            ServiceRoutine = &HlVectorStart + ((IdtIndex - MINIMUM_VECTOR) *
                                               DispatchCodeLength);

            ArpCreateGate(IdtTable + IdtIndex,
                          ServiceRoutine,
                          KERNEL_CS,
                          INTERRUPT_GATE_TYPE,
                          SEGMENT_PRIVILEGE_KERNEL);
        }

        DispatchCodeLength = (ULONG)(&HlVectorEnd - &HlVectorMidpoint) /
                             (MAXIMUM_VECTOR - MIDPOINT_VECTOR + 1);

        for (IdtIndex = MIDPOINT_VECTOR;
             IdtIndex <= MAXIMUM_VECTOR;
             IdtIndex += 1) {

            ServiceRoutine = &HlVectorMidpoint + ((IdtIndex - MIDPOINT_VECTOR) *
                                                  DispatchCodeLength);

            ArpCreateGate(IdtTable + IdtIndex,
                          ServiceRoutine,
                          KERNEL_CS,
                          INTERRUPT_GATE_TYPE,
                          SEGMENT_PRIVILEGE_KERNEL);
        }

        //
        // Set up the debug trap handlers.
        //

        ArpCreateGate(IdtTable + VECTOR_DIVIDE_ERROR,
                      ArDivideByZeroExceptionHandlerAsm,
                      KERNEL_CS,
                      TRAP_GATE_TYPE,
                      SEGMENT_PRIVILEGE_USER);

        ArpCreateGate(IdtTable + VECTOR_NMI,
                      NULL,
                      NMI_TSS,
                      TASK_GATE_TYPE,
                      SEGMENT_PRIVILEGE_KERNEL);

        ArpCreateGate(IdtTable + VECTOR_BREAKPOINT,
                      ArBreakExceptionHandlerAsm,
                      KERNEL_CS,
                      INTERRUPT_GATE_TYPE,
                      SEGMENT_PRIVILEGE_USER);

        ArpCreateGate(IdtTable + VECTOR_DEBUG,
                      ArSingleStepExceptionHandlerAsm,
                      KERNEL_CS,
                      INTERRUPT_GATE_TYPE,
                      SEGMENT_PRIVILEGE_KERNEL);

        ArpCreateGate(IdtTable + VECTOR_DEBUG_SERVICE,
                      KdDebugServiceHandlerAsm,
                      KERNEL_CS,
                      INTERRUPT_GATE_TYPE,
                      SEGMENT_PRIVILEGE_KERNEL);

        //
        // Set up the double fault and general protection fault handlers.
        //

        ArpCreateGate(IdtTable + VECTOR_DOUBLE_FAULT,
                      NULL,
                      DOUBLE_FAULT_TSS,
                      TASK_GATE_TYPE,
                      SEGMENT_PRIVILEGE_KERNEL);

        ArpCreateGate(IdtTable + VECTOR_PROTECTION_FAULT,
                      ArProtectionFaultHandlerAsm,
                      KERNEL_CS,
                      INTERRUPT_GATE_TYPE,
                      SEGMENT_PRIVILEGE_KERNEL);

        ArpCreateGate(IdtTable + VECTOR_MATH_FAULT,
                      ArMathFaultHandlerAsm,
                      KERNEL_CS,
                      INTERRUPT_GATE_TYPE,
                      SEGMENT_PRIVILEGE_KERNEL);

        //
        // Set up the system call handler.
        //

        ArpCreateGate(IdtTable + VECTOR_SYSTEM_CALL,
                      ArSystemCallHandlerAsm,
                      KERNEL_CS,
                      TRAP_GATE_TYPE,
                      SEGMENT_PRIVILEGE_USER);

        //
        // Set up the spurious interrupt vector.
        //

        ArpCreateGate(IdtTable + VECTOR_SPURIOUS_INTERRUPT,
                      HlSpuriousInterruptHandlerAsm,
                      KERNEL_CS,
                      INTERRUPT_GATE_TYPE,
                      SEGMENT_PRIVILEGE_KERNEL);

        //
        // Set up the page fault handler.
        //

        ArpCreateGate(IdtTable + VECTOR_PAGE_FAULT,
                      ArpPageFaultHandlerAsm,
                      KERNEL_CS,
                      INTERRUPT_GATE_TYPE,
                      SEGMENT_PRIVILEGE_KERNEL);

        ArpCreateGate(IdtTable + VECTOR_STACK_EXCEPTION,
                      ArpPageFaultHandlerAsm,
                      KERNEL_CS,
                      INTERRUPT_GATE_TYPE,
                      SEGMENT_PRIVILEGE_KERNEL);

        //
        // Set up floating point access handlers.
        //

        ArpCreateGate(IdtTable + VECTOR_DEVICE_NOT_AVAILABLE,
                      ArFpuAccessExceptionHandlerAsm,
                      KERNEL_CS,
                      TRAP_GATE_TYPE,
                      SEGMENT_PRIVILEGE_KERNEL);
    }

    //
    // Load the IDT register with our interrupt descriptor table.
    //

    IdtRegister.Limit = (IDT_SIZE * 8) - 1;
    IdtRegister.Base = (ULONG)IdtTable;
    ArLoadIdtr(&IdtRegister);
    return;
}

VOID
ArpSetProcessorFeatures (
    VOID
    )

/*++

Routine Description:

    This routine reads processor features.

Arguments:

    None.

Return Value:

    None.

--*/

{

    ULONG Cr4;
    ULONG Eax;
    ULONG Ebx;
    ULONG Ecx;
    ULONG Edx;

    //
    // First call CPUID to find out the highest supported value.
    //

    Eax = X86_CPUID_IDENTIFICATION;
    ArCpuid(&Eax, &Ebx, &Ecx, &Edx);
    if (Eax < X86_CPUID_BASIC_INFORMATION) {
        return;
    }

    Eax = X86_CPUID_BASIC_INFORMATION;
    ArCpuid(&Eax, &Ebx, &Ecx, &Edx);

    //
    // If FXSAVE and FXRSTOR are supported, set the bits in CR4 to enable them.
    //

    if ((Edx & X86_CPUID_BASIC_EDX_FX_SAVE_RESTORE) != 0) {
        ArSaveFpuState = ArFxSave;
        ArRestoreFpuState = ArFxRestore;
        Cr4 = ArGetControlRegister4();
        Cr4 |= CR4_OS_FX_SAVE_RESTORE | CR4_OS_XMM_EXCEPTIONS |
               CR4_PAGE_GLOBAL_ENABLE;

        ArSetControlRegister4(Cr4);

    //
    // Fall back to the old FSAVE/FRSTOR instructions.
    //

    } else {
        ArSaveFpuState = ArSaveX87State;
        ArRestoreFpuState = ArRestoreX87State;
    }

    return;
}

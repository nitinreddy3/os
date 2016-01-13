/*++

Copyright (c) 2013 Minoca Corp. All Rights Reserved

Module Name:

    iobuf.c

Abstract:

    This module implements I/O buffer management.

Author:

    Evan Green 6-Jan-2013

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel.h>
#include "mmp.h"

//
// ---------------------------------------------------------------- Definitions
//

//
// This flag indicates that the underlying buffer being described was created
// with this structure. When the structure is destroyed, the memory will be
// freed as well.
//

#define IO_BUFFER_FLAG_MEMORY_OWNED 0x00000001

//
// This flag is set when the structure was not allocated by these routines.
//

#define IO_BUFFER_FLAG_STRUCTURE_NOT_OWNED 0x00000002

//
// This flag is set when the I/O buffer's memory is locked.
//

#define IO_BUFFER_FLAG_MEMORY_LOCKED 0x00000004

//
// This flag is set when the I/O buffer meta-data is non-paged.
//

#define IO_BUFFER_FLAG_NON_PAGED 0x00000008

//
// This flag is set if the buffer is meant to be filled with physical pages
// from page cache entries.
//

#define IO_BUFFER_FLAG_PAGE_CACHE_BACKED 0x00000010

//
// This flag is set if the buffer represents a single fragment of another I/O
// buffer.
//

#define IO_BUFFER_FLAG_FRAGMENT 0x00000020

//
// This flag is set if the I/O buffer represents a region in user mode.
//

#define IO_BUFFER_FLAG_USER_MODE 0x00000040

//
// This flag is set if the I/O buffer is completely mapped. It does not have to
// be virtually contiguous.
//

#define IO_BUFFER_FLAG_MAPPED 0x00000080

//
// This flag is set if the I/O buffer is mapped virtually contiguous.
//

#define IO_BUFFER_FLAG_VIRTUALLY_CONTIGUOUS 0x00000100

//
// This flag is set if the I/O buffer needs to be unmapped on free. An I/O
// buffer may have valid virtual addresses, but only needs to be unmapped if
// the virtual addresses were allocated by I/O buffer routines.
//

#define IO_BUFFER_FLAG_UNMAP_ON_FREE 0x00000200

//
// This flag is set if the I/O buffer can be extended by appending physical
// pages, page cache entries, or by allocating new physical memory.
//

#define IO_BUFFER_FLAG_EXTENDABLE 0x00000400

//
// Store the number of I/O vectors to place on the stack before needed to
// allocate the array.
//

#define LOCAL_IO_VECTOR_COUNT 8

//
// ------------------------------------------------------ Data Type Definitions
//

//
// ----------------------------------------------- Internal Function Prototypes
//

VOID
MmpReleaseIoBufferResources (
    PIO_BUFFER IoBuffer
    );

KSTATUS
MmpMapIoBufferFragments (
    PIO_BUFFER IoBuffer,
    UINTN FragmentStart,
    UINTN FragmentCount,
    ULONG MapFlags
    );

VOID
MmpUnmapIoBuffer (
    PIO_BUFFER IoBuffer
    );

BOOL
MmpIsIoBufferMapped (
    PIO_BUFFER IoBuffer,
    BOOL VirtuallyContiguous
    );

KSTATUS
MmpExtendIoBuffer (
    PIO_BUFFER IoBuffer,
    PHYSICAL_ADDRESS MinimumPhysicalAddress,
    PHYSICAL_ADDRESS MaximumPhysicalAddress,
    UINTN Alignment,
    UINTN Size,
    BOOL PhysicallyContiguous
    );

//
// -------------------------------------------------------------------- Globals
//

//
// Remember the size of the I/O buffer alignment.
//

ULONG MmIoBufferAlignment;

//
// ------------------------------------------------------------------ Functions
//

KERNEL_API
PIO_BUFFER
MmAllocateNonPagedIoBuffer (
    PHYSICAL_ADDRESS MinimumPhysicalAddress,
    PHYSICAL_ADDRESS MaximumPhysicalAddress,
    UINTN Alignment,
    UINTN Size,
    BOOL PhysicallyContiguous,
    BOOL WriteThrough,
    BOOL NonCached
    )

/*++

Routine Description:

    This routine allocates memory for use as an I/O buffer. This memory will
    remain mapped in memory until the buffer is destroyed.

Arguments:

    MinimumPhysicalAddress - Supplies the minimum physical address of the
        allocation.

    MaximumPhysicalAddress - Supplies the maximum physical address of the
        allocation.

    Alignment - Supplies the required physical alignment of the buffer.

    Size - Supplies the minimum size of the buffer, in bytes.

    PhysicallyContiguous - Supplies a boolean indicating whether or not the
        requested buffer should be physically contiguous.

    WriteThrough - Supplies a boolean indicating if the I/O buffer virtual
        addresses should be mapped write through (TRUE) or the default
        write back (FALSE). If you're not sure, supply FALSE.

    NonCached - Supplies a boolean indicating if the I/O buffer virtual
        addresses should be mapped non-cached (TRUE) or the default, which is
        to map is as normal cached memory (FALSE). If you're not sure, supply
        FALSE.

Return Value:

    Returns a pointer to the I/O buffer on success, or NULL on failure.

--*/

{

    UINTN AlignedSize;
    UINTN AllocationSize;
    PVOID CurrentAddress;
    UINTN FragmentCount;
    UINTN FragmentIndex;
    PIO_BUFFER IoBuffer;
    UINTN PageCount;
    UINTN PageIndex;
    ULONG PageShift;
    ULONG PageSize;
    PHYSICAL_ADDRESS PhysicalAddress;
    UINTN PhysicalRunAlignment;
    UINTN PhysicalRunSize;
    KSTATUS Status;
    ULONG UnmapFlags;
    PVOID VirtualAddress;

    PageShift = MmPageShift();
    PageSize = MmPageSize();
    VirtualAddress = NULL;

    //
    // Align both the alignment and the size up to a page. Alignment up to a
    // page does not work if the value is 0.
    //

    if (Alignment == 0) {
        Alignment = PageSize;

    } else {
        Alignment = ALIGN_RANGE_UP(Alignment, PageSize);
    }

    AlignedSize = ALIGN_RANGE_UP(Size, Alignment);
    PageCount = AlignedSize >> PageShift;

    //
    // TODO: Implement support for honoring the minimum and maximum physical
    // addresses in I/O buffers.
    //

    ASSERT((MinimumPhysicalAddress == 0) &&
           ((MaximumPhysicalAddress == MAX_ULONG) ||
            (MaximumPhysicalAddress == MAX_ULONGLONG)));

    //
    // If the buffer will be physically contiguous then only one fragment is
    // needed.
    //

    AllocationSize = sizeof(IO_BUFFER);
    if (PhysicallyContiguous != FALSE) {
        FragmentCount = 1;

    } else {
        FragmentCount = PageCount;
    }

    AllocationSize += (FragmentCount * sizeof(IO_BUFFER_FRAGMENT));

    //
    // Always assume that the I/O buffer might end up cached.
    //

    AllocationSize += (PageCount * sizeof(PPAGE_CACHE_ENTRY));

    //
    // Allocate an I/O buffer.
    //

    IoBuffer = MmAllocateNonPagedPool(AllocationSize, MM_IO_ALLOCATION_TAG);
    if (IoBuffer == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto AllocateIoBufferEnd;
    }

    RtlZeroMemory(IoBuffer, AllocationSize);
    IoBuffer->Internal.MaxFragmentCount = FragmentCount;
    IoBuffer->Internal.PageCount = PageCount;
    IoBuffer->Internal.TotalSize = AlignedSize;
    IoBuffer->Fragment = (PVOID)IoBuffer + sizeof(IO_BUFFER);
    IoBuffer->Internal.PageCacheEntries = (PVOID)IoBuffer +
                                          sizeof(IO_BUFFER) +
                                          (FragmentCount *
                                           sizeof(IO_BUFFER_FRAGMENT));

    //
    // Allocate a region of kernel address space.
    //

    Status = MmpAllocateAddressRange(&MmKernelVirtualSpace,
                                     AlignedSize,
                                     PageSize,
                                     MemoryTypeReserved,
                                     AllocationStrategyAnyAddress,
                                     FALSE,
                                     &VirtualAddress);

    if (!KSUCCESS(Status)) {
        goto AllocateIoBufferEnd;
    }

    //
    // Physically back and map the region based on the alignment and contiguity.
    //

    PhysicalRunAlignment = Alignment;
    if (PhysicallyContiguous != FALSE) {
        PhysicalRunSize = AlignedSize;

    } else {
        PhysicalRunSize = PhysicalRunAlignment;
    }

    Status = MmpMapRange(VirtualAddress,
                         AlignedSize,
                         PhysicalRunAlignment,
                         PhysicalRunSize,
                         WriteThrough,
                         NonCached);

    if (!KSUCCESS(Status)) {
        goto AllocateIoBufferEnd;
    }

    //
    // Now fill in I/O buffer fragments for this allocation.
    //

    if (PhysicallyContiguous != FALSE) {
        IoBuffer->FragmentCount = 1;
        IoBuffer->Fragment[0].VirtualAddress = VirtualAddress;
        IoBuffer->Fragment[0].Size = AlignedSize;
        PhysicalAddress = MmpVirtualToPhysical(VirtualAddress, NULL);

        ASSERT(PhysicalAddress != INVALID_PHYSICAL_ADDRESS);

        IoBuffer->Fragment[0].PhysicalAddress = PhysicalAddress;

    } else {

        ASSERT(IoBuffer->FragmentCount == 0);

        //
        // Iterate over the pages, coalescing physically contiguous regions
        // into the same fragment.
        //

        CurrentAddress = VirtualAddress;
        FragmentIndex = 0;
        for (PageIndex = 0; PageIndex < PageCount; PageIndex += 1) {
            PhysicalAddress = MmpVirtualToPhysical(CurrentAddress, NULL);

            ASSERT(PhysicalAddress != INVALID_PHYSICAL_ADDRESS);

            //
            // If this buffer is contiguous with the last one, then just up the
            // size of this fragment. Otherwise, add a new fragment.
            //

            if ((IoBuffer->FragmentCount != 0) &&
                ((IoBuffer->Fragment[FragmentIndex - 1].PhysicalAddress +
                  IoBuffer->Fragment[FragmentIndex - 1].Size) ==
                  PhysicalAddress)) {

                IoBuffer->Fragment[FragmentIndex - 1].Size += PageSize;

            } else {
                IoBuffer->Fragment[FragmentIndex].VirtualAddress =
                                                                CurrentAddress;

                IoBuffer->Fragment[FragmentIndex].PhysicalAddress =
                                                               PhysicalAddress;

                IoBuffer->Fragment[FragmentIndex].Size = PageSize;
                IoBuffer->FragmentCount += 1;
                FragmentIndex += 1;
            }

            CurrentAddress += PageSize;
        }

        ASSERT(IoBuffer->FragmentCount <= PageCount);
    }

    IoBuffer->Internal.Flags = IO_BUFFER_FLAG_NON_PAGED |
                               IO_BUFFER_FLAG_UNMAP_ON_FREE |
                               IO_BUFFER_FLAG_MEMORY_OWNED |
                               IO_BUFFER_FLAG_MEMORY_LOCKED |
                               IO_BUFFER_FLAG_MAPPED |
                               IO_BUFFER_FLAG_VIRTUALLY_CONTIGUOUS;

    ASSERT(KSUCCESS(Status));

AllocateIoBufferEnd:
    if (!KSUCCESS(Status)) {
        if (VirtualAddress != NULL) {
            UnmapFlags = UNMAP_FLAG_FREE_PHYSICAL_PAGES |
                         UNMAP_FLAG_SEND_INVALIDATE_IPI;

            MmpFreeAccountingRange(NULL,
                                   &MmKernelVirtualSpace,
                                   VirtualAddress,
                                   AlignedSize,
                                   FALSE,
                                   UnmapFlags);
        }

        if (IoBuffer != NULL) {
            MmFreeNonPagedPool(IoBuffer);
            IoBuffer = NULL;
        }
    }

    return IoBuffer;
}

KERNEL_API
PIO_BUFFER
MmAllocatePagedIoBuffer (
    UINTN Size
    )

/*++

Routine Description:

    This routine allocates memory for use as a pageable I/O buffer.

Arguments:

    Size - Supplies the minimum size of the buffer, in bytes.

Return Value:

    Returns a pointer to the I/O buffer on success, or NULL on failure.

--*/

{

    UINTN AllocationSize;
    PIO_BUFFER IoBuffer;

    AllocationSize = sizeof(IO_BUFFER) +
                     sizeof(IO_BUFFER_FRAGMENT) +
                     Size;

    IoBuffer = MmAllocatePagedPool(AllocationSize, MM_IO_ALLOCATION_TAG);
    if (IoBuffer == NULL) {
        return NULL;
    }

    RtlZeroMemory(IoBuffer, AllocationSize);
    IoBuffer->Fragment = (PVOID)IoBuffer + sizeof(IO_BUFFER);
    IoBuffer->FragmentCount = 1;
    IoBuffer->Internal.TotalSize = Size;
    IoBuffer->Internal.MaxFragmentCount = 1;
    IoBuffer->Fragment[0].VirtualAddress = (PVOID)IoBuffer +
                                           sizeof(IO_BUFFER) +
                                           sizeof(IO_BUFFER_FRAGMENT);

    IoBuffer->Fragment[0].Size = Size;
    IoBuffer->Fragment[0].PhysicalAddress = INVALID_PHYSICAL_ADDRESS;
    IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_VIRTUALLY_CONTIGUOUS |
                                IO_BUFFER_FLAG_MAPPED;

    return IoBuffer;
}

KERNEL_API
PIO_BUFFER
MmAllocateUninitializedIoBuffer (
    UINTN Size,
    BOOL CacheBacked
    )

/*++

Routine Description:

    This routine allocates an uninitialized I/O buffer that the caller will
    fill in with pages. It simply allocates the structures for the given
    size, assuming a buffer fragment may be required for each page.

Arguments:

    Size - Supplies the minimum size of the buffer, in bytes. This size is
        rounded up (always) to a page, but does assume page alignment.

    CacheBacked - Supplies a boolean indicating if the buffer is to be backed
        by page cache entries or not.

Return Value:

    Returns a pointer to the I/O buffer on success, or NULL on failure.

--*/

{

    ULONG AllocationSize;
    ULONG FragmentSize;
    PIO_BUFFER IoBuffer;
    UINTN PageCount;

    Size = ALIGN_RANGE_UP(Size, MmPageSize());
    PageCount = Size >> MmPageShift();
    FragmentSize = PageCount * sizeof(IO_BUFFER_FRAGMENT);
    AllocationSize = sizeof(IO_BUFFER) + FragmentSize;
    if (CacheBacked != FALSE) {
        AllocationSize += PageCount * sizeof(PPAGE_CACHE_ENTRY);
    }

    IoBuffer = MmAllocateNonPagedPool(AllocationSize, MM_IO_ALLOCATION_TAG);
    if (IoBuffer == NULL) {
        goto AllocateUninitializedIoBufferEnd;
    }

    RtlZeroMemory(IoBuffer, AllocationSize);
    IoBuffer->Internal.PageCount = PageCount;
    IoBuffer->Internal.MaxFragmentCount = PageCount;
    IoBuffer->Fragment = (PVOID)IoBuffer + sizeof(IO_BUFFER);
    IoBuffer->Internal.Flags = IO_BUFFER_FLAG_NON_PAGED |
                               IO_BUFFER_FLAG_EXTENDABLE;

    if (CacheBacked != FALSE) {
        IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_PAGE_CACHE_BACKED |
                                    IO_BUFFER_FLAG_MEMORY_LOCKED;

        IoBuffer->Internal.PageCacheEntries = (PVOID)IoBuffer +
                                              sizeof(IO_BUFFER) +
                                              FragmentSize;
    }

AllocateUninitializedIoBufferEnd:
    return IoBuffer;
}

KERNEL_API
KSTATUS
MmCreateIoBuffer (
    PVOID Buffer,
    UINTN SizeInBytes,
    BOOL NonPaged,
    BOOL LockMemory,
    BOOL KernelMode,
    PIO_BUFFER *NewIoBuffer
    )

/*++

Routine Description:

    This routine creates an I/O buffer from an existing memory buffer. This
    routine must be called at low level.

Arguments:

    Buffer - Supplies a pointer to the memory buffer on which to base the I/O
        buffer.

    SizeInBytes - Supplies the size of the buffer, in bytes.

    NonPaged - Supplies a boolean indicating whether or not the I/O buffer
        structure should be non-paged.

    LockMemory - Supplies a boolean indicating whether or not the buffer's
        memory needs to be locked.

    KernelMode - Supplies a boolean indicating whether or not this buffer is
        a kernel mode buffer (TRUE) or a user mode buffer (FALSE). If it is a
        user mode buffer, this routine will fail if a non-user mode address
        was passed in.

    NewIoBuffer - Supplies a pointer where a pointer to the new I/O buffer
        will be returned on success.

Return Value:

    Status code.

--*/

{

    UINTN AllocationSize;
    UINTN BytesLocked;
    PVOID CurrentAddress;
    PVOID EndAddress;
    UINTN FragmentIndex;
    UINTN FragmentSize;
    PIMAGE_SECTION ImageSection;
    PIO_BUFFER IoBuffer;
    IO_BUFFER LockedBuffer;
    PVOID NextAddress;
    PPAGE_CACHE_ENTRY PageCacheEntry;
    UINTN PageCount;
    UINTN PageIndex;
    UINTN PageOffset;
    ULONG PageShift;
    ULONG PageSize;
    PHYSICAL_ADDRESS PhysicalAddress;
    PKPROCESS Process;
    PVOID SectionEnd;
    KSTATUS Status;

    BytesLocked = 0;
    ImageSection = NULL;
    PageShift = MmPageShift();
    PageSize = MmPageSize();

    ASSERT(KeGetRunLevel() == RunLevelLow);

    EndAddress = Buffer + SizeInBytes;
    PageCount = (ALIGN_RANGE_UP((UINTN)EndAddress, PageSize) -
                 ALIGN_RANGE_DOWN((UINTN)Buffer, PageSize)) >> PageShift;

    //
    // Create an I/O buffer structure. If the memory is to be locked, assume
    // that locked memory is backed by the page cache.
    //

    if (LockMemory != FALSE) {
        AllocationSize = sizeof(IO_BUFFER) +
                         (PageCount * sizeof(IO_BUFFER_FRAGMENT)) +
                         (PageCount * sizeof(PPAGE_CACHE_ENTRY));

    } else {
        AllocationSize = sizeof(IO_BUFFER) + sizeof(IO_BUFFER_FRAGMENT);
    }

    if (NonPaged != FALSE) {
        IoBuffer = MmAllocateNonPagedPool(AllocationSize, MM_IO_ALLOCATION_TAG);

    } else {
        IoBuffer = MmAllocatePagedPool(AllocationSize, MM_IO_ALLOCATION_TAG);
    }

    if (IoBuffer == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto CreateIoBufferEnd;
    }

    RtlZeroMemory(IoBuffer, AllocationSize);
    IoBuffer->Fragment = (PVOID)IoBuffer + sizeof(IO_BUFFER);

    //
    // Record that the meta-data is non-paged so that it can be properly
    // released. Also record that all pages in the buffer are locked. It is
    // necessary to do this here in case the locking process fails; previously
    // locked pages need to be cleaned up.
    //

    if (NonPaged != FALSE) {
        IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_NON_PAGED;
    }

    if (KernelMode != FALSE) {
        Process = PsGetKernelProcess();

        ASSERT((Buffer >= KERNEL_VA_START) && (Buffer + SizeInBytes >= Buffer));

    } else {
        Process = PsGetCurrentProcess();

        ASSERT(Process != PsGetKernelProcess());

        if ((Buffer + SizeInBytes > KERNEL_VA_START) ||
            (Buffer + SizeInBytes < Buffer)) {

            Status = STATUS_ACCESS_VIOLATION;
            goto CreateIoBufferEnd;
        }

        IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_USER_MODE;
    }

    IoBuffer->Internal.TotalSize = SizeInBytes;
    IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_MAPPED |
                                IO_BUFFER_FLAG_VIRTUALLY_CONTIGUOUS;

    //
    // If the memory is not meant to be locked, just build the I/O buffer with
    // one fragment and only fill in the virtual address.
    //

    if (LockMemory == FALSE) {
        IoBuffer->Internal.MaxFragmentCount = 1;
        IoBuffer->FragmentCount = 1;
        IoBuffer->Fragment[0].VirtualAddress = Buffer;
        IoBuffer->Fragment[0].Size = SizeInBytes;
        IoBuffer->Fragment[0].PhysicalAddress = INVALID_PHYSICAL_ADDRESS;
        Status = STATUS_SUCCESS;
        goto CreateIoBufferEnd;
    }

    //
    // Initialize the page cache entry array.
    //

    IoBuffer->Internal.MaxFragmentCount = PageCount;
    IoBuffer->Internal.PageCount = PageCount;
    IoBuffer->Internal.PageCacheEntries = (PVOID)IoBuffer +
                                          sizeof(IO_BUFFER) +
                                          (PageCount *
                                           sizeof(IO_BUFFER_FRAGMENT));

    //
    // Make sure the entire buffer is in memory, and lock it down there.
    //

    CurrentAddress = Buffer;
    FragmentIndex = 0;
    PageIndex = 0;
    SectionEnd = NULL;
    while (CurrentAddress < EndAddress) {

        //
        // Attempt to grab the next section if a section boundary was just
        // crossed or there has been no section up to this point. If there
        // is no section, assume the memory is non-paged.
        //

        if (SectionEnd <= CurrentAddress) {
            if (ImageSection != NULL) {
                MmpImageSectionReleaseReference(ImageSection);
                ImageSection = NULL;
            }

            Status = MmpLookupSection(CurrentAddress,
                                      Process,
                                      &ImageSection,
                                      &PageOffset);

            if (KSUCCESS(Status)) {
                SectionEnd = ImageSection->VirtualAddress + ImageSection->Size;
            }
        }

        //
        // If there is an image section, then page the data in and lock it down
        // at the same time.
        //

        if (ImageSection != NULL) {
            Status = MmpPageIn(ImageSection, PageOffset, &LockedBuffer);
            if (Status == STATUS_TRY_AGAIN) {
                continue;
            }

            if (!KSUCCESS(Status)) {
                goto CreateIoBufferEnd;
            }

            //
            // Get the locked physical address and page cache entry from the
            // returned I/O buffer. Transfer the reference taken on the page
            // cache entry to the new I/O buffer.
            //

            PhysicalAddress = MmGetIoBufferPhysicalAddress(&LockedBuffer, 0);
            PageCacheEntry = MmGetIoBufferPageCacheEntry(&LockedBuffer, 0);
            if (PageCacheEntry != NULL) {
                IoBuffer->Internal.PageCacheEntries[PageIndex] = PageCacheEntry;
                IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_PAGE_CACHE_BACKED;
            }

        //
        // If there is no image section, then the page better be non-paged and
        // the caller should not release it until this I/O buffer is done using
        // it.
        //

        } else {
            PhysicalAddress = MmpVirtualToPhysical(CurrentAddress, NULL);
            if (PhysicalAddress == INVALID_PHYSICAL_ADDRESS) {
                Status = STATUS_INVALID_PARAMETER;
                goto CreateIoBufferEnd;
            }
        }

        //
        // Determine the size of this fragment. If this is the beginning of
        // the buffer, then go up to the next page boundary. Clip if that goes
        // beyond the end. This makes sure all fragments are page aligned
        // except for the beginning and end.
        //

        NextAddress = (PVOID)(UINTN)ALIGN_RANGE_UP((UINTN)CurrentAddress + 1,
                                                   PageSize);

        if (NextAddress > EndAddress) {
            NextAddress = EndAddress;
        }

        FragmentSize = (UINTN)NextAddress - (UINTN)CurrentAddress;

        ASSERT(FragmentSize != 0);

        //
        // If this buffer is contiguous with the last one, then just up the
        // size of this fragment. Otherwise, add a new fragment.
        //

        if ((IoBuffer->FragmentCount != 0) &&
            ((IoBuffer->Fragment[FragmentIndex - 1].PhysicalAddress +
              IoBuffer->Fragment[FragmentIndex - 1].Size) == PhysicalAddress)) {

            IoBuffer->Fragment[FragmentIndex - 1].Size += FragmentSize;

        } else {
            IoBuffer->Fragment[FragmentIndex].VirtualAddress = CurrentAddress;
            IoBuffer->Fragment[FragmentIndex].PhysicalAddress = PhysicalAddress;
            IoBuffer->Fragment[FragmentIndex].Size = FragmentSize;
            IoBuffer->FragmentCount += 1;
            FragmentIndex += 1;
        }

        BytesLocked += FragmentSize;
        CurrentAddress += FragmentSize;
        PageOffset += 1;
        PageIndex += 1;
    }

    Status = STATUS_SUCCESS;

CreateIoBufferEnd:
    if (LockMemory != FALSE) {
        if (ImageSection != NULL) {
            MmpImageSectionReleaseReference(ImageSection);
        }

        if (BytesLocked != 0) {
            IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_MEMORY_LOCKED;
        }
    }

    if (!KSUCCESS(Status)) {
        if (IoBuffer != NULL) {
            MmFreeIoBuffer(IoBuffer);
            IoBuffer = NULL;
        }
    }

    *NewIoBuffer = IoBuffer;
    return Status;
}

KSTATUS
MmCreateIoBufferFromVector (
    PIO_VECTOR Vector,
    BOOL VectorInKernelMode,
    UINTN VectorCount,
    PIO_BUFFER *NewIoBuffer
    )

/*++

Routine Description:

    This routine creates a paged usermode I/O buffer based on an I/O vector
    array. This is generally used to support vectored I/O functions in the C
    library.

Arguments:

    Vector - Supplies a pointer to the I/O vector array.

    VectorInKernelMode - Supplies a boolean indicating if the given I/O vector
        array comes directly from kernel mode.

    VectorCount - Supplies the number of elements in the vector array.

    NewIoBuffer - Supplies a pointer where a pointer to the newly created I/O
        buffer will be returned on success. The caller is responsible for
        releasing this buffer.

Return Value:

    STATUS_SUCCESS on success.

    STATUS_INVALID_PARAMETER if the vector count is invalid.

    STATUS_INSUFFICIENT_RESOURCES on allocation failure.

    STATUS_ACCESS_VIOLATION if the given vector array was from user-mode and
    was not valid.

--*/

{

    PVOID Address;
    PIO_VECTOR AllocatedVector;
    UINTN AllocationSize;
    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    PIO_BUFFER IoBuffer;
    PIO_VECTOR IoVector;
    IO_VECTOR LocalVector[LOCAL_IO_VECTOR_COUNT];
    PIO_BUFFER_FRAGMENT PreviousFragment;
    UINTN Size;
    KSTATUS Status;
    UINTN TotalSize;
    UINTN VectorIndex;

    ASSERT(KeGetRunLevel() == RunLevelLow);

    AllocatedVector = NULL;
    IoBuffer = NULL;
    if ((VectorCount > MAX_IO_VECTOR_COUNT) || (VectorCount == 0)) {
        Status = STATUS_INVALID_PARAMETER;
        goto CreateIoBufferFromVectorEnd;
    }

    IoVector = Vector;
    if (VectorInKernelMode == FALSE) {
        if (VectorCount < LOCAL_IO_VECTOR_COUNT) {
            IoVector = LocalVector;

        } else {
            AllocatedVector = MmAllocatePagedPool(
                                               sizeof(IO_VECTOR) * VectorCount,
                                               MM_IO_ALLOCATION_TAG);

            if (AllocatedVector == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto CreateIoBufferFromVectorEnd;
            }

            IoVector = AllocatedVector;
        }

        Status = MmCopyFromUserMode(IoVector,
                                    Vector,
                                    sizeof(IO_VECTOR) * VectorCount);

        if (!KSUCCESS(Status)) {
            goto CreateIoBufferFromVectorEnd;
        }
    }

    //
    // Create an I/O buffer structure, set up for a paged user-mode buffer with
    // a fragment for each vector.
    //

    AllocationSize = sizeof(IO_BUFFER) +
                     (VectorCount * sizeof(IO_BUFFER_FRAGMENT));

    IoBuffer = MmAllocatePagedPool(AllocationSize, MM_IO_ALLOCATION_TAG);
    if (IoBuffer == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto CreateIoBufferFromVectorEnd;
    }

    RtlZeroMemory(IoBuffer, AllocationSize);
    IoBuffer->Internal.Flags = IO_BUFFER_FLAG_USER_MODE | IO_BUFFER_FLAG_MAPPED;
    IoBuffer->Internal.MaxFragmentCount = VectorCount;
    IoBuffer->Fragment = (PVOID)IoBuffer + sizeof(IO_BUFFER);

    //
    // Fill in the fragments.
    //

    TotalSize = 0;
    FragmentIndex = 0;
    PreviousFragment = NULL;
    Fragment = IoBuffer->Fragment;
    for (VectorIndex = 0; VectorIndex < VectorCount; VectorIndex += 1) {
        Address = IoVector[VectorIndex].Data;
        Size = IoVector[VectorIndex].Length;

        //
        // Validate the vector address.
        //

        if ((Address >= KERNEL_VA_START) ||
            (Address + Size > KERNEL_VA_START) ||
            (Address + Size < Address)) {

            Status = STATUS_ACCESS_VIOLATION;
            goto CreateIoBufferFromVectorEnd;
        }

        //
        // Skip empty vectors.
        //

        if (Size == 0) {
            continue;

        //
        // Coalesce adjacent vectors.
        //

        } else if ((PreviousFragment != NULL) &&
                   (PreviousFragment->VirtualAddress + PreviousFragment->Size ==
                    Address)) {

            PreviousFragment->Size += IoVector[VectorIndex].Length;

        //
        // Add this as a new fragment.
        //

        } else {
            Fragment->VirtualAddress = IoVector[VectorIndex].Data;
            Fragment->Size = IoVector[VectorIndex].Length;
            FragmentIndex += 1;
            PreviousFragment = Fragment;
            Fragment += 1;
        }

        TotalSize += IoVector[VectorIndex].Length;
    }

    IoBuffer->Internal.TotalSize = TotalSize;
    IoBuffer->FragmentCount = FragmentIndex;
    Status = STATUS_SUCCESS;

CreateIoBufferFromVectorEnd:
    if (!KSUCCESS(Status)) {
        if (IoBuffer != NULL) {
            MmFreeIoBuffer(IoBuffer);
            IoBuffer = NULL;
        }
    }

    if (AllocatedVector != NULL) {
        MmFreePagedPool(AllocatedVector);
    }

    *NewIoBuffer = IoBuffer;
    return Status;
}

VOID
MmInitializeIoBuffer (
    PIO_BUFFER IoBuffer,
    PVOID VirtualAddress,
    PHYSICAL_ADDRESS PhysicalAddress,
    UINTN SizeInBytes,
    BOOL CacheBacked,
    BOOL MemoryLocked
    )

/*++

Routine Description:

    This routine initializes an I/O buffer based on the given virtual and
    physical address and the size. It is assumed that the range of bytes is
    both virtually and physically contiguous so that it can be contained in
    one fragment.

Arguments:

    IoBuffer - Supplies a pointer to the I/O buffer to initialize.

    VirtualAddress - Supplies the starting virtual address of the I/O buffer.

    PhysicalAddress - Supplies the starting physical address of the I/O buffer.

    SizeInBytes - Supplies the size of the I/O buffer, in bytes.

    CacheBacked - Supplies a boolean indicating if the I/O buffer will be
        backed by page cache entries.

    MemoryLocked - Supplies a boolean if the physical address supplied is
        locked in memory.

Return Value:

    None.

--*/

{

    UINTN Address;
    ULONG PageSize;

    Address = (UINTN)VirtualAddress;
    PageSize = MmPageSize();

    //
    // Assert that this buffer only spans one page.
    //

    ASSERT(ALIGN_RANGE_UP(Address + SizeInBytes, PageSize) -
           ALIGN_RANGE_DOWN(Address, PageSize) <= PageSize);

    //
    // Note that the I/O buffer structure is not owned so that it is not
    // released when freed.
    //

    RtlZeroMemory(IoBuffer, sizeof(IO_BUFFER));
    IoBuffer->Internal.Flags = IO_BUFFER_FLAG_STRUCTURE_NOT_OWNED;
    IoBuffer->Fragment = &(IoBuffer->Internal.Fragment);
    IoBuffer->Internal.MaxFragmentCount = 1;

    //
    // If the caller is initializing the buffer to be cache-backed, then set up
    // the page cache entries array.
    //

    if (CacheBacked != FALSE) {
        IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_PAGE_CACHE_BACKED |
                                    IO_BUFFER_FLAG_EXTENDABLE |
                                    IO_BUFFER_FLAG_MEMORY_LOCKED;

        IoBuffer->Internal.PageCacheEntries =
                                          &(IoBuffer->Internal.PageCacheEntry);

        IoBuffer->Internal.PageCount = 1;
    }

    //
    // Record that the memory is locked so that the physical pages get unlocked
    // when the buffer is released.
    //

    if (MemoryLocked != FALSE) {
        IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_MEMORY_LOCKED;
    }

    //
    // Find the physical address if it was not supplied and a virtual address
    // was supplied.
    //

    if (VirtualAddress != NULL) {
        IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_MAPPED |
                                    IO_BUFFER_FLAG_VIRTUALLY_CONTIGUOUS;

        if (PhysicalAddress == INVALID_PHYSICAL_ADDRESS) {
            PhysicalAddress = MmpVirtualToPhysical(VirtualAddress, NULL);

            ASSERT(PhysicalAddress != INVALID_PHYSICAL_ADDRESS);
        }
    }

    //
    // If a physical address is now present, set up the first and only fragment.
    //

    if (PhysicalAddress != INVALID_PHYSICAL_ADDRESS) {

        ASSERT(SizeInBytes != 0);

        IoBuffer->Internal.TotalSize = SizeInBytes;
        IoBuffer->Fragment[0].VirtualAddress = VirtualAddress;
        IoBuffer->Fragment[0].Size = SizeInBytes;
        IoBuffer->Fragment[0].PhysicalAddress = PhysicalAddress;
        IoBuffer->FragmentCount = 1;
    }

    return;
}

KERNEL_API
VOID
MmFreeIoBuffer (
    PIO_BUFFER IoBuffer
    )

/*++

Routine Description:

    This routine destroys an I/O buffer. If the memory was allocated when the
    I/O buffer was created, then the memory will be released at this time as
    well.

Arguments:

    IoBuffer - Supplies a pointer to the I/O buffer to release.

Return Value:

    None.

--*/

{

    ULONG Flags;

    Flags = IoBuffer->Internal.Flags;
    MmpReleaseIoBufferResources(IoBuffer);
    if ((Flags & IO_BUFFER_FLAG_STRUCTURE_NOT_OWNED) == 0) {
        if ((Flags & IO_BUFFER_FLAG_NON_PAGED) != 0) {
            MmFreeNonPagedPool(IoBuffer);

        } else {
            MmFreePagedPool(IoBuffer);
        }
    }

    return;
}

VOID
MmResetIoBuffer (
    PIO_BUFFER IoBuffer
    )

/*++

Routine Description:

    This routine resets an I/O buffer for re-use, unmapping any memory and
    releasing any associated page cache entries.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

Return Value:

    Status code.

--*/

{

    //
    // Support user mode I/O buffers if this fires and it seems useful to add.
    //

    ASSERT((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) == 0);

    //
    // Release all the resources associated with the I/O buffer, but do not
    // free the buffer structure itself.
    //

    MmpReleaseIoBufferResources(IoBuffer);

    //
    // Now zero and reset the I/O buffer.
    //

    ASSERT(IoBuffer->Fragment != NULL);

    RtlZeroMemory(IoBuffer->Fragment,
                  IoBuffer->FragmentCount * sizeof(IO_BUFFER_FRAGMENT));

    IoBuffer->FragmentCount = 0;
    IoBuffer->Internal.TotalSize = 0;
    IoBuffer->Internal.CurrentOffset = 0;
    IoBuffer->Internal.Flags &= ~(IO_BUFFER_FLAG_UNMAP_ON_FREE |
                                  IO_BUFFER_FLAG_MAPPED |
                                  IO_BUFFER_FLAG_VIRTUALLY_CONTIGUOUS);

    if (IoBuffer->Internal.PageCacheEntries != NULL) {
        RtlZeroMemory(IoBuffer->Internal.PageCacheEntries,
                      IoBuffer->Internal.PageCount * sizeof(PVOID));
    }

    return;
}

KERNEL_API
KSTATUS
MmMapIoBuffer (
    PIO_BUFFER IoBuffer,
    BOOL WriteThrough,
    BOOL NonCached,
    BOOL VirtuallyContiguous
    )

/*++

Routine Description:

    This routine maps the given I/O buffer into memory. If the caller requests
    that the I/O buffer be mapped virtually contiguous, then all fragments will
    be updated with the virtually contiguous mappings. If the I/O buffer does
    not need to be virtually contiguous, then this routine just ensure that
    each fragment is mapped.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

    WriteThrough - Supplies a boolean indicating if the virtual addresses
        should be mapped write through (TRUE) or the default write back (FALSE).

    NonCached - Supplies a boolean indicating if the virtual addresses should
        be mapped non-cached (TRUE) or the default, which is to map is as
        normal cached memory (FALSE).

    VirtuallyContiguous - Supplies a boolean indicating whether or not the
        caller needs the I/O buffer to be mapped virtually contiguous (TRUE) or
        not (FALSE). In the latter case, each I/O buffer fragment will at least
        be virtually contiguous.

Return Value:

    Status code.

--*/

{

    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentCount;
    UINTN FragmentIndex;
    ULONG IoBufferFlags;
    ULONG MapFlags;
    UINTN MapFragmentStart;
    BOOL MapRequired;
    KSTATUS Status;

    ASSERT(IoBuffer->FragmentCount >= 1);

    //
    // Check to see if the I/O buffer is already virtually contiguous. Note
    // that the flag might not be set if the I/O buffer is backed by the page
    // cache and a virtually contiguous mapping request has not yet been made.
    //

    IoBufferFlags = IoBuffer->Internal.Flags;
    if (VirtuallyContiguous != FALSE) {
        if ((IoBufferFlags & IO_BUFFER_FLAG_VIRTUALLY_CONTIGUOUS) != 0) {

            ASSERT(MmpIsIoBufferMapped(IoBuffer, TRUE) != FALSE);

            return STATUS_SUCCESS;
        }

        if (MmpIsIoBufferMapped(IoBuffer, TRUE) != FALSE) {
            IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_VIRTUALLY_CONTIGUOUS;
            return STATUS_SUCCESS;
        }

    //
    // Otherwise, if the I/O buffer is mapped, then it is good enough.
    //

    } else {
        if ((IoBufferFlags & IO_BUFFER_FLAG_MAPPED) != 0) {

            ASSERT(MmpIsIoBufferMapped(IoBuffer, FALSE) != FALSE);

            return STATUS_SUCCESS;
        }

        if (MmpIsIoBufferMapped(IoBuffer, FALSE) != FALSE) {
            IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_MAPPED;
            return STATUS_SUCCESS;
        }
    }

    //
    // User mode buffers should always be mapped virtually contiguous.
    //

    ASSERT((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) == 0);

    //
    // Collect the map flags. This routine should never allocate user mode
    // virtual addresses.
    //

    MapFlags = MAP_FLAG_PRESENT | MAP_FLAG_GLOBAL;
    if (WriteThrough != FALSE) {
        MapFlags |= MAP_FLAG_WRITE_THROUGH;
    }

    if (NonCached != FALSE) {
        MapFlags |= MAP_FLAG_CACHE_DISABLE;
    }

    //
    // If a virtually contiguous mapping was requested, unmap any existing
    // ranges and then allocate an address range to cover the whole buffer.
    //

    if (VirtuallyContiguous != FALSE) {
        if ((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_MAPPED) != 0) {
            MmpUnmapIoBuffer(IoBuffer);
        }

        Status = MmpMapIoBufferFragments(IoBuffer,
                                         0,
                                         IoBuffer->FragmentCount,
                                         MapFlags);

        if (!KSUCCESS(Status)) {
            goto MapIoBufferEnd;
        }

        IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_VIRTUALLY_CONTIGUOUS;

    //
    // Otherwise run through the fragments and map any portions of the I/O
    // buffer that are not currently mapped.
    //

    } else {
        MapRequired = FALSE;
        MapFragmentStart = 0;
        for (FragmentIndex = 0;
             FragmentIndex < IoBuffer->FragmentCount;
             FragmentIndex += 1) {

            Fragment = &(IoBuffer->Fragment[FragmentIndex]);

            //
            // If this fragment is already mapped, then map the unmapped set of
            // fragments before it, if necessary.
            //

            if (Fragment->VirtualAddress != NULL) {
                if (MapRequired == FALSE) {
                    continue;
                }

                FragmentCount = FragmentIndex - MapFragmentStart;
                Status = MmpMapIoBufferFragments(IoBuffer,
                                                 MapFragmentStart,
                                                 FragmentCount,
                                                 MapFlags);

                if (!KSUCCESS(Status)) {
                    goto MapIoBufferEnd;
                }

                //
                // Reset to search for the next run of unmapped fragments.
                //

                MapRequired = FALSE;
                continue;
            }

            //
            // If this is the first unmapped fragment found, then store its
            // index.
            //

            if (MapRequired == FALSE) {
                MapFragmentStart = FragmentIndex;
                MapRequired = TRUE;
            }
        }

        //
        // If the last set of fragments was unmapped, map it here.
        //

        if (MapRequired != FALSE) {
            FragmentCount = FragmentIndex - MapFragmentStart;
            Status = MmpMapIoBufferFragments(IoBuffer,
                                             MapFragmentStart,
                                             FragmentCount,
                                             MapFlags);

            if (!KSUCCESS(Status)) {
                goto MapIoBufferEnd;
            }
        }
    }

    IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_UNMAP_ON_FREE |
                                IO_BUFFER_FLAG_MAPPED;

    Status = STATUS_SUCCESS;

MapIoBufferEnd:
    return Status;
}

KERNEL_API
KSTATUS
MmCopyIoBuffer (
    PIO_BUFFER Destination,
    UINTN DestinationOffset,
    PIO_BUFFER Source,
    UINTN SourceOffset,
    UINTN ByteCount
    )

/*++

Routine Description:

    This routine copies the contents of the source I/O buffer starting at the
    source offset to the destination I/O buffer starting at the destination
    offset. It assumes that the arguments are correct such that the copy can
    succeed.

Arguments:

    Destination - Supplies a pointer to the destination I/O buffer that is to
        be copied into.

    DestinationOffset - Supplies the offset into the destination I/O buffer
        where the copy should begin.

    Source - Supplies a pointer to the source I/O buffer whose contents will be
        copied to the destination.

    SourceOffset - Supplies the offset into the source I/O buffer where the
        copy should begin.

    ByteCount - Supplies the size of the requested copy in bytes.

Return Value:

    Status code.

--*/

{

    UINTN BytesThisRound;
    PIO_BUFFER_FRAGMENT DestinationFragment;
    UINTN DestinationFragmentOffset;
    PVOID DestinationVirtualAddress;
    UINTN ExtensionSize;
    UINTN FragmentIndex;
    UINTN MaxDestinationSize;
    UINTN MaxSourceSize;
    PIO_BUFFER_FRAGMENT SourceFragment;
    UINTN SourceFragmentOffset;
    PVOID SourceVirtualAddress;
    KSTATUS Status;

    DestinationOffset += Destination->Internal.CurrentOffset;
    SourceOffset += Source->Internal.CurrentOffset;

    //
    // The source should always have enough data for the copy.
    //

    ASSERT((SourceOffset + ByteCount) <= Source->Internal.TotalSize);

    //
    // If memory can be appended to the destination and it needs to be, then
    // extend the I/O buffer.
    //

    ASSERT(((Destination->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) != 0) ||
           ((DestinationOffset + ByteCount) <=
            Destination->Internal.TotalSize));

    if (((Destination->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) != 0) &&
        ((DestinationOffset + ByteCount) > Destination->Internal.TotalSize)) {

        ExtensionSize = (DestinationOffset + ByteCount) -
                        Destination->Internal.TotalSize;

        Status = MmpExtendIoBuffer(Destination,
                                   0,
                                   MAX_ULONGLONG,
                                   0,
                                   ExtensionSize,
                                   FALSE);

        if (!KSUCCESS(Status)) {
            goto CopyIoBufferEnd;
        }
    }

    //
    // Both I/O buffers had better not be user mode buffers.
    //

    ASSERT(((Destination->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) == 0) ||
           ((Source->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) == 0));

    //
    // Make sure both buffers are mapped.
    //

    Status = MmMapIoBuffer(Destination, FALSE, FALSE, FALSE);
    if (!KSUCCESS(Status)) {
        goto CopyIoBufferEnd;
    }

    Status = MmMapIoBuffer(Source, FALSE, FALSE, FALSE);
    if (!KSUCCESS(Status)) {
        goto CopyIoBufferEnd;
    }

    //
    // Do not assume that the fragments are virtually contiguous. Get the
    // starting fragment for both buffers.
    //

    DestinationFragment = NULL;
    DestinationFragmentOffset = 0;
    for (FragmentIndex = 0;
         FragmentIndex < Destination->FragmentCount;
         FragmentIndex += 1) {

        DestinationFragment = &(Destination->Fragment[FragmentIndex]);
        if ((DestinationFragmentOffset + DestinationFragment->Size) >
            DestinationOffset) {

            DestinationFragmentOffset = DestinationOffset -
                                        DestinationFragmentOffset;

            break;
        }

        DestinationFragmentOffset += DestinationFragment->Size;
    }

    ASSERT(DestinationFragment != NULL);
    ASSERT(FragmentIndex != Destination->FragmentCount);

    SourceFragment = NULL;
    SourceFragmentOffset = 0;
    for (FragmentIndex = 0;
         FragmentIndex < Source->FragmentCount;
         FragmentIndex += 1) {

        SourceFragment = &(Source->Fragment[FragmentIndex]);
        if ((SourceFragmentOffset + SourceFragment->Size) > SourceOffset) {
            SourceFragmentOffset = SourceOffset - SourceFragmentOffset;
            break;
        }

        SourceFragmentOffset += SourceFragment->Size;
    }

    ASSERT(SourceFragment != NULL);
    ASSERT(FragmentIndex != Source->FragmentCount);

    //
    // Now execute the copy fragment by fragment.
    //

    MaxDestinationSize = DestinationFragment->Size - DestinationFragmentOffset;
    MaxSourceSize = SourceFragment->Size - SourceFragmentOffset;
    while (ByteCount != 0) {
        if (MaxDestinationSize < MaxSourceSize) {
            BytesThisRound = MaxDestinationSize;

        } else {
            BytesThisRound = MaxSourceSize;
        }

        if (BytesThisRound > ByteCount) {
            BytesThisRound = ByteCount;
        }

        ASSERT(DestinationFragment->VirtualAddress != NULL);
        ASSERT(SourceFragment->VirtualAddress != NULL);

        DestinationVirtualAddress = DestinationFragment->VirtualAddress +
                                    DestinationFragmentOffset;

        SourceVirtualAddress = SourceFragment->VirtualAddress +
                               SourceFragmentOffset;

        if ((Destination->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) != 0) {
            Status = MmCopyToUserMode(DestinationVirtualAddress,
                                      SourceVirtualAddress,
                                      BytesThisRound);

        } else if ((Source->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) != 0) {
            Status = MmCopyFromUserMode(DestinationVirtualAddress,
                                        SourceVirtualAddress,
                                        BytesThisRound);

        } else {
            RtlCopyMemory(DestinationVirtualAddress,
                          SourceVirtualAddress,
                          BytesThisRound);

            Status = STATUS_SUCCESS;
        }

        if (!KSUCCESS(Status)) {
            goto CopyIoBufferEnd;
        }

        DestinationFragmentOffset += BytesThisRound;
        MaxDestinationSize -= BytesThisRound;
        if (MaxDestinationSize == 0) {

            ASSERT(DestinationFragmentOffset == DestinationFragment->Size);

            DestinationFragment += 1;
            DestinationFragmentOffset = 0;
            MaxDestinationSize = DestinationFragment->Size;
        }

        SourceFragmentOffset += BytesThisRound;
        MaxSourceSize -= BytesThisRound;
        if (MaxSourceSize == 0) {

            ASSERT(SourceFragmentOffset == SourceFragment->Size);

            SourceFragment += 1;
            SourceFragmentOffset = 0;
            MaxSourceSize = SourceFragment->Size;
        }

        ByteCount -= BytesThisRound;
    }

CopyIoBufferEnd:
    return Status;
}

KERNEL_API
KSTATUS
MmZeroIoBuffer (
    PIO_BUFFER IoBuffer,
    UINTN Offset,
    UINTN ByteCount
    )

/*++

Routine Description:

    This routine zeroes the contents of the I/O buffer starting at the offset
    for the given number of bytes.

Arguments:

    IoBuffer - Supplies a pointer to the I/O buffer that is to be zeroed.

    Offset - Supplies the offset into the I/O buffer where the zeroing
        should begin.

    ByteCount - Supplies the number of bytes to zero.

Return Value:

    Status code.

--*/

{

    UINTN CurrentOffset;
    UINTN ExtensionSize;
    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    UINTN FragmentOffset;
    KSTATUS Status;
    UINTN ZeroSize;

    Offset += IoBuffer->Internal.CurrentOffset;

    //
    // Support user mode I/O buffers if this fires and it seems useful to add.
    //

    ASSERT((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) == 0);

    //
    // If memory can be appended to the buffer and it needs to be, then extend
    // the I/O buffer.
    //

    ASSERT(((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) != 0) ||
           ((Offset + ByteCount) <= IoBuffer->Internal.TotalSize));

    if (((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) != 0) &&
        ((Offset + ByteCount) > IoBuffer->Internal.TotalSize)) {

        ExtensionSize = (Offset + ByteCount) - IoBuffer->Internal.TotalSize;
        Status = MmpExtendIoBuffer(IoBuffer,
                                   0,
                                   MAX_ULONGLONG,
                                   0,
                                   ExtensionSize,
                                   FALSE);

        if (!KSUCCESS(Status)) {
            return Status;
        }
    }

    //
    // Make sure the buffer is mapped.
    //

    Status = MmMapIoBuffer(IoBuffer, FALSE, FALSE, FALSE);
    if (!KSUCCESS(Status)) {
        return Status;
    }

    FragmentIndex = 0;
    CurrentOffset = 0;
    while (ByteCount != 0) {
        if (FragmentIndex >= IoBuffer->FragmentCount) {
            return STATUS_INCORRECT_BUFFER_SIZE;
        }

        Fragment = &(IoBuffer->Fragment[FragmentIndex]);
        FragmentIndex += 1;
        if ((CurrentOffset + Fragment->Size) <= Offset) {
            CurrentOffset += Fragment->Size;
            continue;
        }

        ZeroSize = Fragment->Size;
        FragmentOffset = 0;
        if (Offset > CurrentOffset) {
            FragmentOffset = Offset - CurrentOffset;
            ZeroSize -= FragmentOffset;
        }

        if (ZeroSize > ByteCount) {
            ZeroSize = ByteCount;
        }

        RtlZeroMemory(Fragment->VirtualAddress + FragmentOffset, ZeroSize);
        ByteCount -= ZeroSize;
        CurrentOffset += Fragment->Size;
    }

    return STATUS_SUCCESS;
}

KERNEL_API
KSTATUS
MmCopyIoBufferData (
    PIO_BUFFER IoBuffer,
    PVOID Buffer,
    UINTN Offset,
    UINTN Size,
    BOOL ToIoBuffer
    )

/*++

Routine Description:

    This routine copies from a buffer into the given I/O buffer or out of the
    given I/O buffer.

Arguments:

    IoBuffer - Supplies a pointer to the I/O buffer to copy in or out of.

    Buffer - Supplies a pointer to the regular linear buffer to copy to or from.
        This must be a kernel mode address.

    Offset - Supplies an offset in bytes from the beginning of the I/O buffer
        to copy to or from.

    Size - Supplies the number of bytes to copy.

    ToIoBuffer - Supplies a boolean indicating whether data is copied into the
        I/O buffer (TRUE) or out of the I/O buffer (FALSE).

Return Value:

    STATUS_SUCCESS on success.

    STATUS_INCORRECT_BUFFER_SIZE if the copy goes outside the I/O buffer.

    Other error codes if the I/O buffer could not be mapped.

--*/

{

    UINTN CopyOffset;
    UINTN CopySize;
    UINTN CurrentOffset;
    UINTN ExtensionSize;
    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    KSTATUS Status;

    ASSERT(Buffer >= KERNEL_VA_START);

    Offset += IoBuffer->Internal.CurrentOffset;

    //
    // If memory can be appended to the buffer and it needs to be, then extend
    // the I/O buffer.
    //

    ASSERT((ToIoBuffer != FALSE) ||
           ((Offset + Size) <= IoBuffer->Internal.TotalSize));

    ASSERT((ToIoBuffer == FALSE) ||
           ((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) != 0) ||
           ((Offset + Size) <= IoBuffer->Internal.TotalSize));

    if ((ToIoBuffer != FALSE) &&
        ((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) != 0) &&
        ((Offset + Size) > IoBuffer->Internal.TotalSize)) {

        ExtensionSize = (Offset + Size) - IoBuffer->Internal.TotalSize;
        Status = MmpExtendIoBuffer(IoBuffer,
                                   0,
                                   MAX_ULONGLONG,
                                   0,
                                   ExtensionSize,
                                   FALSE);

        if (!KSUCCESS(Status)) {
            return Status;
        }
    }

    Status = MmMapIoBuffer(IoBuffer, FALSE, FALSE, FALSE);
    if (!KSUCCESS(Status)) {
        return Status;
    }

    FragmentIndex = 0;
    CurrentOffset = 0;
    while (Size != 0) {
        if (FragmentIndex >= IoBuffer->FragmentCount) {
            return STATUS_INCORRECT_BUFFER_SIZE;
        }

        Fragment = &(IoBuffer->Fragment[FragmentIndex]);
        FragmentIndex += 1;
        if ((CurrentOffset + Fragment->Size) <= Offset) {
            CurrentOffset += Fragment->Size;
            continue;
        }

        CopySize = Fragment->Size;
        CopyOffset = 0;
        if (Offset > CurrentOffset) {
            CopyOffset = Offset - CurrentOffset;
            CopySize -= CopyOffset;
        }

        if (CopySize > Size) {
            CopySize = Size;
        }

        //
        // Copy into the I/O buffer fragment, potentially to user mode.
        //

        if (ToIoBuffer != FALSE) {
            if ((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) != 0) {
                Status = MmCopyToUserMode(Fragment->VirtualAddress + CopyOffset,
                                          Buffer,
                                          CopySize);

            } else {
                RtlCopyMemory(Fragment->VirtualAddress + CopyOffset,
                              Buffer,
                              CopySize);
            }

        //
        // Copy out of the I/O buffer fragment, potentially from user mode.
        //

        } else {
            if ((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) != 0) {
                Status = MmCopyFromUserMode(
                                         Buffer,
                                         Fragment->VirtualAddress + CopyOffset,
                                         CopySize);

            } else {
                RtlCopyMemory(Buffer,
                              Fragment->VirtualAddress + CopyOffset,
                              CopySize);
            }
        }

        if (!KSUCCESS(Status)) {
            return Status;
        }

        Size -= CopySize;
        Buffer += CopySize;
        CurrentOffset += Fragment->Size;
    }

    return STATUS_SUCCESS;
}

KERNEL_API
ULONG
MmGetIoBufferAlignment (
    VOID
    )

/*++

Routine Description:

    This routine returns the required alignment for all flush operations.

Arguments:

    None.

Return Value:

    Returns the size of a data cache line, in bytes.

--*/

{

    ULONG IoBufferAlignment;
    ULONG L1DataCacheLineSize;

    IoBufferAlignment = MmIoBufferAlignment;
    if (IoBufferAlignment == 0) {

        //
        // Take the maximum between the L1 cache and any registered cache
        // controllers.
        //

        L1DataCacheLineSize = ArGetDataCacheLineSize();
        IoBufferAlignment = HlGetDataCacheLineSize();
        if (L1DataCacheLineSize > IoBufferAlignment) {
            IoBufferAlignment = L1DataCacheLineSize;
        }

        MmIoBufferAlignment = IoBufferAlignment;
    }

    return IoBufferAlignment;
}

KERNEL_API
KSTATUS
MmValidateIoBuffer (
    PHYSICAL_ADDRESS MinimumPhysicalAddress,
    PHYSICAL_ADDRESS MaximumPhysicalAddress,
    UINTN Alignment,
    UINTN SizeInBytes,
    BOOL PhysicallyContiguous,
    PIO_BUFFER *IoBuffer
    )

/*++

Routine Description:

    This routine validates an I/O buffer for use by a device. If the I/O buffer
    does not meet the given requirements, then a new I/O buffer that meets the
    requirements will be returned. This new I/O buffer will not contain the
    same data as the originally supplied I/O buffer. It is up to the caller to
    decide which further actions need to be taken if a different buffer is
    returned.

Arguments:

    MinimumPhysicalAddress - Supplies the minimum allowed physical address for
        the I/O buffer.

    MaximumPhysicalAddress - Supplies the maximum allowed physical address for
        the I/O buffer.

    Alignment - Supplies the required physical alignment of the I/O buffer.

    SizeInBytes - Supplies the minimum required size of the buffer, in bytes.

    PhysicallyContiguous - Supplies a boolean indicating whether or not the
        I/O buffer should be physically contiguous.

    IoBuffer - Supplies a pointer to a pointer to an I/O buffer. On entry, this
        contains a pointer to the I/O buffer to be validated. On exit, it may
        point to a newly allocated I/O buffer that the caller must free.

Return Value:

    Status code.

--*/

{

    BOOL AllocateIoBuffer;
    PIO_BUFFER Buffer;
    UINTN BufferOffset;
    UINTN CurrentOffset;
    UINTN EndOffset;
    UINTN ExtensionSize;
    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    UINTN FragmentOffset;
    UINTN FragmentSize;
    PHYSICAL_ADDRESS PhysicalAddressEnd;
    PHYSICAL_ADDRESS PhysicalAddressStart;
    KSTATUS Status;

    AllocateIoBuffer = FALSE;
    Buffer = *IoBuffer;
    if (Buffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Status = STATUS_SUCCESS;

    //
    // If the I/O buffer won't be able to fit the data and it is not
    // extendable,  then do not re-allocate a different buffer, just fail.
    //

    if (((Buffer->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) == 0) &&
        ((Buffer->Internal.CurrentOffset + SizeInBytes) >
          Buffer->Internal.TotalSize)) {

        Status = STATUS_BUFFER_TOO_SMALL;
        goto ValidateIoBufferEnd;
    }

    //
    // DMA cannot be done to a user mode buffer.
    //

    if ((Buffer->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) != 0) {
        AllocateIoBuffer = TRUE;
        goto ValidateIoBufferEnd;
    }

    //
    // Validate that the physical pages starting at the I/O buffer's offset are
    // in the specified range, aligned and that they are physically contigous,
    // if necessary.
    //

    BufferOffset = Buffer->Internal.CurrentOffset;
    if (BufferOffset != Buffer->Internal.TotalSize) {
        FragmentIndex = 0;
        CurrentOffset = 0;
        EndOffset = BufferOffset + SizeInBytes;
        if (EndOffset > Buffer->Internal.TotalSize) {
            EndOffset = Buffer->Internal.TotalSize;
        }

        PhysicalAddressEnd = INVALID_PHYSICAL_ADDRESS;
        while (BufferOffset < EndOffset) {
            Fragment = &(Buffer->Fragment[FragmentIndex]);
            if (BufferOffset >= (CurrentOffset + Fragment->Size)) {
                CurrentOffset += Fragment->Size;
                FragmentIndex += 1;
                continue;
            }

            FragmentOffset = BufferOffset - CurrentOffset;
            PhysicalAddressStart = Fragment->PhysicalAddress + FragmentOffset;
            if ((PhysicallyContiguous != FALSE) &&
                (PhysicalAddressEnd != INVALID_PHYSICAL_ADDRESS)  &&
                (PhysicalAddressStart != PhysicalAddressEnd)) {

                AllocateIoBuffer = TRUE;
                goto ValidateIoBufferEnd;
            }

            FragmentSize = Fragment->Size - FragmentOffset;

            //
            // The size and physical address better be aligned.
            //

            if ((IS_ALIGNED(PhysicalAddressStart, Alignment) == FALSE) ||
                (IS_ALIGNED(FragmentSize, Alignment) == FALSE)) {

                AllocateIoBuffer = TRUE;
                goto ValidateIoBufferEnd;
            }

            PhysicalAddressEnd = PhysicalAddressStart + FragmentSize;

            ASSERT(PhysicalAddressEnd > PhysicalAddressStart);

            if ((PhysicalAddressStart < MinimumPhysicalAddress) ||
                (PhysicalAddressEnd > MaximumPhysicalAddress)) {

                AllocateIoBuffer = TRUE;
                goto ValidateIoBufferEnd;
            }

            BufferOffset += FragmentSize;
            CurrentOffset += Fragment->Size;

            ASSERT(BufferOffset == CurrentOffset);

            FragmentIndex += 1;
        }
    }

    //
    // With the existing physical pages in the right range, extend the buffer
    // if necessary and possible.
    //

    if (((Buffer->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) != 0) &&
        ((Buffer->Internal.CurrentOffset + SizeInBytes) >
         Buffer->Internal.TotalSize)) {

        //
        // If the buffer must be physically contiguous, there is no guarantee
        // the extension can satisfy that unless the current offset is at the
        // end of the existing buffer.
        //

        if ((PhysicallyContiguous != FALSE) &&
            (Buffer->Internal.CurrentOffset != Buffer->Internal.TotalSize)) {

            AllocateIoBuffer = TRUE;
            goto ValidateIoBufferEnd;
        }

        ExtensionSize = (Buffer->Internal.CurrentOffset + SizeInBytes) -
                        Buffer->Internal.TotalSize;

        Status = MmpExtendIoBuffer(Buffer,
                                   MinimumPhysicalAddress,
                                   MaximumPhysicalAddress,
                                   Alignment,
                                   ExtensionSize,
                                   PhysicallyContiguous);

        goto ValidateIoBufferEnd;
    }

ValidateIoBufferEnd:
    if (AllocateIoBuffer != FALSE) {
        Buffer = MmAllocateNonPagedIoBuffer(MinimumPhysicalAddress,
                                            MaximumPhysicalAddress,
                                            Alignment,
                                            SizeInBytes,
                                            PhysicallyContiguous,
                                            FALSE,
                                            FALSE);

        if (Buffer == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;

        } else {
            *IoBuffer = Buffer;
        }
    }

    return Status;
}

KSTATUS
MmValidateIoBufferForCachedIo (
    PIO_BUFFER *IoBuffer,
    UINTN SizeInBytes,
    UINTN Alignment
    )

/*++

Routine Description:

    This routine validates an I/O buffer for an I/O operation, potentially
    returning a new I/O buffer.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer pointer. On entry, it stores
        the pointer to the I/O buffer to evaluate. On exit, it stores a pointer
        to a valid I/O buffer, that may actually be a new I/O buffer.

    SizeInBytes - Supplies the required size of the I/O buffer.

    Alignment - Supplies the required alignment of the I/O buffer.

Return Value:

    Status code.

--*/

{

    BOOL AllocateIoBuffer;
    UINTN AvailableFragments;
    PIO_BUFFER Buffer;
    UINTN PageCount;
    ULONG PageShift;
    ULONG PageSize;
    KSTATUS Status;

    AllocateIoBuffer = FALSE;
    Buffer = *IoBuffer;
    PageShift = MmPageShift();
    PageSize = MmPageSize();
    Status = STATUS_SUCCESS;

    //
    // If no I/O buffer was supplied, it is not cached backed or the buffer
    // cannot be expanded, then a buffer needs to be allocated.
    //

    if ((Buffer == NULL) ||
        ((Buffer->Internal.Flags & IO_BUFFER_FLAG_PAGE_CACHE_BACKED) == 0) ||
        ((Buffer->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) == 0)) {

        AllocateIoBuffer = TRUE;
        goto ValidateIoBufferForCachedIoEnd;
    }

    //
    // If the I/O buffer's current offset is not aligned and at the end of the
    // buffer, then the buffer cannot be extended to directly handle the I/O.
    //

    if ((IS_ALIGNED(Buffer->Internal.CurrentOffset, Alignment) == FALSE) ||
        (Buffer->Internal.CurrentOffset != Buffer->Internal.TotalSize)) {

        AllocateIoBuffer = TRUE;
        goto ValidateIoBufferForCachedIoEnd;
    }

    //
    // Determine if the I/O buffer has enough fragments to extend into.
    //

    AvailableFragments = Buffer->Internal.MaxFragmentCount -
                         Buffer->FragmentCount;

    PageCount = ALIGN_RANGE_UP(SizeInBytes, PageSize) >> PageShift;
    if (PageCount > AvailableFragments) {
        AllocateIoBuffer = TRUE;
        goto ValidateIoBufferForCachedIoEnd;
    }

ValidateIoBufferForCachedIoEnd:
    if (AllocateIoBuffer != FALSE) {
        SizeInBytes = ALIGN_RANGE_UP(SizeInBytes, Alignment);
        Buffer = MmAllocateUninitializedIoBuffer(SizeInBytes, TRUE);
        if (Buffer == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;

        } else {
            *IoBuffer = Buffer;
        }
    }

    return Status;
}

VOID
MmIoBufferAppendPage (
    PIO_BUFFER IoBuffer,
    PVOID PageCacheEntry,
    PVOID VirtualAddress,
    PHYSICAL_ADDRESS PhysicalAddress
    )

/*++

Routine Description:

    This routine appends a page, as described by its VA/PA or page cache entry,
    to the end of the given I/O buffer. The caller should either supply a page
    cache entry or a physical address (with an optional virtual address), but
    not both.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

    PageCacheEntry - Supplies an optional pointer to the page cache entry whose
        data will be appended to the I/O buffer.

    VirtualAddress - Supplies an optional virtual address for the range.

    PhysicalAddress - Supplies the optional physical address of the data that
        is to be set in the I/O buffer at the given offset. Use
        INVALID_PHYSICAL_ADDRESS when supplying a page cache entry.

Return Value:

    None.

--*/

{

    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    UINTN PageIndex;
    ULONG PageSize;

    PageSize = MmPageSize();

    ASSERT((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) != 0);
    ASSERT((PageCacheEntry == NULL) ||
           (PhysicalAddress == INVALID_PHYSICAL_ADDRESS));

    ASSERT((PageCacheEntry == NULL) ||
           (IoBuffer->Internal.PageCacheEntries != NULL));

    //
    // There better bet at least one free fragment in case this is not
    // contiguous with the previous fragment.
    //

    ASSERT(IoBuffer->FragmentCount < IoBuffer->Internal.MaxFragmentCount);

    //
    // The current total size of the buffer better be page aligned.
    //

    ASSERT(IS_ALIGNED(IoBuffer->Internal.TotalSize, PageSize) != FALSE);

    //
    // Get the last fragment in the I/O buffer.
    //

    FragmentIndex = 0;
    if (IoBuffer->FragmentCount != 0) {
        FragmentIndex = IoBuffer->FragmentCount - 1;
    }

    //
    // If a page cache entry was supplied, use its physical and virtual
    // addresses.
    //

    if (PageCacheEntry != NULL) {
        PhysicalAddress = IoGetPageCacheEntryPhysicalAddress(PageCacheEntry);
        VirtualAddress = IoGetPageCacheEntryVirtualAddress(PageCacheEntry);
    }

    //
    // If the address is physically and virtually contiguous with the last
    // fragment, then append it there.
    //

    Fragment = &(IoBuffer->Fragment[FragmentIndex]);
    if (((Fragment->PhysicalAddress + Fragment->Size) == PhysicalAddress) &&
        (((VirtualAddress == NULL) && (Fragment->VirtualAddress == NULL)) ||
         (((VirtualAddress != NULL) && (Fragment->VirtualAddress != NULL)) &&
          ((Fragment->VirtualAddress + Fragment->Size) == VirtualAddress)))) {

        ASSERT((Fragment->Size + PageSize) > Fragment->Size);

        Fragment->Size += PageSize;

    //
    // Otherwise stick it in the next fragment.
    //

    } else {
        if (IoBuffer->FragmentCount != 0) {
            Fragment += 1;
        }

        ASSERT(Fragment->PhysicalAddress == INVALID_PHYSICAL_ADDRESS);
        ASSERT(Fragment->VirtualAddress == NULL);
        ASSERT(Fragment->Size == 0);

        Fragment->PhysicalAddress = PhysicalAddress;
        Fragment->VirtualAddress = VirtualAddress;
        Fragment->Size = PageSize;
        IoBuffer->FragmentCount += 1;
    }

    //
    // If there is a page cache entry, then stick it into the array of page
    // cache entries at the appropriate offset.
    //

    if (PageCacheEntry != NULL) {

        //
        // The fragment count should always be less than or equal to the page
        // count.
        //

        ASSERT(IoBuffer->FragmentCount <= IoBuffer->Internal.PageCount);

        PageIndex = IoBuffer->Internal.TotalSize >> MmPageShift();

        ASSERT(PageIndex < IoBuffer->Internal.PageCount);
        ASSERT(IoBuffer->Internal.PageCacheEntries[PageIndex] == NULL);
        ASSERT((IoBuffer->Internal.Flags &
                IO_BUFFER_FLAG_PAGE_CACHE_BACKED) != 0);

        IoPageCacheEntryAddReference(PageCacheEntry);
        IoBuffer->Internal.PageCacheEntries[PageIndex] = PageCacheEntry;
    }

    IoBuffer->Internal.TotalSize += PageSize;
    return;
}

VOID
MmSetIoBufferPageCacheEntry (
    PIO_BUFFER IoBuffer,
    UINTN IoBufferOffset,
    PVOID PageCacheEntry
    )

/*++

Routine Description:

    This routine sets the given page cache entry in the I/O buffer at the given
    offset. The physical address of the page cache entry should match that of
    the I/O buffer at the given offset.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

    IoBufferOffset - Supplies an offset into the given I/O buffer.

    PageCacheEntry - Supplies a pointer to the page cache entry to set.

Return Value:

    None.

--*/

{

    UINTN PageIndex;

    IoBufferOffset += IoBuffer->Internal.CurrentOffset;

    //
    // The I/O buffer offset better be page aligned.
    //

    ASSERT(IS_ALIGNED(IoBufferOffset, MmPageSize()));
    ASSERT((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) == 0);

    PageIndex = IoBufferOffset >> MmPageShift();

    //
    // The offset's page index better be valid, un-set and the physical address
    // at the given offset better match what's in the page cache entry.
    //

    ASSERT(PageIndex < IoBuffer->Internal.PageCount);
    ASSERT(IoBuffer->Internal.PageCacheEntries[PageIndex] == NULL);
    ASSERT(MmGetIoBufferPhysicalAddress(IoBuffer, IoBufferOffset) ==
           IoGetPageCacheEntryPhysicalAddress(PageCacheEntry));

    IoPageCacheEntryAddReference(PageCacheEntry);
    IoBuffer->Internal.PageCacheEntries[PageIndex] = PageCacheEntry;
    IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_PAGE_CACHE_BACKED;
    return;
}

PVOID
MmGetIoBufferPageCacheEntry (
    PIO_BUFFER IoBuffer,
    UINTN IoBufferOffset
    )

/*++

Routine Description:

    This routine returns the page cache entry associated with the given I/O
    buffer at the given offset into the buffer.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

    IoBufferOffset - Supplies an offset into the I/O buffer, in bytes.

Return Value:

    Returns a pointer to a page cache entry if the physical page at the given
    offset has been cached, or NULL otherwise.

--*/

{

    UINTN PageIndex;

    if ((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_PAGE_CACHE_BACKED) == 0) {
        return NULL;
    }

    IoBufferOffset += IoBuffer->Internal.CurrentOffset;

    //
    // The I/O buffer offset better be page aligned.
    //

    ASSERT(IS_ALIGNED(IoBufferOffset, MmPageSize()));
    ASSERT((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_USER_MODE) == 0);

    PageIndex = IoBufferOffset >> MmPageShift();

    ASSERT(PageIndex < IoBuffer->Internal.PageCount);

    return IoBuffer->Internal.PageCacheEntries[PageIndex];
}

KERNEL_API
UINTN
MmGetIoBufferSize (
    PIO_BUFFER IoBuffer
    )

/*++

Routine Description:

    This routine returns the size of the I/O buffer, in bytes.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

Return Value:

    Returns the size of the I/O buffer, in bytes.

--*/

{

    return IoBuffer->Internal.TotalSize - IoBuffer->Internal.CurrentOffset;
}

KERNEL_API
UINTN
MmGetIoBufferCurrentOffset (
    PIO_BUFFER IoBuffer
    )

/*++

Routine Description:

    This routine returns the given I/O buffer's current offset. The offset is
    the point at which all I/O should begin.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

Return Value:

    Returns the I/O buffers current offset.

--*/

{

    return IoBuffer->Internal.CurrentOffset;
}

KERNEL_API
VOID
MmIoBufferIncrementOffset (
    PIO_BUFFER IoBuffer,
    UINTN OffsetIncrement
    )

/*++

Routine Description:

    This routine increments the I/O buffer's current offset by the given amount.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

    OffsetIncrement - Supplies the number of bytes by which the offset will be
        incremented.

Return Value:

    None.

--*/

{

    IoBuffer->Internal.CurrentOffset += OffsetIncrement;

    ASSERT(IoBuffer->Internal.CurrentOffset <= IoBuffer->Internal.TotalSize);

    return;
}

KERNEL_API
VOID
MmIoBufferDecrementOffset (
    PIO_BUFFER IoBuffer,
    UINTN OffsetDecrement
    )

/*++

Routine Description:

    This routine decrements the I/O buffer's current offset by the given amount.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

    OffsetDecrement - Supplies the number of bytes by which the offset will be
        incremented.

Return Value:

    None.

--*/

{

    IoBuffer->Internal.CurrentOffset -= OffsetDecrement;

    ASSERT(IoBuffer->Internal.CurrentOffset <= IoBuffer->Internal.TotalSize);

    return;
}

PHYSICAL_ADDRESS
MmGetIoBufferPhysicalAddress (
    PIO_BUFFER IoBuffer,
    UINTN IoBufferOffset
    )

/*++

Routine Description:

    This routine returns the physical address at a given offset within an I/O
    buffer.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

    IoBufferOffset - Supplies a byte offset into the I/O buffer.

Return Value:

    Returns the physical address of the memory at the given offset within the
    I/O buffer.

--*/

{

    UINTN FragmentEnd;
    UINTN FragmentIndex;
    UINTN FragmentStart;
    PHYSICAL_ADDRESS PhysicalAddress;

    IoBufferOffset += IoBuffer->Internal.CurrentOffset;
    PhysicalAddress = INVALID_PHYSICAL_ADDRESS;
    FragmentStart = 0;
    for (FragmentIndex = 0;
         FragmentIndex < IoBuffer->FragmentCount;
         FragmentIndex += 1) {

        FragmentEnd = FragmentStart + IoBuffer->Fragment[FragmentIndex].Size;
        if ((IoBufferOffset >= FragmentStart) &&
            (IoBufferOffset < FragmentEnd)) {

            PhysicalAddress = IoBuffer->Fragment[FragmentIndex].PhysicalAddress;
            PhysicalAddress += (IoBufferOffset - FragmentStart);
            break;
        }

        FragmentStart = FragmentEnd;
    }

    return PhysicalAddress;
}

//
// --------------------------------------------------------- Internal Functions
//

VOID
MmpReleaseIoBufferResources (
    PIO_BUFFER IoBuffer
    )

/*++

Routine Description:

    This routine release all the memory resources for an I/O buffer. It does
    not release the memory allocated for the I/O buffer structure itself.

Arguments:

    IoBuffer - Supplies a pointer to the I/O buffer to release.

Return Value:

    None.

--*/

{

    UINTN CacheEntryIndex;
    ULONG Flags;
    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    PVOID *PageCacheEntries;
    PPAGE_CACHE_ENTRY PageCacheEntry;
    UINTN PageCount;
    UINTN PageIndex;
    ULONG PageOffset;
    ULONG PageShift;
    ULONG PageSize;
    PHYSICAL_ADDRESS PhysicalAddress;
    UINTN Size;

    Flags = IoBuffer->Internal.Flags;
    PageShift = MmPageShift();
    PageSize = MmPageSize();
    IoBuffer->Internal.CurrentOffset = 0;

    //
    // First unmap the I/O buffer, if necessary..
    //

    if ((Flags & IO_BUFFER_FLAG_UNMAP_ON_FREE) != 0) {
        MmpUnmapIoBuffer(IoBuffer);
    }

    //
    // Now look to free or unlock the physical pages. If the memory itself is
    // owned by the I/O buffer structure or the I/O buffer was filled in with
    // page cache entries, iterate over the I/O buffer, releasing each fragment.
    //

    if (((Flags & IO_BUFFER_FLAG_MEMORY_OWNED) != 0) ||
        ((Flags & IO_BUFFER_FLAG_PAGE_CACHE_BACKED) != 0)) {

        PageCacheEntry = NULL;
        PageCacheEntries = IoBuffer->Internal.PageCacheEntries;
        for (FragmentIndex = 0;
             FragmentIndex < IoBuffer->FragmentCount;
             FragmentIndex += 1) {

            Fragment = &(IoBuffer->Fragment[FragmentIndex]);

            //
            // There may be multiple pages within a fragment. Some might be in
            // the page cache and others may not. Iterate over the fragment
            // page by page.
            //

            ASSERT(IS_ALIGNED(Fragment->Size, PageSize) != FALSE);
            ASSERT(IS_ALIGNED(Fragment->PhysicalAddress, PageSize) != FALSE);

            PageCount = Fragment->Size >> PageShift;
            PhysicalAddress = Fragment->PhysicalAddress;
            for (PageIndex = 0; PageIndex < PageCount; PageIndex += 1) {
                if (PageCacheEntries != NULL) {
                    PageCacheEntry = *PageCacheEntries;
                    PageCacheEntries += 1;
                }

                //
                // If there is a page cache entry, do not free the page. It may
                // or may not get released when the page cache entry reference
                // is dropped.
                //

                if (PageCacheEntry != NULL) {

                    ASSERT(
                        (Fragment->PhysicalAddress + (PageIndex * PageSize)) ==
                        IoGetPageCacheEntryPhysicalAddress(PageCacheEntry));

                    IoPageCacheEntryReleaseReference(PageCacheEntry);

                //
                // If this is a regular memory-owned buffer and the page
                // wasn't borrowed by the page cache, then proceed to release
                // the physical page.
                //

                } else if ((Flags & IO_BUFFER_FLAG_MEMORY_OWNED) != 0) {
                    MmFreePhysicalPage(PhysicalAddress);

                //
                // Otherwise, this is a section of a fragment in a purely page
                // cache backed buffer that does not have a page cache entry.
                // Such a section should not exist.
                //

                } else {

                    ASSERT((Flags & IO_BUFFER_FLAG_PAGE_CACHE_BACKED) != 0);
                    ASSERT(FALSE);

                    continue;
                }

                PhysicalAddress += PageSize;
            }
        }

    //
    // If the memory is not owned by the buffer and locked, then unlock every
    // page in the buffer.
    //

    } else if (((Flags & IO_BUFFER_FLAG_MEMORY_OWNED) == 0) &&
               ((Flags & IO_BUFFER_FLAG_MEMORY_LOCKED) != 0)) {

        //
        // In the course of locking this memory, some page cache entries may
        // have been referenced and other physical pages may have been locked.
        // Loop over the buffer and decide what to do for each page.
        //

        ASSERT(IoBuffer->Internal.PageCount > 0);
        ASSERT(IoBuffer->Internal.PageCacheEntries != NULL);

        CacheEntryIndex = 0;
        PageCacheEntries = IoBuffer->Internal.PageCacheEntries;
        for (FragmentIndex = 0;
             FragmentIndex < IoBuffer->FragmentCount;
             FragmentIndex += 1) {

            //
            // The physical address of the first fragment isn't guaranteed
            // to be page aligned, so account for the page offset when
            // calculating the number of pages to unlock.
            //

            Fragment = &(IoBuffer->Fragment[FragmentIndex]);
            PageOffset = REMAINDER(Fragment->PhysicalAddress, PageSize);
            Size = Fragment->Size + PageOffset;
            Size = ALIGN_RANGE_UP(Size, PageSize);
            PageCount = Size >> PageShift;
            PhysicalAddress = Fragment->PhysicalAddress - PageOffset;
            for (PageIndex = 0; PageIndex < PageCount; PageIndex += 1) {
                PageCacheEntry = PageCacheEntries[CacheEntryIndex];
                if (PageCacheEntry != NULL) {
                    IoPageCacheEntryReleaseReference(PageCacheEntry);

                } else {
                    MmpUnlockPhysicalPages(PhysicalAddress, 1);
                }

                CacheEntryIndex += 1;
                PhysicalAddress += PageSize;
            }
        }
    }

    return;
}

KSTATUS
MmpMapIoBufferFragments (
    PIO_BUFFER IoBuffer,
    UINTN FragmentStart,
    UINTN FragmentCount,
    ULONG MapFlags
    )

/*++

Routine Description:

    This routine maps the given set of fragments within the provided I/O buffer.

Arguments:

    IoBuffer - Supplies a pointer to an I/O buffer.

    FragmentStart - Supplies the index of the first fragment to be mapped.

    FragmentCount - Supplies the number of fragments to be mapped.

    MapFlags - Supplies the map flags to use when mapping the I/O buffer. See
        MAP_FLAG_* for definitions.

Return Value:

    Status code.

--*/

{

    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    UINTN FragmentSize;
    PVOID *PageCacheEntries;
    PPAGE_CACHE_ENTRY PageCacheEntry;
    UINTN PageIndex;
    UINTN PageOffset;
    ULONG PageShift;
    ULONG PageSize;
    PHYSICAL_ADDRESS PhysicalAddress;
    UINTN Size;
    KSTATUS Status;
    PVOID VirtualAddress;

    ASSERT(FragmentCount != 0);
    ASSERT((FragmentStart + FragmentCount) <= IoBuffer->FragmentCount);

    PageShift = MmPageShift();
    PageSize = MmPageSize();

    //
    // Determine the size of the fragments to be mapped.
    //

    Size = 0;
    for (FragmentIndex = FragmentStart;
         FragmentIndex < (FragmentStart + FragmentCount);
         FragmentIndex += 1) {

        Fragment = &(IoBuffer->Fragment[FragmentIndex]);
        Size += Fragment->Size;
    }

    ASSERT(Size != 0);
    ASSERT(IS_ALIGNED(Size, PageSize) != FALSE);

    //
    // Allocate a range of virtual address space.
    //

    Status = MmpAllocateAddressRange(&MmKernelVirtualSpace,
                                     Size,
                                     PageSize,
                                     MemoryTypeReserved,
                                     AllocationStrategyAnyAddress,
                                     FALSE,
                                     &VirtualAddress);

    if (!KSUCCESS(Status)) {
        goto MapIoBufferFragmentsEnd;
    }

    ASSERT(VirtualAddress >= KERNEL_VA_START);

    //
    // Get the current page offset if this is page cache backed.
    //

    PageIndex = 0;
    PageCacheEntries = NULL;
    if ((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_PAGE_CACHE_BACKED) != 0) {

        ASSERT(IoBuffer->Internal.PageCacheEntries != NULL);

        PageCacheEntries = IoBuffer->Internal.PageCacheEntries;
        PageOffset = 0;
        for (FragmentIndex = 0;
             FragmentIndex < FragmentStart;
             FragmentIndex += 1) {

            Fragment = &(IoBuffer->Fragment[FragmentIndex]);
            PageOffset += Fragment->Size;
        }

        ASSERT(IS_ALIGNED(PageOffset, PageSize) != FALSE);

        PageIndex = PageOffset >> PageShift;
    }

    //
    // Map each fragment page by page.
    //

    for (FragmentIndex = FragmentStart;
         FragmentIndex < (FragmentStart + FragmentCount);
         FragmentIndex += 1) {

        Fragment = &(IoBuffer->Fragment[FragmentIndex]);
        Fragment->VirtualAddress = VirtualAddress;
        PhysicalAddress = Fragment->PhysicalAddress;
        FragmentSize = Fragment->Size;
        while (FragmentSize != 0) {

            //
            // The physical address and size should be page-aligned.
            //

            ASSERT(IS_ALIGNED(PhysicalAddress, PageSize) != FALSE);
            ASSERT(IS_ALIGNED(FragmentSize, PageSize) != FALSE);

            MmpMapPage(PhysicalAddress, VirtualAddress, MapFlags);

            //
            // If this page is backed by the page cache, then attempt to set
            // this VA in the page cache entry. When a page cache entry is
            // appended to an I/O buffer, the I/O buffer gets the page cache
            // entry's VA if it is mapped. Thus, if an I/O buffer fragment is
            // backed by a page cache entry and needs mapping, the page cache
            // entry is likely unmapped. So attempt to win the race to mark it
            // mapped.
            //

            if (PageCacheEntries != FALSE) {
                PageCacheEntry = PageCacheEntries[PageIndex];
                if (PageCacheEntry != NULL) {
                    IoSetPageCacheEntryVirtualAddress(PageCacheEntry,
                                                      VirtualAddress);
                }

                PageIndex += 1;
            }

            PhysicalAddress += PageSize;
            VirtualAddress += PageSize;
            FragmentSize -= PageSize;
        }
    }

MapIoBufferFragmentsEnd:
    return Status;
}

VOID
MmpUnmapIoBuffer (
    PIO_BUFFER IoBuffer
    )

/*++

Routine Description:

    This routine unmaps the given I/O buffer.

Arguments:

    IoBuffer - Supplies a pointer to the I/O buffer to be unmapped.

Return Value:

    None.

--*/

{

    BOOL CacheMatch;
    PVOID CurrentAddress;
    PVOID EndAddress;
    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    UINTN FragmentOffset;
    UINTN FragmentSize;
    PVOID PageCacheAddress;
    PVOID *PageCacheEntries;
    PPAGE_CACHE_ENTRY PageCacheEntry;
    UINTN PageCacheIndex;
    UINTN PageCount;
    UINTN PageIndex;
    ULONG PageShift;
    ULONG PageSize;
    PVOID StartAddress;
    KSTATUS Status;
    UINTN UnmapSize;
    PVOID UnmapStartAddress;

    ASSERT((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_UNMAP_ON_FREE) != 0);

    PageShift = MmPageShift();
    PageSize = MmPageSize();
    PageCacheEntries = NULL;
    if ((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_PAGE_CACHE_BACKED) != 0) {

        ASSERT(IoBuffer->Internal.PageCacheEntries != NULL);

        PageCacheEntries = IoBuffer->Internal.PageCacheEntries;
    }

    StartAddress = NULL;
    EndAddress = NULL;
    UnmapSize = 0;
    FragmentOffset = 0;
    FragmentIndex = 0;
    PageCacheIndex = 0;
    while (FragmentIndex < IoBuffer->FragmentCount) {
        Fragment = &(IoBuffer->Fragment[FragmentIndex]);

        //
        // If this fragment has no virtual address, skip it. Maybe the next
        // fragment is virtually contiguous with the last.
        //

        if (Fragment->VirtualAddress == NULL) {
            FragmentIndex += 1;
            continue;
        }

        //
        // Start by assuming there will be nothing to unmap this time around,
        // hoping that multiple fragments can be unmapped together.
        //

        UnmapStartAddress = NULL;

        //
        // If there are page cache entries to worry about, then go through the
        // current fragment page by page starting from the fragment offset.
        // This may be finishing the same fragment started the last time around.
        //

        if (PageCacheEntries != NULL) {
            FragmentSize = Fragment->Size - FragmentOffset;
            PageCount = FragmentSize >> PageShift;
            CurrentAddress = Fragment->VirtualAddress + FragmentOffset;
            for (PageIndex = 0; PageIndex < PageCount; PageIndex += 1) {

                ASSERT(PageCacheIndex < IoBuffer->Internal.PageCount);

                PageCacheEntry = PageCacheEntries[PageCacheIndex];
                FragmentOffset += PageSize;
                PageCacheIndex += 1;

                //
                // Check to see if the current virtual address matches the page
                // cache entry's virtual address.
                //

                CacheMatch = FALSE;
                if (PageCacheEntry != NULL) {
                    PageCacheAddress = IoGetPageCacheEntryVirtualAddress(
                                                               PageCacheEntry);

                    if (PageCacheAddress == CurrentAddress) {
                        CacheMatch = TRUE;
                    }
                }

                //
                // If the current virtual address needs to be unmapped, check
                // to see if it is contiguous with an existing run. If not, go
                // to unmap the existing run and set the current address as the
                // start of the next. If there is no current run, set this as
                // the beginning of the next run.
                //

                if (CacheMatch == FALSE) {
                    if (StartAddress != NULL) {
                        if (CurrentAddress != EndAddress) {
                            UnmapStartAddress = StartAddress;
                            UnmapSize = EndAddress - StartAddress;
                            StartAddress = CurrentAddress;
                            EndAddress = CurrentAddress + PageSize;
                            break;
                        }

                    } else {
                        StartAddress = CurrentAddress;
                        EndAddress = CurrentAddress;
                    }

                    EndAddress += PageSize;
                    CurrentAddress += PageSize;
                    continue;
                }

                //
                // The current virtual address is owned by the page cache. It
                // should not be unmapped. So if there is an existing run of
                // memory to unmap, go to unmap it. And don't start a new run.
                // Otherwise just move to the next virtual address.
                //

                ASSERT(CacheMatch != FALSE);

                if (StartAddress != NULL) {
                    UnmapStartAddress = StartAddress;
                    UnmapSize = EndAddress - StartAddress;
                    StartAddress = NULL;
                    break;
                }

                CurrentAddress += PageSize;
            }

            //
            // If the whole fragment was processed, move to the next fragment.
            //

            if (FragmentOffset >= Fragment->Size) {
                FragmentOffset = 0;
                FragmentIndex += 1;
            }

        //
        // If the buffer is not backed by page cache entries, treat the
        // fragment as a whole to be unmapped. If it's contiguous with the
        // current run of VA's, add it. Otherwise set it to start a new run and
        // mark the current run to be unmapped.
        //

        } else {
            if ((StartAddress != NULL) &&
                (Fragment->VirtualAddress != EndAddress)) {

                UnmapStartAddress = StartAddress;
                UnmapSize = EndAddress - StartAddress;
                StartAddress = NULL;
            }

            if (StartAddress == NULL) {
                StartAddress = Fragment->VirtualAddress;
                EndAddress = Fragment->VirtualAddress;
            }

            EndAddress += Fragment->Size;
            FragmentIndex += 1;
        }

        //
        // If there is something to unmap this time around, do the unmapping.
        //

        if (UnmapStartAddress != NULL) {

            ASSERT(UnmapSize != 0);

            //
            // This routine can fail if the system can no longer allocate
            // memory descriptors. Leak the VA. Not much callers can really
            // do.
            //

            Status = MmpFreeAccountingRange(NULL,
                                            &MmKernelVirtualSpace,
                                            UnmapStartAddress,
                                            UnmapSize,
                                            FALSE,
                                            UNMAP_FLAG_SEND_INVALIDATE_IPI);

            ASSERT(KSUCCESS(Status));
        }
    }

    //
    // There may be one last remaining sequence to be unmapped. Do it now.
    //

    if (StartAddress != NULL) {
        UnmapSize = EndAddress - StartAddress;

        //
        // This routine can fail if the system can no longer allocate
        // memory descriptors. Leak the VA. Not much callers can really
        // do.
        //

        Status = MmpFreeAccountingRange(NULL,
                                        &MmKernelVirtualSpace,
                                        StartAddress,
                                        UnmapSize,
                                        FALSE,
                                        UNMAP_FLAG_SEND_INVALIDATE_IPI);

        ASSERT(KSUCCESS(Status));
    }

    IoBuffer->Internal.Flags &= ~(IO_BUFFER_FLAG_MAPPED |
                                  IO_BUFFER_FLAG_UNMAP_ON_FREE |
                                  IO_BUFFER_FLAG_VIRTUALLY_CONTIGUOUS);

    return;
}

BOOL
MmpIsIoBufferMapped (
    PIO_BUFFER IoBuffer,
    BOOL VirtuallyContiguous
    )

/*++

Routine Description:

    This routine determines if each fragment of the I/O buffer is mapped.

Arguments:

    IoBuffer - Supplies a pointer to the I/O buffer to evaluate.

    VirtuallyContiguous - Supplies a boolean indicating whether or not the I/O
        buffer needs to be virtually contiguous.

Return Value:

    Returns TRUE if the I/O buffer is mapped appropriately or FALSE otherwise.

--*/

{

    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    PVOID VirtualAddress;

    ASSERT(IoBuffer->FragmentCount >= 1);

    VirtualAddress = IoBuffer->Fragment[0].VirtualAddress;
    for (FragmentIndex = 0;
         FragmentIndex < IoBuffer->FragmentCount;
         FragmentIndex += 1) {

        Fragment = &(IoBuffer->Fragment[FragmentIndex]);
        if ((Fragment->VirtualAddress == NULL) ||
            ((VirtuallyContiguous != FALSE) &&
             (VirtualAddress != Fragment->VirtualAddress))) {

            return FALSE;
        }

        VirtualAddress += Fragment->Size;
    }

    return TRUE;
}

KSTATUS
MmpExtendIoBuffer (
    PIO_BUFFER IoBuffer,
    PHYSICAL_ADDRESS MinimumPhysicalAddress,
    PHYSICAL_ADDRESS MaximumPhysicalAddress,
    UINTN Alignment,
    UINTN Size,
    BOOL PhysicallyContiguous
    )

/*++

Routine Description:

    This routine extends the given I/O buffer by allocating physical pages and
    appending them to the last active fragment or the inactive fragments.

Arguments:

    IoBuffer - Supplies a pointer to the I/O buffer to extend.

    MinimumPhysicalAddress - Supplies the minimum physical address of the
        extension.

    MaximumPhysicalAddress - Supplies the maximum physical address of the
        extension.

    Alignment - Supplies the required physical alignment.

    Size - Supplies the number of bytes by which the I/O buffer needs to be
        extended.

    PhysicallyContiguous - Supplies a boolean indicating whether or not the
        pages allocated for the extension should be physically contiguous.

Return Value:

    Status code.

--*/

{

    UINTN AvailableFragments;
    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    UINTN PageCount;
    UINTN PageIndex;
    ULONG PageShift;
    ULONG PageSize;
    PHYSICAL_ADDRESS PhysicalAddress;
    KSTATUS Status;

    ASSERT((IoBuffer->Internal.Flags & IO_BUFFER_FLAG_EXTENDABLE) != 0);

    PageShift = MmPageShift();
    PageSize = MmPageSize();

    //
    // TODO: Implement support for honoring the minimum and maximum physical
    // addresses in I/O buffers.
    //

    ASSERT((MinimumPhysicalAddress == 0) &&
           ((MaximumPhysicalAddress == MAX_ULONG) ||
            (MaximumPhysicalAddress == MAX_ULONGLONG)));

    //
    // Protect against an extension that the I/O buffer cannot accomodate.
    // Assume the worst case in that each new page needs its own fragment.
    //

    AvailableFragments = IoBuffer->Internal.MaxFragmentCount -
                         IoBuffer->FragmentCount;

    PageCount = ALIGN_RANGE_UP(Size, PageSize) >> PageShift;
    if (PageCount > AvailableFragments) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    //
    // The new pages always get attached to the last fragment or set in the
    // next fragment.
    //

    FragmentIndex = IoBuffer->FragmentCount;
    if (FragmentIndex != 0) {
        FragmentIndex -=1 ;
    }

    Fragment = &(IoBuffer->Fragment[FragmentIndex]);

    //
    // If the extension needs to be physically contiguous, allocate the pages
    // and then either append them to the current fragment or add them to the
    // next fragment.
    //

    if (PhysicallyContiguous != FALSE) {
        PhysicalAddress = MmpAllocatePhysicalPages(PageCount, Alignment);
        if (PhysicalAddress == INVALID_PHYSICAL_ADDRESS) {
            Status = STATUS_NO_MEMORY;
            goto ExtendIoBufferEnd;
        }

        if ((Fragment->VirtualAddress == NULL) &&
            ((Fragment->PhysicalAddress + Fragment->Size) ==
             PhysicalAddress)) {

            ASSERT(Fragment->Size != 0);

            Fragment->Size += PageCount << PageShift;

        } else {
            if (IoBuffer->FragmentCount != 0) {
                FragmentIndex += 1;
                Fragment += 1;
            }

            ASSERT(FragmentIndex < IoBuffer->Internal.MaxFragmentCount);
            ASSERT(Fragment->VirtualAddress == NULL);
            ASSERT(Fragment->PhysicalAddress == INVALID_PHYSICAL_ADDRESS);
            ASSERT(Fragment->Size == 0);

            Fragment->PhysicalAddress = PhysicalAddress;
            Fragment->Size = PageCount << PageShift;
            IoBuffer->FragmentCount += 1;
        }

        IoBuffer->Internal.TotalSize += PageCount << PageShift;

    //
    // Otherwise extend the I/O buffer by allocating enough pages to cover the
    // requested size and appending them to the end of the fragment array.
    //

    } else {
        for (PageIndex = 0; PageIndex < PageCount; PageIndex += 1) {
            PhysicalAddress = MmpAllocatePhysicalPages(1, Alignment);
            if (PhysicalAddress == INVALID_PHYSICAL_ADDRESS) {
                Status = STATUS_NO_MEMORY;
                goto ExtendIoBufferEnd;
            }

            //
            // Check to see if the physical page can be attached to the current
            // fragment.
            //

            if ((Fragment->VirtualAddress == NULL) &&
                ((Fragment->PhysicalAddress + Fragment->Size) ==
                 PhysicalAddress)) {

                ASSERT(Fragment->Size != 0);

                Fragment->Size += PageSize;

            } else {
                if (IoBuffer->FragmentCount != 0) {
                    FragmentIndex += 1;
                    Fragment += 1;
                }

                ASSERT(FragmentIndex < IoBuffer->Internal.MaxFragmentCount);
                ASSERT(Fragment->VirtualAddress == NULL);
                ASSERT(Fragment->PhysicalAddress == INVALID_PHYSICAL_ADDRESS);
                ASSERT(Fragment->Size == 0);

                Fragment->PhysicalAddress = PhysicalAddress;
                Fragment->Size = PageSize;
                IoBuffer->FragmentCount += 1;
            }

            IoBuffer->Internal.TotalSize += PageSize;
        }
    }

    //
    // This extension is not mapped, which means the whole buffer is no longer
    // mapped. Unset the flag. Also, the I/O buffer now contains physical pages
    // that need to be freed on release; note that as well.
    //

    IoBuffer->Internal.Flags &= ~IO_BUFFER_FLAG_MAPPED;
    IoBuffer->Internal.Flags |= IO_BUFFER_FLAG_MEMORY_OWNED;
    Status = STATUS_SUCCESS;

ExtendIoBufferEnd:
    return Status;
}

#pragma once

#include <cstdint>
#include <string>

namespace GuestAddressSpace
{
enum class FailureOperation : uint32_t
{
    None, ReservePreferred, ReserveAny, SplitReservation, CreateBacking,
    ResizeBacking, MapView, ProtectNull
};
// POD storage is available during static Memory construction, before logging.
struct FailureMemoryStatus
{
    uint32_t valid;
    uint32_t error;
    uint32_t loadPercent;
    uint64_t totalPhysical;
    uint64_t availablePhysical;
    uint64_t totalCommit;
    uint64_t availableCommit;
    uint64_t totalVirtual;
    uint64_t availableVirtual;
};
struct FailureInfo
{
    FailureOperation operation;
    uint32_t error;
    uint32_t preferredReservationError;
    int32_t viewIndex;
    uintptr_t address;
    uint64_t size;
    uint64_t offset;
    // Captured at failure, before cleanup or main/logging initialization.
    // utcFileTime uses Windows FILETIME units (100 ns since 1601-01-01 UTC).
    uint64_t utcFileTime;
    uint64_t uptimeMilliseconds;
    uint32_t threadId;
    uint32_t flags;
    uint32_t protection;
    uintptr_t processHandle;
    uintptr_t backingHandle;
    FailureMemoryStatus memory;
};
FailureInfo GetFailureInfo();
const char* FailureOperationName(FailureOperation operation);
const char* FailureApiName(FailureOperation operation);
// A and C alias the same 512 MiB; E starts one physical page later.
// Keep these OS mappings coherent even for direct recompiled base+address loads.
uint8_t* Allocate();
void Release(uint8_t* base);

// Steprant 2.4 (Android): the virtual view (0 - 0xA0000000) has no alias, so
// Allocate maps it as private anonymous memory, which the kernel can back
// with transparent huge pages; the physical views stay one shared backing.
// LO_GUEST_THP=0 keeps the 2.2 mapping. AdviseHugePages asks for huge pages
// on the 2 MiB-aligned part of a virtual range and returns the bytes advised
// (0 elsewhere, or when the view is not anonymous). DescribeHugePages gives
// the kernel's THP settings and the process's huge-page total for the log.
bool VirtualViewAnonymous();
uint64_t AdviseHugePages(uint8_t* base, uint32_t guestAddress, uint64_t size);
std::string DescribeHugePages();
}

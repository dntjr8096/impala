//===- SectionMemoryManager.cpp - Memory manager for MCJIT/RtDyld *- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the section-based memory manager used by the MCJIT
// execution engine and RuntimeDyld
//
//===----------------------------------------------------------------------===//
// Impala: Copied from the LLVM project to customize private portions of the
// implementation.

#include "SectionMemoryManager.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Process.h"

#include "common/logging.h"

namespace impala {

// ---- Impala: llvm/llvm-project#71968 ----
bool SectionMemoryManager::hasSpace(const MemoryGroup &MemGroup,
                                    uintptr_t Size) const {
  for (const FreeMemBlock &FreeMB : MemGroup.FreeMem) {
    if (FreeMB.Free.size() >= Size)
      return true;
  }
  return false;
}

static uintptr_t alignTo(uintptr_t Size, uint32_t Alignment) {
  return (Size + Alignment - 1) & ~(uintptr_t)(Alignment - 1);
}

static uint32_t checkAlignment(uint32_t Alignment, unsigned PageSize) {
  DCHECK_GT(Alignment, 0);
  DCHECK(!(Alignment & (Alignment - 1))) << "Alignment must be a power of two.";
  DCHECK_LT(Alignment, PageSize);
  // Code alignment needs to be at least the stub alignment - however, we
  // don't have an easy way to get that here so as a workaround, we assume
  // it's 8, which is the largest value I observed across all platforms.
  constexpr uint32_t StubAlign = 8;
  return std::max(Alignment, StubAlign);
}

void SectionMemoryManager::reserveAllocationSpace(
    uintptr_t CodeSize, uint32_t CodeAlign, uintptr_t RODataSize, uint32_t RODataAlign,
    uintptr_t RWDataSize, uint32_t RWDataAlign) {
  if (CodeSize == 0 && RODataSize == 0 && RWDataSize == 0) return;

  static const unsigned PageSize = sys::Process::getPageSize();

  CodeAlign = checkAlignment(CodeAlign, PageSize);
  RODataAlign = checkAlignment(RODataAlign, PageSize);
  RWDataAlign = checkAlignment(RWDataAlign, PageSize);

  // Get space required for each section. Use the same calculation as
  // allocateSection because we need to be able to satisfy it.
  uintptr_t RequiredCodeSize = alignTo(CodeSize, CodeAlign) + CodeAlign;
  uintptr_t RequiredRODataSize = alignTo(RODataSize, RODataAlign) + RODataAlign;
  uintptr_t RequiredRWDataSize = alignTo(RWDataSize, RWDataAlign) + RWDataAlign;

  if (hasSpace(CodeMem, RequiredCodeSize) &&
      hasSpace(RODataMem, RequiredRODataSize) &&
      hasSpace(RWDataMem, RequiredRWDataSize)) {
    // Sufficient space in contiguous block already available.
    return;
  }

  // MemoryManager does not have functions for releasing memory after it's
  // allocated. Normally it tries to use any excess blocks that were allocated
  // due to page alignment, but if we have insufficient free memory for the
  // request this can lead to allocating disparate memory that can violate the
  // ARM ABI. Clear free memory so only the new allocations are used, but do
  // not release allocated memory as it may still be in-use.
  CodeMem.FreeMem.clear();
  RODataMem.FreeMem.clear();
  RWDataMem.FreeMem.clear();

  // Round up to the nearest page size. Blocks must be page-aligned.
  RequiredCodeSize = alignTo(RequiredCodeSize, PageSize);
  RequiredRODataSize = alignTo(RequiredRODataSize, PageSize);
  RequiredRWDataSize = alignTo(RequiredRWDataSize, PageSize);
  uintptr_t RequiredSize = RequiredCodeSize + RequiredRODataSize + RequiredRWDataSize;

  std::error_code ec;
  sys::MemoryBlock MB = sys::Memory::allocateMappedMemory(RequiredSize, nullptr,
      sys::Memory::MF_READ | sys::Memory::MF_WRITE, ec);
  if (ec) {
    return;
  }
  // Request is page-aligned, so we should always get back exactly the request.
  DCHECK_EQ(MB.size(), RequiredSize);
  // CodeMem will arbitrarily own this MemoryBlock to handle cleanup.
  CodeMem.AllocatedMem.push_back(MB);
  uintptr_t Addr = (uintptr_t)MB.base();
  FreeMemBlock FreeMB;
  FreeMB.PendingPrefixIndex = (unsigned)-1;

  if (CodeSize > 0) {
    DCHECK_EQ(Addr, alignTo(Addr, CodeAlign));
    FreeMB.Free = sys::MemoryBlock((void*)Addr, RequiredCodeSize);
    CodeMem.FreeMem.push_back(FreeMB);
    Addr += RequiredCodeSize;
  }

  if (RODataSize > 0) {
    DCHECK_EQ(Addr, alignTo(Addr, RODataAlign));
    FreeMB.Free = sys::MemoryBlock((void*)Addr, RequiredRODataSize);
    RODataMem.FreeMem.push_back(FreeMB);
    Addr += RequiredRODataSize;
  }

  if (RWDataSize > 0) {
    DCHECK_EQ(Addr, alignTo(Addr, RWDataAlign));
    FreeMB.Free = sys::MemoryBlock((void*)Addr, RequiredRWDataSize);
    RWDataMem.FreeMem.push_back(FreeMB);
  }
}
// ---- End Impala changes ----

uint8_t *SectionMemoryManager::allocateDataSection(uintptr_t Size,
                                                   unsigned Alignment,
                                                   unsigned SectionID,
                                                   StringRef SectionName,
                                                   bool IsReadOnly) {
  if (IsReadOnly)
    return allocateSection(RODataMem, Size, Alignment);
  return allocateSection(RWDataMem, Size, Alignment);
}

uint8_t *SectionMemoryManager::allocateCodeSection(uintptr_t Size,
                                                   unsigned Alignment,
                                                   unsigned SectionID,
                                                   StringRef SectionName) {
  return allocateSection(CodeMem, Size, Alignment);
}

uint8_t *SectionMemoryManager::allocateSection(MemoryGroup &MemGroup,
                                               uintptr_t Size,
                                               unsigned Alignment) {
  if (!Alignment)
    Alignment = 16;

  assert(!(Alignment & (Alignment - 1)) && "Alignment must be a power of two.");

  uintptr_t RequiredSize = Alignment * ((Size + Alignment - 1)/Alignment + 1);
  uintptr_t Addr = 0;

  // Look in the list of free memory regions and use a block there if one
  // is available.
  for (FreeMemBlock &FreeMB : MemGroup.FreeMem) {
    if (FreeMB.Free.size() >= RequiredSize) {
      Addr = (uintptr_t)FreeMB.Free.base();
      uintptr_t EndOfBlock = Addr + FreeMB.Free.size();
      // Align the address.
      Addr = (Addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);

      if (FreeMB.PendingPrefixIndex == (unsigned)-1) {
        // The part of the block we're giving out to the user is now pending
        MemGroup.PendingMem.push_back(sys::MemoryBlock((void *)Addr, Size));

        // Remember this pending block, such that future allocations can just
        // modify it rather than creating a new one
        FreeMB.PendingPrefixIndex = MemGroup.PendingMem.size() - 1;
      } else {
        sys::MemoryBlock &PendingMB = MemGroup.PendingMem[FreeMB.PendingPrefixIndex];
        PendingMB = sys::MemoryBlock(PendingMB.base(), Addr + Size - (uintptr_t)PendingMB.base());
      }

      // Remember how much free space is now left in this block
      FreeMB.Free = sys::MemoryBlock((void *)(Addr + Size), EndOfBlock - Addr - Size);
      return (uint8_t*)Addr;
    }
  }

  // No pre-allocated free block was large enough. Allocate a new memory region.
  // Note that all sections get allocated as read-write.  The permissions will
  // be updated later based on memory group.
  //
  // FIXME: It would be useful to define a default allocation size (or add
  // it as a constructor parameter) to minimize the number of allocations.
  //
  // FIXME: Initialize the Near member for each memory group to avoid
  // interleaving.
  std::error_code ec;
  sys::MemoryBlock MB = sys::Memory::allocateMappedMemory(RequiredSize,
                                                          &MemGroup.Near,
                                                          sys::Memory::MF_READ |
                                                            sys::Memory::MF_WRITE,
                                                          ec);
  if (ec) {
    // FIXME: Add error propagation to the interface.
    return nullptr;
  }

  // Save this address as the basis for our next request
  MemGroup.Near = MB;

  // Remember that we allocated this memory
  MemGroup.AllocatedMem.push_back(MB);
  Addr = (uintptr_t)MB.base();
  uintptr_t EndOfBlock = Addr + MB.size();

  // Align the address.
  Addr = (Addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);

  // The part of the block we're giving out to the user is now pending
  MemGroup.PendingMem.push_back(sys::MemoryBlock((void *)Addr, Size));

  // The allocateMappedMemory may allocate much more memory than we need. In
  // this case, we store the unused memory as a free memory block.
  unsigned FreeSize = EndOfBlock-Addr-Size;
  if (FreeSize > 16) {
    FreeMemBlock FreeMB;
    FreeMB.Free = sys::MemoryBlock((void*)(Addr + Size), FreeSize);
    FreeMB.PendingPrefixIndex = (unsigned)-1;
    MemGroup.FreeMem.push_back(FreeMB);
  }

  // Return aligned address
  return (uint8_t*)Addr;
}

bool SectionMemoryManager::finalizeMemory(std::string *ErrMsg)
{
  // FIXME: Should in-progress permissions be reverted if an error occurs?
  std::error_code ec;

  // Make code memory executable.
  ec = applyMemoryGroupPermissions(CodeMem,
                                   sys::Memory::MF_READ | sys::Memory::MF_EXEC);
  if (ec) {
    if (ErrMsg) {
      *ErrMsg = ec.message();
    }
    return true;
  }

  // Make read-only data memory read-only.
  ec = applyMemoryGroupPermissions(RODataMem, sys::Memory::MF_READ);
  if (ec) {
    if (ErrMsg) {
      *ErrMsg = ec.message();
    }
    return true;
  }

  // Read-write data memory already has the correct permissions

  // Some platforms with separate data cache and instruction cache require
  // explicit cache flush, otherwise JIT code manipulations (like resolved
  // relocations) will get to the data cache but not to the instruction cache.
  invalidateInstructionCache();

  return false;
}

static sys::MemoryBlock trimBlockToPageSize(sys::MemoryBlock M) {
  static const size_t PageSize = sys::Process::getPageSize();

  size_t StartOverlap =
      (PageSize - ((uintptr_t)M.base() % PageSize)) % PageSize;

  size_t TrimmedSize = M.size();
  TrimmedSize -= StartOverlap;
  TrimmedSize -= TrimmedSize % PageSize;

  sys::MemoryBlock Trimmed((void *)((uintptr_t)M.base() + StartOverlap), TrimmedSize);

  assert(((uintptr_t)Trimmed.base() % PageSize) == 0);
  assert((Trimmed.size() % PageSize) == 0);
  assert(M.base() <= Trimmed.base() && Trimmed.size() <= M.size());

  return Trimmed;
}


std::error_code
SectionMemoryManager::applyMemoryGroupPermissions(MemoryGroup &MemGroup,
                                                  unsigned Permissions) {
  for (sys::MemoryBlock &MB : MemGroup.PendingMem)
    if (std::error_code EC = sys::Memory::protectMappedMemory(MB, Permissions))
      return EC;

  MemGroup.PendingMem.clear();

  // Now go through free blocks and trim any of them that don't span the entire
  // page because one of the pending blocks may have overlapped it.
  for (FreeMemBlock &FreeMB : MemGroup.FreeMem) {
    FreeMB.Free = trimBlockToPageSize(FreeMB.Free);
    // We cleared the PendingMem list, so all these pointers are now invalid
    FreeMB.PendingPrefixIndex = (unsigned)-1;
  }

  // Remove all blocks which are now empty
  MemGroup.FreeMem.erase(
      remove_if(MemGroup.FreeMem,
                [](FreeMemBlock &FreeMB) { return FreeMB.Free.size() == 0; }),
      MemGroup.FreeMem.end());

  return std::error_code();
}

void SectionMemoryManager::invalidateInstructionCache() {
  for (sys::MemoryBlock &Block : CodeMem.PendingMem)
    sys::Memory::InvalidateInstructionCache(Block.base(), Block.size());
}

SectionMemoryManager::~SectionMemoryManager() {
  for (MemoryGroup *Group : {&CodeMem, &RWDataMem, &RODataMem}) {
    for (sys::MemoryBlock &Block : Group->AllocatedMem)
      sys::Memory::releaseMappedMemory(Block);
  }
}

} // namespace impala

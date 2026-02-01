/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#pragma once

#include "helper.h"

class VioGpuShmemAllocator
{
  public:
    VioGpuShmemAllocator();
    void Init(ULONGLONG size);
    void Reset();
    bool Allocate(ULONGLONG size, ULONGLONG alignment, ULONGLONG *offset);
    void Free(ULONGLONG offset, ULONGLONG size);

    ULONGLONG GetTotal() const
    {
        return m_total;
    }

  private:
    struct FreeBlock
    {
        LIST_ENTRY entry;
        ULONGLONG offset;
        ULONGLONG size;
    };

    static ULONGLONG AlignUp(ULONGLONG value, ULONGLONG alignment);
    static ULONGLONG AlignDown(ULONGLONG value, ULONGLONG alignment);

    FreeBlock *AllocateBlock();
    void FreeBlockNode(FreeBlock *block);

    void InsertFreeBlockLocked(FreeBlock *block);
    bool FindBestFitLocked(ULONGLONG size,
                           ULONGLONG alignment,
                           FreeBlock **out_block,
                           ULONGLONG *out_aligned);

    KSPIN_LOCK m_lock;
    LIST_ENTRY m_free_list;
    ULONGLONG m_total;
    bool m_initialized;
};

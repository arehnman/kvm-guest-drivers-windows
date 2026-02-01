/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#include "viogpu_shmem_allocator.h"

static const ULONGLONG kMaxShmemSize = 16ull * 1024 * 1024 * 1024;

VioGpuShmemAllocator::VioGpuShmemAllocator() : m_total(0), m_initialized(false)
{
    KeInitializeSpinLock(&m_lock);
    InitializeListHead(&m_free_list);
}

ULONGLONG VioGpuShmemAllocator::AlignUp(ULONGLONG value, ULONGLONG alignment)
{
    if (alignment == 0)
    {
        return value;
    }
    if (alignment & (alignment - 1))
    {
        return value;
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

ULONGLONG VioGpuShmemAllocator::AlignDown(ULONGLONG value, ULONGLONG alignment)
{
    if (alignment == 0)
    {
        return value;
    }
    if (alignment & (alignment - 1))
    {
        return value;
    }
    return value & ~(alignment - 1);
}

VioGpuShmemAllocator::FreeBlock *VioGpuShmemAllocator::AllocateBlock()
{
    return (FreeBlock *)ExAllocatePoolUninitialized(NonPagedPoolNx, sizeof(FreeBlock), VIOGPUTAG);
}

void VioGpuShmemAllocator::FreeBlockNode(FreeBlock *block)
{
    if (block)
    {
        ExFreePoolWithTag(block, VIOGPUTAG);
    }
}

void VioGpuShmemAllocator::Reset()
{
    NT_ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);

    while (!IsListEmpty(&m_free_list))
    {
        PLIST_ENTRY entry = RemoveHeadList(&m_free_list);
        FreeBlock *block = CONTAINING_RECORD(entry, FreeBlock, entry);
        FreeBlockNode(block);
    }

    m_total = 0;
    m_initialized = false;

    KeReleaseSpinLock(&m_lock, oldIrql);
}

void VioGpuShmemAllocator::Init(ULONGLONG size)
{
    NT_ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    ULONGLONG clamped = size;
    if (clamped > kMaxShmemSize)
    {
        clamped = kMaxShmemSize;
    }
    clamped = AlignDown(clamped, PAGE_SIZE);

    Reset();

    if (clamped == 0)
    {
        return;
    }

    FreeBlock *block = AllocateBlock();
    if (!block)
    {
        return;
    }

    block->offset = 0;
    block->size = clamped;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    InsertTailList(&m_free_list, &block->entry);
    m_total = clamped;
    m_initialized = true;
    KeReleaseSpinLock(&m_lock, oldIrql);
}

void VioGpuShmemAllocator::InsertFreeBlockLocked(FreeBlock *block)
{
    PLIST_ENTRY entry = m_free_list.Flink;
    while (entry != &m_free_list)
    {
        FreeBlock *cur = CONTAINING_RECORD(entry, FreeBlock, entry);
        if (block->offset < cur->offset)
        {
            break;
        }
        entry = entry->Flink;
    }
    InsertTailList(entry, &block->entry);

    FreeBlock *prev = NULL;
    if (block->entry.Blink != &m_free_list)
    {
        prev = CONTAINING_RECORD(block->entry.Blink, FreeBlock, entry);
    }
    if (prev && prev->offset + prev->size == block->offset)
    {
        prev->size += block->size;
        RemoveEntryList(&block->entry);
        FreeBlockNode(block);
        block = prev;
    }

    FreeBlock *next = NULL;
    if (block->entry.Flink != &m_free_list)
    {
        next = CONTAINING_RECORD(block->entry.Flink, FreeBlock, entry);
    }
    if (next && block->offset + block->size == next->offset)
    {
        block->size += next->size;
        RemoveEntryList(&next->entry);
        FreeBlockNode(next);
    }
}

bool VioGpuShmemAllocator::FindBestFitLocked(ULONGLONG size,
                                             ULONGLONG alignment,
                                             FreeBlock **out_block,
                                             ULONGLONG *out_aligned)
{
    FreeBlock *best = NULL;
    ULONGLONG best_waste = 0;
    ULONGLONG best_aligned = 0;

    for (PLIST_ENTRY entry = m_free_list.Flink; entry != &m_free_list; entry = entry->Flink)
    {
        FreeBlock *block = CONTAINING_RECORD(entry, FreeBlock, entry);
        ULONGLONG block_start = block->offset;
        ULONGLONG block_end = block_start + block->size;
        if (block_end < block_start)
        {
            continue;
        }

        ULONGLONG aligned = AlignUp(block_start, alignment);
        if (aligned < block_start)
        {
            continue;
        }

        ULONGLONG end = aligned + size;
        if (end < aligned || end > block_end)
        {
            continue;
        }

        ULONGLONG waste = (aligned - block_start) + (block_end - end);
        if (!best || waste < best_waste || (waste == best_waste && block->size < best->size))
        {
            best = block;
            best_waste = waste;
            best_aligned = aligned;
            if (waste == 0)
            {
                break;
            }
        }
    }

    if (!best)
    {
        return false;
    }

    *out_block = best;
    *out_aligned = best_aligned;
    return true;
}

bool VioGpuShmemAllocator::Allocate(ULONGLONG size, ULONGLONG alignment, ULONGLONG *offset)
{
    NT_ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    if (!offset || size == 0)
    {
        return false;
    }

    if (alignment == 0 || alignment < PAGE_SIZE || (alignment & (alignment - 1)))
    {
        alignment = PAGE_SIZE;
    }

    size = AlignUp(size, PAGE_SIZE);

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);

    if (!m_initialized || m_total == 0)
    {
        KeReleaseSpinLock(&m_lock, oldIrql);
        return false;
    }

    FreeBlock *block = NULL;
    ULONGLONG aligned = 0;
    if (!FindBestFitLocked(size, alignment, &block, &aligned))
    {
        KeReleaseSpinLock(&m_lock, oldIrql);
        return false;
    }

    RemoveEntryList(&block->entry);

    ULONGLONG block_start = block->offset;
    ULONGLONG block_end = block_start + block->size;
    ULONGLONG alloc_end = aligned + size;
    ULONGLONG front_size = aligned - block_start;
    ULONGLONG tail_size = block_end - alloc_end;

    if (front_size > 0 && tail_size > 0)
    {
        FreeBlock *tail = AllocateBlock();
        if (!tail)
        {
            block->offset = block_start;
            block->size = block_end - block_start;
            InsertFreeBlockLocked(block);
            KeReleaseSpinLock(&m_lock, oldIrql);
            return false;
        }

        block->offset = block_start;
        block->size = front_size;
        InsertFreeBlockLocked(block);

        tail->offset = alloc_end;
        tail->size = tail_size;
        InsertFreeBlockLocked(tail);
    }
    else if (front_size > 0)
    {
        block->offset = block_start;
        block->size = front_size;
        InsertFreeBlockLocked(block);
    }
    else if (tail_size > 0)
    {
        block->offset = alloc_end;
        block->size = tail_size;
        InsertFreeBlockLocked(block);
    }
    else
    {
        FreeBlockNode(block);
    }

    *offset = aligned;
    KeReleaseSpinLock(&m_lock, oldIrql);
    return true;
}

void VioGpuShmemAllocator::Free(ULONGLONG offset, ULONGLONG size)
{
    NT_ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    if (size == 0)
    {
        return;
    }

    if ((offset & (PAGE_SIZE - 1)) != 0 || (size & (PAGE_SIZE - 1)) != 0)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);

    if (!m_initialized || m_total == 0)
    {
        KeReleaseSpinLock(&m_lock, oldIrql);
        return;
    }

    if (offset >= m_total || size > (m_total - offset))
    {
        KeReleaseSpinLock(&m_lock, oldIrql);
        return;
    }

    FreeBlock *block = AllocateBlock();
    if (!block)
    {
        KeReleaseSpinLock(&m_lock, oldIrql);
        return;
    }

    block->offset = offset;
    block->size = size;
    InsertFreeBlockLocked(block);

    KeReleaseSpinLock(&m_lock, oldIrql);
}

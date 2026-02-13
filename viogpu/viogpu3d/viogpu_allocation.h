/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#pragma once
#include "helper.h"
#include "virgl_hw.h"
#include "viogpum.h"

class VioGpuAdapter;
class VioGpuDevice;

class VioGpuResource
{
  public:
  private:
};

class VioGpuAllocation
{
  public:
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_CREATE_ALLOCATION_EXCHANGE *exchange);
    ~VioGpuAllocation(void);

    UINT GetId(void)
    {
        return m_Id;
    }

    void MarkBusy();
    void UnmarkBusy();

    void SetDxPhysicalAddress(size_t DxPhysicalAddress)
    {
        m_DxPhysicalAddress = DxPhysicalAddress;
    };

    size_t GetDxPhysicalAddress()
    {
        return m_DxPhysicalAddress;
    };

    BOOL IsCoherent()
    {
        return (m_options.flags & VIRGL_RESOURCE_FLAG_MAP_COHERENT) != 0;
    }

    bool IsValid() const
    {
        return m_valid;
    }
    bool IsBlob() const
    {
        return m_is_blob;
    }
    ULONGLONG GetBlobSize() const
    {
        return m_blob_size;
    }
    ULONG GetBlobFlags() const
    {
        return m_blob_flags;
    }
    bool IsBlobCreated() const
    {
        return m_blob_created;
    }

    void EnsureBlobCreated(ULONG ctx_id);

    void AttachBacking(MDL *pMdl, size_t pageCount, size_t pageOffset);
    void DetachBacking();

    void FlushToScreen(UINT scan_id);

    static NTSTATUS GetStandardAllocationDriverData(DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation);
    static NTSTATUS DxgkCreateAllocation(VioGpuAdapter *adapter, DXGKARG_CREATEALLOCATION *pCreateAllocation);

    NTSTATUS DescribeAllocation(DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation);
    NTSTATUS MapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);
    NTSTATUS UnmapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);

    NTSTATUS EscapeResourceInfo(VIOGPU_RES_INFO_REQ *resInfo);
    NTSTATUS EscapeResourceBusy(VIOGPU_RES_BUSY_REQ *resBusy);
    NTSTATUS EscapeResourceMapBlob(VIOGPU_RES_MAP_BLOB_REQ *resMap, VioGpuDevice *device);
    NTSTATUS EscapeResourceUnmapBlob(VIOGPU_RES_UNMAP_BLOB_REQ *resUnmap, VioGpuDevice *device);

  private:
    struct BlobUserMapping
    {
        LIST_ENTRY ListEntry;
        PEPROCESS Process;
        HANDLE ProcessId;
        PVOID UserVa;
        ULONGLONG Size;
        ULONG MapInfo;
        ULONG RefCount;
    };

    BlobUserMapping *FindBlobMappingLocked(HANDLE process_id, PEPROCESS process);

    VioGpuAdapter *m_adapter;

    VIOGPU_RESOURCE_OPTIONS m_options;
    UINT m_Id;
    bool m_is_blob;
    bool m_valid;
    ULONGLONG m_blob_size;
    ULONGLONG m_blob_id;
    ULONG m_blob_mem;
    ULONG m_blob_flags;
    ULONGLONG m_blob_offset;
    bool m_blob_shmem_allocated;
    ULONG m_blob_map_info;
    bool m_blob_mapped;
    bool m_blob_created;
    NTSTATUS m_blob_create_status;
    ULONG m_blob_create_resp_type;
    FAST_MUTEX m_blob_map_mutex;
    LIST_ENTRY m_blob_map_list;
    ULONG m_blob_map_user_refs;
    volatile LONG m_resource_created;

    void CreateBlobResource(UINT ctx_id);    

    MDL *m_pMDL;
    size_t m_pageCount;
    size_t m_pageOffset;

    size_t m_DxPhysicalAddress;

    KEVENT m_busyNotification;
    volatile LONG m_busy;
};
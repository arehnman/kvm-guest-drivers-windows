/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#pragma once

#include "helper.h"


class VioGpuAdapter;
class VioGpuDevice;
class VioGpuAllocation;
class VioGpuCommander;

class VioGpuCommand
{
  public:
    VioGpuCommand(VioGpuAdapter *adapter);

    void Run();

    void PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand);

    static void RunningCbDone(void *cmd);

    void VioGpuCommand::VioGpuCommandDone();

    void SetDmaBuf(char *pDmaBuffer)
    {
        m_pDmaBuffer = pDmaBuffer;
    }

    void SetPrivateDataSlot(VioGpuCommand **slot)
    {
        m_pPrivateDataSlot = slot;
    }

    void AttachAllocations(DXGK_ALLOCATIONLIST *allocationList, UINT allocationListLength);

    UINT GetSubmissionFenceId() const
    {
        return m_FenceId;
    }

    UINT GetNodeOrdinal() const
    {
        return m_NodeOrdinal;
    }

    UINT GetEngineOrdinal() const
    {
        return m_EngineOrdinal;
    }

    BOOLEAN OnPacketCompletedFromIsr(UINT *fenceId, UINT *nodeOrdinal, UINT *engineOrdinal);

  private:
    VioGpuAdapter *m_pAdapter;
    VioGpuCommander *m_pCommander;
    VioGpuDevice *m_pContext;

    UINT m_FenceId;
    UINT m_NodeOrdinal;
    UINT m_EngineOrdinal;

    char *m_pDmaBuffer;
    char *m_pCommand;
    char *m_pEnd;
    LONG m_done = 0;
    LONG m_isrPendingPackets = 0;
    VioGpuCommand **m_pPrivateDataSlot = NULL;

    VioGpuAllocation **m_allocations;
    UINT m_allocationsLength;
};

class VioGpuCommander
{
  public:
    VioGpuCommander(VioGpuAdapter *pAdapter);

    NTSTATUS Patch(const DXGKARG_PATCH *pPatch);
    NTSTATUS SubmitCommand(const DXGKARG_SUBMITCOMMAND *pSubmitCommand);

  private:

    VioGpuAdapter *m_pAdapter;
};

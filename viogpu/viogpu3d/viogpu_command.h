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

    void AttachAllocations(DXGK_ALLOCATIONLIST *allocationList, UINT allocationListLength);


  private:
    VioGpuAdapter *m_pAdapter;
    VioGpuCommander *m_pCommander;
    VioGpuDevice *m_pContext;

    UINT m_FenceId;

    char *m_pDmaBuffer;
    char *m_pCommand;
    char *m_pEnd;
    LONG m_done = 0;

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

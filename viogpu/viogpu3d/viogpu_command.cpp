/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#include "viogpu_command.h"
#include "viogpu_device.h"
#include "viogpu_adapter.h"
#include "baseobj.h"

#pragma code_seg(push)
#pragma code_seg()

VioGpuCommand::VioGpuCommand(VioGpuAdapter *adapter)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_pAdapter = adapter;
    m_pCommander = &adapter->commander;
    m_pContext = NULL;

    m_FenceId = 0;
    m_NodeOrdinal = 0;
    m_EngineOrdinal = 0;
    m_pDmaBuffer = NULL;
    m_pCommand = NULL;
    m_pEnd = NULL;

    m_allocations = NULL;
    m_allocationsLength = 0;
};

static BOOLEAN IsExpectedEmptySubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    if (pSubmitCommand->DmaBufferSubmissionEndOffset <
        pSubmitCommand->DmaBufferSubmissionStartOffset)
    {
        return FALSE;
    }

    const BOOLEAN emptyDmaRange =
        pSubmitCommand->DmaBufferSubmissionEndOffset ==
        pSubmitCommand->DmaBufferSubmissionStartOffset;

    if (!emptyDmaRange)
    {
        return FALSE;
    }

    if (pSubmitCommand->Flags.Flip || pSubmitCommand->Flags.FlipWithNoWait)
    {
        return TRUE;
    }

    if (pSubmitCommand->Flags.Paging ||
        pSubmitCommand->Flags.NullRendering)
    {
        return TRUE;
    }

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WIN8)
    if (pSubmitCommand->Flags.ContextSwitch)
    {
        return TRUE;
    }
#endif

    return FALSE;
}

void VioGpuCommand::PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    InterlockedExchange(&m_done, 0);
    InterlockedExchange(&m_isrPendingPackets, 0);
    InterlockedExchange(&m_dmaNotified, 0);

    m_FenceId = pSubmitCommand->SubmissionFenceId;
    m_EngineOrdinal = pSubmitCommand->EngineOrdinal;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WIN8)
    m_NodeOrdinal = pSubmitCommand->NodeOrdinal;
#else
    m_NodeOrdinal = 0;
#endif
    m_submitFlagsValue = pSubmitCommand->Flags.Value;
    m_submitPaging = pSubmitCommand->Flags.Paging ? TRUE : FALSE;
    m_expectedEmptySubmit = IsExpectedEmptySubmit(pSubmitCommand);
    if (m_pDmaBuffer)
    {
        m_pCommand = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionStartOffset;
        m_pEnd = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionEndOffset;
    }
    else
    {
        m_pCommand = NULL;
        m_pEnd = NULL;
    }
    m_pContext = reinterpret_cast<VioGpuDevice *>(pSubmitCommand->hContext);

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s cmd=%p fence=%u node=%u engine=%u hContext=%p ctx_id=%u dma=%p start=0x%x end=0x%x priv=%p\n",
              __FUNCTION__,
              this,
              m_FenceId,
              m_NodeOrdinal,
              m_EngineOrdinal,
              m_pContext,
              m_pContext ? m_pContext->GetId() : 0,
              m_pDmaBuffer,
              pSubmitCommand->DmaBufferSubmissionStartOffset,
              pSubmitCommand->DmaBufferSubmissionEndOffset,
              pSubmitCommand->pDmaBufferPrivateData));
}

static UINT CountDmaCompletionPackets(char *command, char *end, UINT fenceId, BOOLEAN *valid)
{
    UINT packets = 0;
    char *cursor = command;

    *valid = TRUE;

    while (cursor < end)
    {
        if (cursor + sizeof(VIOGPU_COMMAND_HDR) > end)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s fence_id=%u truncated command header: cmd=%p end=%p\n",
                      __FUNCTION__,
                      fenceId,
                      cursor,
                      end));
            *valid = FALSE;
            return 0;
        }

        VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)cursor;
        cursor += sizeof(VIOGPU_COMMAND_HDR);

        if (cursor + cmdHdr->size > end)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s fence_id=%u invalid command size=%u cmd=%p end=%p\n",
                      __FUNCTION__,
                      fenceId,
                      cmdHdr->size,
                      cursor,
                      end));
            *valid = FALSE;
            return 0;
        }

        cursor += cmdHdr->size;

        switch (cmdHdr->type)
        {
            case VIOGPU_CMD_NOP:
            case VIOGPU_CMD_SUBMIT:
            case VIOGPU_CMD_TRANSFER_TO_HOST:
            case VIOGPU_CMD_TRANSFER_FROM_HOST:
                packets++;
                break;

            case VIOGPU_CMD_PRESENT_FLIP:
                if (cmdHdr->size < sizeof(VIOGPU_PRESENT_FLIP_CMD))
                {
                    DbgPrint(TRACE_LEVEL_WARNING,
                             ("%s fence_id=%u invalid present flip size=%u\n",
                              __FUNCTION__,
                              fenceId,
                              cmdHdr->size));
                    *valid = FALSE;
                    return 0;
                }
                packets++;
                break;

            default:
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("%s fence_id=%u unsupported command type=%u size=%u\n",
                          __FUNCTION__, fenceId, cmdHdr->type, cmdHdr->size));
                *valid = FALSE;
                return 0;
        }
    }

    return packets;
}

void VioGpuCommand::Run()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    InterlockedIncrement(&m_done);

    if (!m_pCommand || !m_pEnd || m_pCommand >= m_pEnd)
    {
        if (m_expectedEmptySubmit)
        {
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("%s cmd=%p empty dma submit (fence-only) fence=%u node=%u engine=%u submit_flags=0x%x paging=%u hContext=%p ctx_id=%u owner_pid=%p cmd_ptr=%p end_ptr=%p\n",
                      __FUNCTION__,
                      this,
                      m_FenceId,
                      m_NodeOrdinal,
                      m_EngineOrdinal,
                      m_submitFlagsValue,
                      m_submitPaging ? 1 : 0,
                      m_pContext,
                      m_pContext ? m_pContext->GetId() : 0,
                      m_pContext ? m_pContext->GetOwnerProcessId() : 0,
                      m_pCommand,
                      m_pEnd));
        }
        else
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s cmd=%p WARNING unexpected empty dma buffer fence=%u node=%u engine=%u submit_flags=0x%x paging=%u hContext=%p ctx_id=%u owner_pid=%p cmd_ptr=%p end_ptr=%p\n",
                      __FUNCTION__,
                      this,
                      m_FenceId,
                      m_NodeOrdinal,
                      m_EngineOrdinal,
                      m_submitFlagsValue,
                      m_submitPaging ? 1 : 0,
                      m_pContext,
                      m_pContext ? m_pContext->GetId() : 0,
                      m_pContext ? m_pContext->GetOwnerProcessId() : 0,
                      m_pCommand,
                      m_pEnd));
        }

        /* Keep fence completion ordering consistent with non-empty submits.
         * Immediate software completion can overtake older pending fences and
         * trigger VIDEO_SCHEDULER_INTERNAL_ERROR(0x119, Arg1=1).
         */
        InterlockedExchange(&m_isrPendingPackets, 1);
        InterlockedIncrement(&m_done);
        UINT ret = m_pAdapter->ctrlQueue.SubmitNop(VioGpuCommand::RunningCbDone, this, TRUE /* fenced */);
        if (ret)
        {
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("%s cmd=%p failed to queue empty submit fence=%u node=%u engine=%u ret=%u\n",
                      __FUNCTION__,
                      this,
                      m_FenceId,
                      m_NodeOrdinal,
                      m_EngineOrdinal,
                      ret));
            InterlockedExchange(&m_isrPendingPackets, 0);
            VioGpuCommand::VioGpuCommandDone();
        }

        VioGpuCommand::VioGpuCommandDone();
        return;
    }

    BOOLEAN validCommandBuffer = TRUE;
    UINT pendingPackets = CountDmaCompletionPackets(m_pCommand, m_pEnd, m_FenceId, &validCommandBuffer);
    if (!validCommandBuffer)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s cmd=%p fence=%u invalid DMA command stream, completing with fenced NOP\n",
                  __FUNCTION__,
                  this,
                  m_FenceId));

        InterlockedExchange(&m_isrPendingPackets, 1);
        InterlockedIncrement(&m_done);
        UINT ret = m_pAdapter->ctrlQueue.SubmitNop(VioGpuCommand::RunningCbDone, this, TRUE /* fenced */);
        if (ret)
        {
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("%s cmd=%p failed to queue invalid-stream completion fence=%u node=%u engine=%u ret=%u\n",
                      __FUNCTION__,
                      this,
                      m_FenceId,
                      m_NodeOrdinal,
                      m_EngineOrdinal,
                      ret));
            InterlockedExchange(&m_isrPendingPackets, 0);
            VioGpuCommand::VioGpuCommandDone();
        }

        VioGpuCommand::VioGpuCommandDone();
        return;
    }

    InterlockedExchange(&m_isrPendingPackets, (LONG)pendingPackets);

    while (m_pCommand < m_pEnd)
    {
        if (m_pCommand + sizeof(VIOGPU_COMMAND_HDR) > m_pEnd)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s fence_id=%u truncated command header: cmd=%p end=%p\n",
                      __FUNCTION__, m_FenceId, m_pCommand, m_pEnd));
            break;
        }
        VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)m_pCommand;
        m_pCommand += sizeof(VIOGPU_COMMAND_HDR);

        void *cmdBody = m_pCommand;
        if (m_pCommand + cmdHdr->size > m_pEnd)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s fence_id=%u invalid command size=%u cmd=%p end=%p\n",
                      __FUNCTION__, m_FenceId, cmdHdr->size, m_pCommand, m_pEnd));
            break;
        }
        m_pCommand += cmdHdr->size;
        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s fence_id=%d running command=%d", __FUNCTION__, m_FenceId, cmdHdr->type));

        switch (cmdHdr->type)
        {
            case VIOGPU_CMD_NOP:
                {
                    InterlockedIncrement(&m_done);

                    UINT ret = m_pAdapter->ctrlQueue.SubmitNop(VioGpuCommand::RunningCbDone,
                                                                this,
                                                                TRUE /* fenced */);
                    if (ret)
                    {
                        DbgPrint(TRACE_LEVEL_WARNING,
                                 ("%s fence_id=%u failed nop submit ret=%u\n",
                                  __FUNCTION__,
                                  m_FenceId,
                                  ret));
                        InterlockedDecrement(&m_isrPendingPackets);
                        VioGpuCommand::VioGpuCommandDone();
                    }
                    break;
                }

            case VIOGPU_CMD_SUBMIT:
                {
                    InterlockedIncrement(&m_done);

                    PBYTE submitCmd = NULL;
                    if (cmdHdr->size > 0)
                    {
                        submitCmd = new (NonPagedPoolNx) BYTE[cmdHdr->size];
                        if (!submitCmd)
                        {
                            DbgPrint(TRACE_LEVEL_FATAL,
                                     ("%s fence_id=%u OOM allocating submit buffer (size=%u) -> bugcheck\n",
                                      __FUNCTION__,
                                      m_FenceId,
                                      cmdHdr->size));
                            KeBugCheckEx(0x000000E2,
                                         static_cast<ULONG_PTR>('OIVg'),
                                         static_cast<ULONG_PTR>(m_FenceId),
                                         static_cast<ULONG_PTR>(cmdHdr->size),
                                         reinterpret_cast<ULONG_PTR>(this));
                        }
                        RtlCopyMemory(submitCmd, cmdBody, cmdHdr->size);
                    }

                    m_pAdapter->ctrlQueue.SubmitCommand(submitCmd,
                                                        cmdHdr->size,
                                                        m_pContext->GetId(),
                                                        VioGpuCommand::RunningCbDone,
                                                        this);
                    break;
                }

            case VIOGPU_CMD_TRANSFER_TO_HOST:
            case VIOGPU_CMD_TRANSFER_FROM_HOST:
                {
                    InterlockedIncrement(&m_done);

                    VIOGPU_TRANSFER_CMD *transferCmd = (VIOGPU_TRANSFER_CMD *)cmdBody;

                    m_pAdapter->ctrlQueue.TransferHostCmd(cmdHdr->type == VIOGPU_CMD_TRANSFER_TO_HOST,
                                                          m_pContext->GetId(),
                                                          transferCmd,
                                                          VioGpuCommand::RunningCbDone,
                                                          this);
                    break;
                }

            case VIOGPU_CMD_PRESENT_FLIP:
                {
                    if (cmdHdr->size < sizeof(VIOGPU_PRESENT_FLIP_CMD))
                    {
                        DbgPrint(TRACE_LEVEL_WARNING,
                                 ("%s fence_id=%u invalid present flip size=%u\n",
                                  __FUNCTION__,
                                  m_FenceId,
                                  cmdHdr->size));
                        break;
                    }

                    VIOGPU_PRESENT_FLIP_CMD *flipCmd = (VIOGPU_PRESENT_FLIP_CMD *)cmdBody;

                    InterlockedIncrement(&m_done);

                    m_pAdapter->ctrlQueue.SetScanout(flipCmd->scan_id,
                                                     flipCmd->res_id,
                                                     flipCmd->width,
                                                     flipCmd->height,
                                                     flipCmd->x,
                                                     flipCmd->y);
                    UINT ret = m_pAdapter->ctrlQueue.ResFlush(flipCmd->res_id,
                                                              flipCmd->width,
                                                              flipCmd->height,
                                                              flipCmd->x,
                                                              flipCmd->y,
                                                              VioGpuCommand::RunningCbDone,
                                                              this);
                    if (ret)
                    {
                        DbgPrint(TRACE_LEVEL_WARNING,
                                 ("%s fence_id=%u failed present flip scan=%u res=%u rect=%ux%u+%u+%u ret=%u\n",
                                  __FUNCTION__,
                                  m_FenceId,
                                  flipCmd->scan_id,
                                  flipCmd->res_id,
                                  flipCmd->width,
                                  flipCmd->height,
                                  flipCmd->x,
                                  flipCmd->y,
                                  ret));
                        InterlockedDecrement(&m_isrPendingPackets);
                        VioGpuCommand::VioGpuCommandDone();
                    }
                    break;
                }

            default:
                {
                    DbgPrint(TRACE_LEVEL_WARNING,
                             ("%s fence_id=%u unsupported command type=%u size=%u\n",
                              __FUNCTION__, m_FenceId, cmdHdr->type, cmdHdr->size));
                    ASSERT(0);
                    break;
                }
        }
    }

    VioGpuCommand::VioGpuCommandDone();
}

ULONG VioGpuCommand::GetContextId() const
{
    return m_pContext ? m_pContext->GetId() : 0;
}

HANDLE VioGpuCommand::GetOwnerProcessId() const
{
    return m_pContext ? m_pContext->GetOwnerProcessId() : NULL;
}

#pragma code_seg(pop)
PAGED_CODE_SEG_BEGIN

NTSTATUS VioGpuCommand::AttachAllocations(DXGK_ALLOCATIONLIST *allocationList, UINT allocationListLength)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    if (allocationListLength == 0)
    {
        m_allocations = NULL;
        m_allocationsLength = 0;
        return STATUS_SUCCESS;
    }

    if (!allocationList)
    {
        return STATUS_INVALID_PARAMETER;
    }

    m_allocations = new (NonPagedPoolNx) VioGpuAllocation *[allocationListLength];
    if (!m_allocations)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s failed to allocate allocation array (count=%u)\n",
                  __FUNCTION__,
                  allocationListLength));
        m_allocationsLength = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_allocationsLength = allocationListLength;
    for (UINT i = 0; i < allocationListLength; i++)
    {
        VioGpuDeviceAllocation *deviceAllocation = reinterpret_cast<VioGpuDeviceAllocation *>(allocationList[i].hDeviceSpecificAllocation);
        if (deviceAllocation)
        {
            m_allocations[i] = deviceAllocation->GetAllocation();
            m_allocations[i]->MarkBusy();
        }
        else
        {
            m_allocations[i] = NULL;
        }
    }

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()

void VioGpuCommand::RunningCbDone(void *cmd)
{
    ((VioGpuCommand *)cmd)->VioGpuCommandDone();
}

BOOLEAN VioGpuCommand::OnPacketCompletedFromIsr(UINT *fenceId, UINT *nodeOrdinal, UINT *engineOrdinal)
{
    LONG pending = InterlockedDecrement(&m_isrPendingPackets);
    if (pending > 0)
    {
        return FALSE;
    }

    if (pending < 0)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s cmd=%p WARNING m_isrPendingPackets underflow fence_id=%u\n",
                  __FUNCTION__,
                  this,
                  m_FenceId));
        InterlockedExchange(&m_isrPendingPackets, 0);
        return FALSE;
    }

    if (InterlockedCompareExchange(&m_dmaNotified, 1, 0) != 0)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s cmd=%p duplicate DMA completion suppressed fence_id=%u node=%u engine=%u ctx_id=%u owner_pid=%p\n",
                  __FUNCTION__,
                  this,
                  m_FenceId,
                  m_NodeOrdinal,
                  m_EngineOrdinal,
                  GetContextId(),
                  GetOwnerProcessId()));
        return FALSE;
    }

    if (fenceId)
    {
        *fenceId = m_FenceId;
    }
    if (nodeOrdinal)
    {
        *nodeOrdinal = m_NodeOrdinal;
    }
    if (engineOrdinal)
    {
        *engineOrdinal = m_EngineOrdinal;
    }

    return TRUE;
}

void VioGpuCommand::VioGpuCommandDone()
{
    if (InterlockedDecrement(&m_done))
    {
        return;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("%s cmd=%p finished fence_id=%d\n", __FUNCTION__, this, m_FenceId));

    if (m_allocations)
    {
        for (UINT i = 0; i < m_allocationsLength; i++)
        {
            if (m_allocations[i])
            {
                m_allocations[i]->UnmarkBusy();
            }
        }
        delete[] m_allocations;
    }

    // DMA completion notify happens from ISR during ctrl queue staging.

    if (m_pPrivateDataSlot)
    {
        InterlockedCompareExchangePointer((PVOID volatile *)m_pPrivateDataSlot, NULL, this);
        m_pPrivateDataSlot = NULL;
    }

    delete this;
}

#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

VioGpuCommander::VioGpuCommander(VioGpuAdapter *pAdapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_pAdapter = pAdapter;
}

NTSTATUS VioGpuCommander::Patch(const DXGKARG_PATCH *pPatch)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s \n", __FUNCTION__));

    UNREFERENCED_PARAMETER(pPatch);

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()
NTSTATUS VioGpuCommander::SubmitCommand(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    VIOGPU_ASSERT(pSubmitCommand != NULL);

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WIN8)
    UINT nodeOrdinal = pSubmitCommand->NodeOrdinal;
#else
    UINT nodeOrdinal = 0;
#endif

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("---> %s fence_id=%u node=%u engine=%u hContext=%p start=0x%x end=0x%x priv=%p\n",
              __FUNCTION__,
              pSubmitCommand->SubmissionFenceId,
              nodeOrdinal,
              pSubmitCommand->EngineOrdinal,
              pSubmitCommand->hContext,
              pSubmitCommand->DmaBufferSubmissionStartOffset,
              pSubmitCommand->DmaBufferSubmissionEndOffset,
              pSubmitCommand->pDmaBufferPrivateData));

    VioGpuCommand *cmd = NULL;
    VioGpuCommand **privSlot = NULL;
    if (pSubmitCommand->pDmaBufferPrivateData)
    {
        privSlot = (VioGpuCommand **)pSubmitCommand->pDmaBufferPrivateData;
        cmd = reinterpret_cast<VioGpuCommand *>(
            InterlockedExchangePointer((PVOID volatile *)privSlot, NULL));
        if (cmd)
        {
            // This command object is consumed by this SubmitCommand invocation.
            // Prevent slot-based reuse across concurrent/duplicate submissions.
            cmd->SetPrivateDataSlot(NULL);
        }
        else
        {
            if (IsExpectedEmptySubmit(pSubmitCommand))
            {
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("%s private slot empty for fence-only submit fence_id=%u node=%u engine=%u flags=0x%x hContext=%p priv_slot=%p\n",
                          __FUNCTION__,
                          pSubmitCommand->SubmissionFenceId,
                          nodeOrdinal,
                          pSubmitCommand->EngineOrdinal,
                          pSubmitCommand->Flags.Value,
                          pSubmitCommand->hContext,
                          privSlot));
            }
            else
            {
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("%s private slot already empty fence_id=%u node=%u engine=%u flags=0x%x hContext=%p start=0x%x end=0x%x priv_slot=%p\n",
                          __FUNCTION__,
                          pSubmitCommand->SubmissionFenceId,
                          nodeOrdinal,
                          pSubmitCommand->EngineOrdinal,
                          pSubmitCommand->Flags.Value,
                          pSubmitCommand->hContext,
                          pSubmitCommand->DmaBufferSubmissionStartOffset,
                          pSubmitCommand->DmaBufferSubmissionEndOffset,
                          privSlot));
            }
        }
    }

    if (!cmd)
    {
        cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
        if (!cmd)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s failed to allocate command object fence_id=%u hContext=%p\n",
                      __FUNCTION__,
                      pSubmitCommand->SubmissionFenceId,
                      pSubmitCommand->hContext));
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("%s created command object cmd=%p fence_id=%u hContext=%p submit_flags=0x%x start=0x%x end=0x%x priv=%p\n",
                  __FUNCTION__,
                  cmd,
                  pSubmitCommand->SubmissionFenceId,
                  pSubmitCommand->hContext,
                  pSubmitCommand->Flags.Value,
                  pSubmitCommand->DmaBufferSubmissionStartOffset,
                  pSubmitCommand->DmaBufferSubmissionEndOffset,
                  pSubmitCommand->pDmaBufferPrivateData));
    }
    else
    {
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("%s consumed private command cmd=%p fence_id=%u hContext=%p priv_slot=%p\n",
                  __FUNCTION__,
                  cmd,
                  pSubmitCommand->SubmissionFenceId,
                  pSubmitCommand->hContext,
                  privSlot));
    }

    cmd->PrepareSubmit(pSubmitCommand);
    m_pAdapter->RecordDmaSubmittedForPreemption(cmd->GetSubmissionFenceId(),
                                                cmd->GetNodeOrdinal(),
                                                cmd->GetEngineOrdinal(),
                                                cmd->GetContextId(),
                                                cmd->GetOwnerProcessId());

    cmd->Run();

    return STATUS_SUCCESS;
}

#pragma code_seg(pop)

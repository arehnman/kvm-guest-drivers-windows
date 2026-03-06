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

void VioGpuCommand::PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_FenceId = pSubmitCommand->SubmissionFenceId;
    m_EngineOrdinal = pSubmitCommand->EngineOrdinal;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WIN8)
    m_NodeOrdinal = pSubmitCommand->NodeOrdinal;
#else
    m_NodeOrdinal = 0;
#endif
    if (m_pDmaBuffer)
    {
        m_pCommand = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionStartOffset;
        m_pEnd = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionEndOffset;
    }
    m_pContext = reinterpret_cast<VioGpuDevice *>(pSubmitCommand->hContext);
}

void VioGpuCommand::Run()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    InterlockedIncrement(&m_done);

    if (!m_pCommand || !m_pEnd || m_pCommand >= m_pEnd)
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("%s cmd=%p WARNING empty dma buffer\n", __FUNCTION__, this));
        if (m_pContext)
        {
            InterlockedIncrement(&m_isrPendingPackets);
            InterlockedIncrement(&m_done);
            m_pAdapter->ctrlQueue.SubmitCommand(0,
                                                0,
                                                m_pContext->GetId(),
                                                VioGpuCommand::RunningCbDone,
                                                this);
        }
        VioGpuCommand::VioGpuCommandDone();
        return;
    }

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
            case VIOGPU_CMD_SUBMIT:
                {
                    InterlockedIncrement(&m_isrPendingPackets);
                    InterlockedIncrement(&m_done);

                    PBYTE submitCmd = new (NonPagedPoolNx) BYTE[cmdHdr->size];
                    RtlCopyMemory(submitCmd, cmdBody, cmdHdr->size);

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
                    InterlockedIncrement(&m_isrPendingPackets);
                    InterlockedIncrement(&m_done);

                    VIOGPU_TRANSFER_CMD *transferCmd = (VIOGPU_TRANSFER_CMD *)cmdBody;

                    m_pAdapter->ctrlQueue.TransferHostCmd(cmdHdr->type == VIOGPU_CMD_TRANSFER_TO_HOST,
                                                          m_pContext->GetId(),
                                                          transferCmd,
                                                          VioGpuCommand::RunningCbDone,
                                                          this);
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

#pragma code_seg(pop)
PAGED_CODE_SEG_BEGIN

void VioGpuCommand::AttachAllocations(DXGK_ALLOCATIONLIST *allocationList, UINT allocationListLength)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_allocations = new (NonPagedPoolNx) VioGpuAllocation *[allocationListLength];
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

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s fence_id=%d\n", __FUNCTION__, pSubmitCommand->SubmissionFenceId));

    VioGpuCommand *cmd = NULL;
    if (pSubmitCommand->pDmaBufferPrivateData)
    {
        VioGpuCommand **priv = (VioGpuCommand **)pSubmitCommand->pDmaBufferPrivateData;
        if (*priv != NULL)
        {
            cmd = *priv;
        }
    }

    if (!cmd)
    {
        cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
        DbgPrint(TRACE_LEVEL_WARNING, ("%s EMPTY cmd=%p\n", __FUNCTION__, cmd));
    }

    cmd->PrepareSubmit(pSubmitCommand);

    cmd->Run();

    return STATUS_SUCCESS;
}

#pragma code_seg(pop)

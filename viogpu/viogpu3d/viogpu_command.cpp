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

    if (m_pCommand == 0)
{
        DbgPrint(TRACE_LEVEL_WARNING, ("%s cmd=%p WARNING m_pCommand == 0\n", __FUNCTION__, this));
        InterlockedIncrement(&m_done);

        m_pAdapter->ctrlQueue.SubmitCommand(0,
                                            0,
                                            m_pContext->GetId(),
                                            VioGpuCommand::RunningCbDone,
                                            this);        
    }

    while (m_pCommand < m_pEnd)
    {

        VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)m_pCommand;
        m_pCommand += sizeof(VIOGPU_COMMAND_HDR);

        void *cmdBody = m_pCommand;
        m_pCommand += cmdHdr->size;

        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s fence_id=%d running command=%d", __FUNCTION__, m_FenceId, cmdHdr->type));

        switch (cmdHdr->type)
        {
            case VIOGPU_CMD_NOP:
            case VIOGPU_CMD_SUBMIT:
                {
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
        delete m_allocations;
    }

    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt;
    interrupt.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    interrupt.DmaCompleted.SubmissionFenceId = m_FenceId;
    interrupt.DmaCompleted.NodeOrdinal = 0;
    interrupt.DmaCompleted.EngineOrdinal = 0;
    m_pAdapter->NotifyInterrupt(&interrupt, true);

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

/*
 * Copyright (C) 2014-2019,  Netronome Systems, Inc.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @file          blocks/vnic/pci_in/gather.c
 * @brief         Code to gather PCI.IN descriptors from pending queues
 */

#include <nfp6000/nfp_cls.h>
#include <nfp6000/nfp_me.h>
#include <nfp6000/nfp_pcie.h>
#include <nfp6000/nfp_qc.h>

#include <assert.h>
#include <nfp.h>
#include <nfp_chipres.h>

#include <nfp/pcie.h>
#include <std/event.h>

#include <vnic/pci_in.h>
#include <vnic/shared/nfd.h>
#include <vnic/shared/nfd_internal.h>
#include <vnic/utils/dma_seqn.h>
#include <vnic/utils/cls_ring.h>
#include <vnic/utils/qc.h>
#include <vnic/utils/qcntl.h>

#include <vnic/shared/nfd_cfg.h>
#include <vnic/shared/nfd_rst_state.h>


#ifndef NFD_IN_ISSUE_DMA_QSHIFT
#define NFD_IN_ISSUE_DMA_QSHIFT 0
#endif

#ifndef NFD_IN_ISSUE_DMA_QXOR
#define NFD_IN_ISSUE_DMA_QXOR 0
#endif

#ifndef _link_sym
#define _link_sym(x) __link_sym(#x)
#endif

/*
 * Gather ME PCIe reset definitions
 */
#define NFD_IN_GATHER_CTX_RST_POLL  1
/* For a 10 us timeout:
 *  val = t_req * f_clk / 16 = (10e-6 * 800e6) >> 4
 *  where 16 is number of clk cycles per count in timestamp CSR */
#define NFD_IN_GATHER_POLL_TIME     0x1f4

/*
 * State variables for PCI.IN gather
 */
__shared __gpr struct qc_bitmask active_bmsk;
__shared __gpr struct qc_bitmask pending_bmsk;
__shared __lmem struct nfd_in_queue_info queue_data[NFD_IN_MAX_QUEUES];

#if (NFD_IN_GATHER_MAX_IN_FLIGHT > 32)
#error "Issue DMA index ring will not work with more than 32 DMAs in flight"
#endif
static __shared __gpr unsigned int idma_list = 0;


/* XXX assume hi bits 0 to DMA into CLS */
static __shared __gpr unsigned int desc_ring_base0;
static __shared __gpr unsigned int desc_ring_base1;

__shared __gpr unsigned int dma_seq_issued = 0;
__shared __gpr unsigned int gather_dma_seq_compl = 0;
__shared __gpr unsigned int dma_issued0 = 0;
__shared __gpr unsigned int dma_completed0 = 0;
__shared __gpr unsigned int dma_issued1 = 0;
__shared __gpr unsigned int dma_completed1 = 0;

/* Signals and transfer registers for managing
 * gather_dma_seq_compl updates*/
static volatile __xread unsigned int nfd_in_gather_event_xfer;
static volatile SIGNAL nfd_in_gather_event_sig;

static __xwrite unsigned int nfd_in_gather_compl_refl_out0 = 0;
__remote volatile __xread unsigned int nfd_in_gather_compl_refl_in0;
__remote volatile SIGNAL nfd_in_gather_compl_refl_sig0;
static __xwrite unsigned int nfd_in_gather_compl_refl_out1 = 0;
__remote volatile __xread unsigned int nfd_in_gather_compl_refl_in1;
__remote volatile SIGNAL nfd_in_gather_compl_refl_sig1;

#define DESC_RING_SZ (NFD_IN_MAX_BATCH_SZ * NFD_IN_DESC_BATCH_Q_SZ *       \
                      sizeof(struct nfd_in_tx_desc))
__export __shared __cls __align(DESC_RING_SZ) struct nfd_in_tx_desc
    desc_ring0[NFD_IN_MAX_BATCH_SZ * NFD_IN_DESC_BATCH_Q_SZ];
__export __shared __cls __align(DESC_RING_SZ) struct nfd_in_tx_desc
    desc_ring1[NFD_IN_MAX_BATCH_SZ * NFD_IN_DESC_BATCH_Q_SZ];

static __gpr struct nfp_pcie_dma_cmd descr_tmp;


_NFP_CHIPRES_ASM(.alloc_mem nfd_in_batch_ring0_mem                         \
                 cls+NFD_IN_BATCH_RING0_ADDR island                        \
                 (NFD_IN_BATCH_RING0_SIZE_LW*4) (NFD_IN_BATCH_RING0_SIZE_LW*4));
_NFP_CHIPRES_ASM(.alloc_resource nfd_in_batch_ring0_num \
                 cls_rings+NFD_IN_BATCH_RING0_NUM island 1 1);
_NFP_CHIPRES_ASM(.alloc_mem nfd_in_batch_ring1_mem                         \
                 cls+NFD_IN_BATCH_RING1_ADDR island                        \
                 (NFD_IN_BATCH_RING1_SIZE_LW*4) (NFD_IN_BATCH_RING1_SIZE_LW*4));
_NFP_CHIPRES_ASM(.alloc_resource nfd_in_batch_ring1_num                    \
                 cls_rings+NFD_IN_BATCH_RING1_NUM island 1 1);


/*
 * Reserve PCIe Resources
 */
PCIE_DMA_ALLOC(nfd_in_gather_dma, island, PCIE_ISL, frompci_hi,
               NFD_IN_GATHER_MAX_IN_FLIGHT);


/* XXX Move to some sort of CT reflect library */

__intrinsic void
reflect_data(unsigned int dst_me, unsigned int dst_xfer,
             unsigned int sig_no, volatile __xwrite void *src_xfer,
             size_t size)
{
    unsigned int addr;
    unsigned int count = (size >> 2);
    struct nfp_mecsr_cmd_indirect_ref_0 indirect;

    /* ctassert(__is_write_reg(src_xfer)); */ /* TEMP, avoid volatile warnings */
    ctassert(__is_ct_const(size));

    /* Generic address computation.
     * Could be expensive if dst_me, or dst_xfer
     * not compile time constants */
    addr = ((dst_me & 0xFF0)<<20 | ((dst_me & 0xF)<<10 | (dst_xfer & 0x3F)<<2));

    indirect.__raw = 0;
    indirect.signal_num = sig_no;
    indirect.signal_ctx = 0;    /* Be explicit that we're targeting ctx 0 */
    local_csr_write(local_csr_cmd_indirect_ref_0, indirect.__raw);

    /* Currently just support reflect_write_sig_remote */
    /* XXX NFP_MECSR_PREV_ALU_OV_SIG_CTX_bit is next to SIG_NUM */
    __asm {
        alu[--, --, b, 3, <<NFP_MECSR_PREV_ALU_OV_SIG_NUM_bit];
        ct[reflect_write_sig_remote, *src_xfer, addr, 0, \
           __ct_const_val(count)], indirect_ref;
    };
}


/**
 * Complete processing of PCIe reset in GATHER ME.
 *
 * This function must only be called once the app master has ACKed the
 * PCIe reset.
 */
__intrinsic void
gather_compl_rst()
{
    __gpr unsigned int amt;

    amt = dma_seq_issued - gather_dma_seq_compl;
    gather_dma_seq_compl = dma_seq_issued;
    distr_gather_seqn(amt);
}


/**
 * Perform shared initialisation of the gather block.
 */
void
gather_setup_shared()
{
     /* Zero bitmasks */
    init_bitmasks(&active_bmsk);
    init_bitmasks(&pending_bmsk);

    cls_ring_setup(NFD_IN_BATCH_RING0_NUM,
                   (__cls void *)_link_sym(nfd_in_batch_ring0_mem),
                   (NFD_IN_BATCH_RING0_SIZE_LW * 4));
    cls_ring_setup(NFD_IN_BATCH_RING1_NUM,
                   (__cls void *)_link_sym(nfd_in_batch_ring1_mem),
                   (NFD_IN_BATCH_RING1_SIZE_LW * 4));

    /*
     * Initialise the CLS TX descriptor ring
     */
    desc_ring_base0 = ((unsigned int) &desc_ring0) & 0xFFFFFFFF;
    desc_ring_base1 = ((unsigned int) &desc_ring1) & 0xFFFFFFFF;

}


/**
 * Perform once off, CTX0-only initialisation of sequence number autopushes
 */
void
distr_gather_setup_shared()
{
    dma_seqn_ap_setup(NFD_IN_GATHER_EVENT_FILTER, NFD_IN_GATHER_EVENT_FILTER,
                      NFD_IN_GATHER_EVENT_TYPE, 0, &nfd_in_gather_event_xfer,
                      &nfd_in_gather_event_sig);
}


/*
 * Convenience function which pushes gather DMA sequence numbers to
 * PCI.IN issue DMA MEs.
 */
__intrinsic void
distr_gather_seqn(unsigned int amt)
{
    __gpr unsigned int mask;
    __gpr unsigned int i0amt;
    __gpr unsigned int i1amt;

    mask = (1 << amt) - 1;
    i0amt = mask & ~idma_list;
    __asm {
        pop_count1[i0amt]
        pop_count2[i0amt]
        pop_count3[i0amt, i0amt]
    }
    i1amt = mask & idma_list;
    __asm {
        pop_count1[i1amt]
        pop_count2[i1amt]
        pop_count3[i1amt, i1amt]
    }
    idma_list >>= amt;

    if (i0amt) {
#ifdef NFD_IN_HAS_ISSUE0
        /* Mirror to Issue DMA 0 */
        __implicit_read(&nfd_in_gather_compl_refl_out0);

        dma_completed0 += i0amt;
        nfd_in_gather_compl_refl_out0 = dma_completed0;
        reflect_data(NFD_IN_DATA_DMA_ME0,
                     __xfer_reg_number(&nfd_in_gather_compl_refl_in0,
                                       NFD_IN_DATA_DMA_ME0),
                     __signal_number(&nfd_in_gather_compl_refl_sig0,
                                     NFD_IN_DATA_DMA_ME0),
                     &nfd_in_gather_compl_refl_out0,
                     sizeof nfd_in_gather_compl_refl_out0);
#else
        local_csr_write(local_csr_mailbox_0, NFD_IN_GATHER_INVALID_IDMA);
        local_csr_write(local_csr_mailbox_1, 0);
        halt();
#endif

    }

    if (i1amt) {
#ifdef NFD_IN_HAS_ISSUE1
        /* Mirror to Issue DMA 1 */
        __implicit_read(&nfd_in_gather_compl_refl_out1);

        dma_completed1 += i1amt;
        nfd_in_gather_compl_refl_out1 = dma_completed1;
        reflect_data(NFD_IN_DATA_DMA_ME1,
                     __xfer_reg_number(&nfd_in_gather_compl_refl_in1,
                                       NFD_IN_DATA_DMA_ME1),
                     __signal_number(&nfd_in_gather_compl_refl_sig1,
                                     NFD_IN_DATA_DMA_ME1),
                     &nfd_in_gather_compl_refl_out1,
                     sizeof nfd_in_gather_compl_refl_out1);
#else
        local_csr_write(local_csr_mailbox_0, NFD_IN_GATHER_INVALID_IDMA);
        local_csr_write(local_csr_mailbox_1, 1);
        halt();
#endif

    }
}


/**
 * Check autopush, compute gather_dma_seq_compl, and reflect to issue_dma ME
 *
 * "gather_dma_seq_compl" tracks the completed gather DMAs.  Events containing
 * the low 12 bits of a sequence number are received when DMAs complete,
 * and these are used to advance the full 32 bit sequence number.  The
 * PCI.IN issue_dma code also needs this sequence number to determine
 * when to process gather batches, so it is reflected to that ME.
 */
__intrinsic void
distr_gather()
{
    __gpr unsigned int amt;

    if (signal_test(&nfd_in_gather_event_sig)) {

        /* Simply acknowledge events when island in PCIe reset state. */
        if (NFD_RST_STATE_TEST_UP(PCIE_ISL)) {
            dma_seqn_advance_save(&nfd_in_gather_event_xfer,
                                  &gather_dma_seq_compl,
                                  &amt);

            distr_gather_seqn(amt);
        }

        __implicit_write(&nfd_in_gather_event_sig);
        event_cls_autopush_filter_reset(
            NFD_IN_GATHER_EVENT_FILTER,
            NFP_CLS_AUTOPUSH_STATUS_MONITOR_ONE_SHOT_ACK,
            NFD_IN_GATHER_EVENT_FILTER);

    } else {
        /* Swap to give other threads a chance to run */
        ctx_swap();
    }
}


/**
 * Perform per context initialisation (for CTX 1 to 7)
 */
void
gather_setup()
{
    /*
     * Initialise a DMA descriptor template
     * RequesterID (rid), CPP address, and PCIe address will be
     * overwritten per transaction.
     * For dma_mode, we technically only want to overwrite the "source"
     * field, i.e. 12 of the 16 bits.
     */
    descr_tmp.rid_override = 1;
    descr_tmp.trans_class = 0;
    descr_tmp.cpp_token = 0;    /* CLS doesn't offer write swap token */
    descr_tmp.dma_cfg_index = NFD_IN_GATHER_CFG_REG;
    descr_tmp.cpp_addr_hi = 0;
}


/**
 * Change the configuration of the queues and rings associated with a vNIC
 * @param cfg_msg       configuration information concerning the change
 *
 * This method performs changes to the local state for a vNIC.  The 'cfg_msg'
 * struct is used in conjunction with 'nfd_cfg_proc_msg' and internal nfd_cfg
 * state to determine a particular queue to change each time this method is
 * called.  See nfd_cfg.h for further information.
 */
__intrinsic void
gather_vnic_setup(struct nfd_cfg_msg *cfg_msg)
{
    struct qc_queue_config txq;
    unsigned int queue;
    unsigned char ring_sz;
    unsigned int ring_base[2];
    __gpr unsigned int bmsk_queue;

    nfd_cfg_proc_msg(cfg_msg, &queue, &ring_sz, ring_base, NFD_CFG_PCI_IN0);

    if (cfg_msg->error || !cfg_msg->interested) {
        return;
    }

    queue = NFD_VID2NATQ(cfg_msg->vid, queue);
    bmsk_queue = NFD_NATQ2BMQ(queue);

    txq.watermark    = NFP_QC_STS_HI_WATERMARK_4;
    txq.event_data   = NFD_EVENT_DATA;
    txq.ptr          = 0;

    if (cfg_msg->up_bit && !queue_data[bmsk_queue].up) {
        /* Up the queue:
         * - Set ring size and requester ID info
         * - (Re)clear queue pointers in case something changed them
         *   while down */
        queue_data[bmsk_queue].tx_w = 0;
        queue_data[bmsk_queue].tx_s = 0;
        queue_data[bmsk_queue].ring_sz_msk = ((1 << ring_sz) - 1);
        queue_data[bmsk_queue].requester_id = NFD_CFG_PF_OFFSET;
        if (NFD_VID_IS_VF(cfg_msg->vid)) {
            queue_data[bmsk_queue].requester_id = (cfg_msg->vid +
                                                   NFD_CFG_VF_OFFSET);
        }
        queue_data[bmsk_queue].spare0 = 0;
        queue_data[bmsk_queue].up = 1;
        queue_data[bmsk_queue].ring_base_hi = ring_base[1] & 0xFF;
        queue_data[bmsk_queue].ring_base_lo = ring_base[0];

        txq.event_type   = NFP_QC_STS_LO_EVENT_TYPE_NOT_EMPTY;
        txq.size         = ring_sz - 8; /* XXX add define for size shift */
        qc_init_queue(PCIE_ISL, NFD_NATQ2QC(queue, NFD_IN_TX_QUEUE), &txq);
    } else if (!cfg_msg->up_bit && queue_data[bmsk_queue].up) {
        /* Down the queue:
         * - Prevent it issuing events
         * - Clear active_msk bit
         * - Clear pending_msk bit
         * - Clear the proc bitmask bit?
         * - Clear tx_w and tx_s
         * - Try to count pending packets? Host responsibility? */

        /* Clear active and pending bitmask bits */
        clear_queue(&bmsk_queue, &active_bmsk);
        clear_queue(&bmsk_queue, &pending_bmsk);

        /* Clear queue LM state */
        queue_data[bmsk_queue].tx_w = 0;
        queue_data[bmsk_queue].tx_s = 0;
        queue_data[bmsk_queue].up = 0;

        /* Set QC queue to safe state (known size, no events, zeroed ptrs) */
        txq.event_type   = NFP_QC_STS_LO_EVENT_TYPE_NEVER;
        txq.size         = 0;
        qc_init_queue(PCIE_ISL, NFD_NATQ2QC(queue, NFD_IN_TX_QUEUE), &txq);
    }
}


/**
 * Check active queues for new work, updating pending_bmsk and write pointers
 *
 * This function uses the active mask to narrow down likely queues to check for
 * outstanding work.  When queues with outstanding work are found, the pending
 * mask is updated.  If a queue is marked active but found to have no pending
 * work, the function attempts to mark it as inactive.  Per queue write pointers
 * are updated and the serviced pointer used as input.
 */
__intrinsic int
gather_check_queues()
{
    __gpr   unsigned int queue;
    int     ret;
    int     retry;
    unsigned int qc_queue;
    __gpr   unsigned int queue_base_addr;
    __xread unsigned int wptr_raw;
    struct nfp_qc_sts_hi wptr;
    SIGNAL  qc_sig;
    __gpr unsigned int ptr_inc;

    /*
     * Look for a queue to reread the write pointer.
     * We try up to MAX_RETRIES times. If we do not find one,
     * we reread the write pointer for the last queue.
     * This loop breaks as soon as a queue is found.
     * If there are no active queues, the method returns.
     */
    for (retry = 0; retry < NFD_IN_MAX_RETRIES; retry++) {
        /* Select a queue to check */
        ret = select_queue(&queue, &active_bmsk);

        if (ret) {
            /* Yield */
            ctx_swap();
            return 0;
        }

        /* Test remaining work on the queue */
        /* XXX the factor of 7 seems about right for PCI.IN. */
        if ((queue_data[queue].tx_w - queue_data[queue].tx_s) <
            (7 * NFD_IN_MAX_BATCH_SZ)) {
            /* We have found a queue to poll.
             * Break and reread wptr */
            break;
        }
    }

    /*
     * Read latest wptr value
     * This is not necessary if wptr - sptr > n * MAX_BATCH_SIZE!
     */
    qc_queue = NFD_NATQ2QC(NFD_BMQ2NATQ(queue), NFD_IN_TX_QUEUE);
    __qc_read(PCIE_ISL, qc_queue, QC_WPTR, &wptr_raw, ctx_swap, &qc_sig);
    wptr.__raw = wptr_raw;

    /*
     * Update queues[queue].wptr, stored as a unsigned int for efficient
     * processing in future operations, and as a record of total wptr
     * updates on the queue.
     */
    ptr_inc = (int) wptr.writeptr - queue_data[queue].tx_w;
    ptr_inc &= queue_data[queue].ring_sz_msk;
    queue_data[queue].tx_w += ptr_inc;

    /*
     * Check for pending work
     */
    /* XXX does this work with wrapping? */
    if ((queue_data[queue].tx_w - queue_data[queue].tx_s) >
        NFD_IN_PENDING_TEST) {
        /* Mark the queue in the pending_bmsk */
        set_queue(&queue, &pending_bmsk);

        return 0; /* tx_w.empty can't be set */
    }

    /*
     * Test to see whether to leave queue active
     * If so, exit by returning
     */
    if (!wptr.empty) {
        return 0;
    }

    /*
     * Set the queue to not active and ping the queue to generate
     * an event for outstanding pointer writes
     */
    clear_queue(&queue, &active_bmsk);
    qc_ping_queue(PCIE_ISL, qc_queue, NFD_EVENT_DATA,
                  NFP_QC_STS_LO_EVENT_TYPE_NOT_EMPTY);

    return 0;
}


/**
 * Examine pending bitmasks and queue state to determine whether there are
 * any outstanding packets to process.  If there are, form a work batch
 * containing packets from the first queue with packets.  A batch may contain
 * up to MAX_TX_BATCH_SZ packets from a single queue.
 *
 * A batch message is placed in the next-neighbour ring for the ME, and the
 * descriptors are DMA'ed into the next slot in the CLS "desc_ring".
 */
__forceinline int
gather()
{
    int ret;
    __gpr unsigned int queue = 0;
    __gpr unsigned int tx_r_update_tmp;
    __gpr unsigned int tx_s_cp;
    __gpr int tx_r_correction;
    __gpr int idma;
    __gpr int ring;

    /* Stop enqueueing TX descriptor DMAs while PCIe reset state set. */
    if (NFD_RST_STATE_TEST_RST(PCIE_ISL)) {
        return 0;
    }

    /*
     * Before looking for a batch of work, we need to be able to issue a DMA
     * and we need space in the CLS rings. The CLS descriptor ring is sized to
     * hold more batches than the CLS ring, so checking for !cls_ring_full is
     * enough.
     */
    if ((dma_seq_issued != (NFD_IN_GATHER_MAX_IN_FLIGHT +
                            gather_dma_seq_compl)) &&
        !CLS_RING_EITHER_FULL(NFD_IN_BATCH_RING0_NUM, NFD_IN_BATCH_RING1_NUM)) {
        ret = select_queue(&queue, &pending_bmsk);

        /* No work to do. */
        if (ret) {
            return 0;
        }

        /*
         * Compute increase, strategy:
         * Round tx_s up to the next NFD_IN_MAX_BATCH_SZ multiple (64B multiple)
         * Compute the batch size that would produce this tx_s
         * Subtract this tx_s from tx_w. If positive or zero, leave batch size
         * unchanged.  If negative, correct tx_s and batch size by adding the
         * negative error.
         */
        ctassert(__is_log2(NFD_IN_MAX_BATCH_SZ));
        tx_s_cp = NFD_IN_MAX_BATCH_SZ + queue_data[queue].tx_s;
        tx_s_cp &= ~(NFD_IN_MAX_BATCH_SZ - 1);
        tx_r_correction = queue_data[queue].tx_w - tx_s_cp;
        tx_r_update_tmp = (NFD_IN_MAX_BATCH_SZ -
                           (queue_data[queue].tx_s & (NFD_IN_MAX_BATCH_SZ - 1)));
        if (tx_r_correction < 0) {
            tx_r_update_tmp += tx_r_correction;
        }

        /*
         * Check if there is a batch to handle
         */
        if (tx_r_update_tmp != 0) {
            /*
             * There is. Put a message on the work_ring.
             */
            struct nfd_in_batch_desc batch;
            unsigned int pcie_addr_off;
            unsigned int pcie_addr_hi_tmp, pcie_addr_lo_tmp;
            __xwrite struct nfp_pcie_dma_cmd descr;
            __xwrite struct nfd_in_batch_desc xbatch;
            SIGNAL batch_sig;
            SIGNAL dma_sig;

            /* logic to only send batches of 8, 4, 3, 2, 1 */
            /* batches of 7, 6, 5 will turn in to batches of 4 */
            if (tx_r_update_tmp != NFD_IN_FAST_PATH_BATCH_SZ) {
                if (tx_r_update_tmp > NFD_IN_MAX_NON_FAST_PATH_BATCH_SZ) {
                    tx_r_update_tmp = NFD_IN_MAX_NON_FAST_PATH_BATCH_SZ;
                }
            }

            /* Figure out which Issue DMA will receive this batch
             * Must be 0 if only one issue_dma ME in use */
#ifdef NFD_IN_HAS_ISSUE1
            idma = (((queue >> NFD_IN_ISSUE_DMA_QSHIFT) & 1) ^
                    NFD_IN_ISSUE_DMA_QXOR);
#else
            idma = 0;
#endif

            /*
             * Compute desc_ring and PCIe offsets
             * PCIe offset depends on tx_s, the total packets serviced on the
             * queue. desc_ring offset depends on the batches processed, each
             * batch having it's own slot in the ring.
             */
            pcie_addr_off = (queue_data[queue].tx_s &
                             queue_data[queue].ring_sz_msk);
            pcie_addr_off = pcie_addr_off * sizeof(struct nfd_in_tx_desc);
            if (idma == 0) {
                dma_issued0++;
                descr_tmp.cpp_addr_lo = desc_ring_base0 |
                                        ((dma_issued0 * NFD_IN_MAX_BATCH_SZ *
                                         sizeof(struct nfd_in_tx_desc)) &
                                         (DESC_RING_SZ - 1));

            } else {
                dma_issued1++;
                descr_tmp.cpp_addr_lo = desc_ring_base1 |
                                        ((dma_issued1 * NFD_IN_MAX_BATCH_SZ *
                                         sizeof(struct nfd_in_tx_desc)) &
                                         (DESC_RING_SZ - 1));
                idma_list |= 1 << (dma_seq_issued - gather_dma_seq_compl);
            }

            /*
             * Increment dma_seq_issued upfront to avoid ambiguity
             * about sequence number zero
             */
            dma_seq_issued++;

            /*
             * Populate the batch message
             */
            batch.__raw = 0;
            batch.queue = queue;
            batch.num = tx_r_update_tmp;
            xbatch.__raw = batch.__raw;
            ring = NFD_IN_BATCH_RING0_NUM + idma;
            cls_ring_put(ring, &xbatch, sizeof(xbatch), &batch_sig);

            /*
             * Prepare the "DMA"
             * Filling the descriptor can possibly be optimised either
             * by doing it all by hand, or playing with initialisation time.
             */

            /* Get 40bit PCIe address using an add with carry for the offset */
            pcie_addr_hi_tmp = queue_data[queue].ring_base_hi;
            pcie_addr_lo_tmp = queue_data[queue].ring_base_lo;
            __asm {
                /* XXX use ASM to perform +carry op explicitly */
                /* XXX semi-colons below technically start ASM comment */
                alu[pcie_addr_lo_tmp, pcie_addr_lo_tmp, +, pcie_addr_off];
                alu[pcie_addr_hi_tmp, pcie_addr_hi_tmp, +carry, 0];
            }
            descr_tmp.pcie_addr_hi = pcie_addr_hi_tmp;
            descr_tmp.pcie_addr_lo = pcie_addr_lo_tmp;
            descr_tmp.rid = queue_data[queue].requester_id;
            /* Can replace with ld_field instruction if 8bit seqn is enough */
            dma_seqn_set_event(&descr_tmp, NFD_IN_GATHER_EVENT_TYPE, 0,
                               dma_seq_issued);
            descr_tmp.length = ((tx_r_update_tmp *
                                 sizeof(struct nfd_in_tx_desc)) - 1);
            descr = descr_tmp;

            /*
             * Update tx_s before swapping
             */
            queue_data[queue].tx_s += tx_r_update_tmp;

            /*
             * Issue the DMA
             */
            __pcie_dma_enq(PCIE_ISL, &descr, NFD_IN_GATHER_DMA_QUEUE,
                           sig_done, &dma_sig);

            /* wait for ring put and the dma signal */
            wait_for_all(&batch_sig, &dma_sig);

        } else {
            /*
             * No batch to processes.
             * Clear pending_bmsk so we don't check it again
             * unless something resets the bitmask
             */
            clear_queue(&queue, &pending_bmsk);
        }
    }

    return 0;
}

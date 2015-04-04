/*
 * Copyright (C) 2014 Netronome Systems, Inc.  All rights reserved.
 *
 * @file          blocks/vnic/pci_out/cache_desc.c
 * @brief         Code to cache FL descriptors from pending queues
 */

#include <assert.h>
#include <nfp.h>

#include <nfp/pcie.h>
#include <std/event.h>

#include <nfp6000/nfp_cls.h>
#include <nfp6000/nfp_event.h>
#include <nfp6000/nfp_me.h>
#include <nfp6000/nfp_pcie.h>
#include <nfp6000/nfp_qc.h>

#include <vnic/nfd_common.h>
#include <vnic/pci_out.h>
#include <vnic/shared/nfd.h>
#include <vnic/shared/nfd_internal.h>
#include <vnic/shared/nfd_cfg.h>
#include <vnic/utils/dma_seqn.h>
#include <vnic/utils/qc.h>
#include <vnic/utils/qcntl.h>


/* Credit define consistency checks */
#if !defined(NFD_OUT_CREDITS_HOST_ISSUED) && !defined(NFD_OUT_CREDITS_NFP_CACHED)
#error "NFD credit type not specified"
#endif

#if defined(NFD_OUT_CREDITS_HOST_ISSUED) && defined(NFD_OUT_CREDITS_NFP_CACHED)
#error "Only one NFD credit type may be specified"
#endif

#ifdef NFD_OUT_CREDITS_HOST_ISSUED
#warning "NFD_OUT_CREDITS_HOST_ISSUED selected, use at own risk!"
#endif


#define NFD_OUT_FL_SZ_PER_QUEUE   \
    (NFD_OUT_FL_BUFS_PER_QUEUE * sizeof(struct nfd_out_fl_desc))


/*
 * Allocate memory at offset 0 in CTM for atomics.
 * Forcing this to zero on all PCIe islands makes code to access credits
 * simpler and more efficient throughout the system.
 */
#define NFD_ATOMICS_ALLOC_IND(_off)                                    \
    ASM(.alloc_mem nfd_out_atomics ctm+##_off island                   \
        (NFD_OUT_ATOMICS_SZ * NFD_OUT_MAX_QUEUES))
#define NFD_ATOMICS_ALLOC(_off) NFD_ATOMICS_ALLOC_IND(_off)


/*
 * State variables for PCI.OUT queue controller accesses
 */
static __xread struct qc_xfers rx_ap_xfers;

static volatile SIGNAL rx_ap_s0;
static volatile SIGNAL rx_ap_s1;
static volatile SIGNAL rx_ap_s2;
static volatile SIGNAL rx_ap_s3;

__shared __gpr struct qc_bitmask active_bmsk;
__shared __gpr struct qc_bitmask urgent_bmsk;


/*
 * Memory for PCI.OUT
 */
/*
 * For PCI.OUT ME0, queue_data is forced to LMEM 0 so that it can be
 * accessed more efficiently.  We allocate the memory via the
 * ".alloc_mem lmem+offset" notation, and then declare queue_data
 * as a pointer, which we set to zero.
 * XXX NFCC doesn't seem to acknowledge linker allocated LMEM,
 * so we also specify -Qlm_start=512 on the command line to reserve
 * the LMEM from NFCC's perspective.
 */
__asm { .alloc_mem queue_data_mem lmem+0 me             \
        (NFD_OUT_QUEUE_INFO_SZ * NFD_OUT_MAX_QUEUES)};
__shared __lmem struct nfd_out_queue_info *queue_data;

__shared __lmem unsigned int fl_cache_pending[NFD_OUT_FL_MAX_IN_FLIGHT];

/* NFD credits are fixed at offset zero in CTM */
NFD_ATOMICS_ALLOC(NFD_OUT_CREDITS_BASE);


__export __ctm __align(NFD_OUT_MAX_QUEUES * NFD_OUT_FL_SZ_PER_QUEUE)
    struct nfd_out_fl_desc
    fl_cache_mem[NFD_OUT_MAX_QUEUES][NFD_OUT_FL_BUFS_PER_QUEUE];

static __gpr unsigned int fl_cache_mem_addr_lo;


/*
 * Sequence numbers and update variables
 */
__gpr unsigned int fl_cache_dma_seq_issued = 0;
__gpr unsigned int fl_cache_dma_seq_compl = 0;
__gpr unsigned int fl_cache_dma_seq_served = 0;
static volatile __xread unsigned int fl_cache_event_xfer;
static SIGNAL fl_cache_event_sig;

static __gpr struct nfp_pcie_dma_cmd descr_tmp;


/**
 *Increment an atomic counter stored in local CTM
 * @param base      Start address of structure to increment
 * @param queue     Queue within structure to increment
 * @param val       Value to add
 * @param counter   Counter to increment
 *
 * XXX replace this command with suitable flowenv alternative when available.
 */
__intrinsic void
_add_imm(unsigned int base, unsigned int queue, unsigned int val,
         unsigned int counter)
{
    unsigned int ind;
    ctassert(__is_ct_const(counter));

    queue = queue * NFD_OUT_ATOMICS_SZ | counter;
    ind = (NFP_MECSR_PREV_ALU_LENGTH(8) | NFP_MECSR_PREV_ALU_OV_LEN |
           NFP_MECSR_PREV_ALU_OVE_DATA(2));

    __asm alu[--, ind, or, val, <<16];
    __asm mem[add_imm, --, base, queue, 1], indirect_ref;
}


/**
 * Zero an atomic counter stored in local CTM
 * @param base      Start address of structure to zero
 * @param queue     Queue within structure to zero
 *
 * XXX replace this command with suitable flowenv alternative when available.
 */
__intrinsic void
_zero_imm(unsigned int base, unsigned int queue, size_t size)
{
    unsigned int ind;
    unsigned int count = size>>2;

    ctassert(__is_ct_const(size));
    ctassert(size <= 32);

    queue = queue * NFD_OUT_ATOMICS_SZ;
    ind = (NFP_MECSR_PREV_ALU_LENGTH(8 + (count - 1)) |
           NFP_MECSR_PREV_ALU_OV_LEN |
           NFP_MECSR_PREV_ALU_OVE_DATA(2));

    __asm alu[--, --, B, ind];
    __asm mem[atomic_write_imm, --, base, queue, \
              __ct_const_val(count)], indirect_ref;
}


/**
 * Perform once off, CTX0-only initialisation of the FL descriptor cacher
 */
void
cache_desc_setup_shared()
{
    struct pcie_dma_cfg_one cfg;

    queue_data = 0;

    /* Zero bitmasks */
    init_bitmasks(&active_bmsk);
    init_bitmasks(&urgent_bmsk);

    /* Configure autopush filters */
    init_bitmask_filters(&rx_ap_xfers, &rx_ap_s0, &rx_ap_s1, &rx_ap_s2,
                         &rx_ap_s3,(NFD_OUT_Q_EVENT_DATA<<6) | NFD_OUT_Q_START,
                         NFP_EVENT_TYPE_FIFO_ABOVE_WM,
                         NFD_OUT_Q_EVENT_START);

    dma_seqn_ap_setup(NFD_OUT_FL_EVENT_FILTER, NFD_OUT_FL_EVENT_FILTER,
                      NFD_OUT_FL_EVENT_TYPE, &fl_cache_event_xfer,
                      &fl_cache_event_sig);

    /*
     * Set up RX_FL_CFG_REG DMA Config Register
     */
    cfg.__raw = 0;
#ifdef NFD_VNIC_NO_HOST
    /* Use signal_only for seqn num generation
     * Don't actually DMA data */
    cfg.signal_only = 1;
#else
    cfg.signal_only = 0;
#endif
    cfg.end_pad     = 0;
    cfg.start_pad   = 0;
    /* Ordering settings? */
    cfg.target_64   = 1;
    cfg.cpp_target  = 7;
    pcie_dma_cfg_set_one(PCIE_ISL, NFD_OUT_FL_CFG_REG, cfg);


    /*
     * Initialise a DMA descriptor template
     * RequesterID (rid), CPP address, and PCIe address will be
     * overwritten per transaction.
     * For dma_mode, we technically only want to overwrite the "source"
     * field, i.e. 12 of the 16 bits.
     */
    descr_tmp.length = NFD_OUT_FL_BATCH_SZ * sizeof(struct nfd_out_fl_desc) - 1;
    descr_tmp.rid_override = 1;
    descr_tmp.trans_class = 0;
    descr_tmp.cpp_token = 0;
    descr_tmp.dma_cfg_index = NFD_OUT_FL_CFG_REG;
    descr_tmp.cpp_addr_hi = (((unsigned long long) fl_cache_mem >> 8) &
                             0xff000000);

    /* Initialise addresses of the FL cache and credits */
    fl_cache_mem_addr_lo = ((unsigned long long) fl_cache_mem & 0xffffffff);
}


/**
 * Perform per CTX configuration of the FL descriptor cacher.
 *
 * This method populates values required by threads calling
 * "cache_desc_compute_fl_addr" as a service method.
 */
void
cache_desc_setup()
{
    fl_cache_mem_addr_lo = ((unsigned long long) fl_cache_mem & 0xffffffff);
}


/**
 * Setup PCI.OUT configuration fro the vNIC specified in cfg_msg
 * @param cfg_msg   Standard configuration message
 *
 * This method handles all PCI.OUT configuration related to bringing a vNIC up
 * or down.
 */
__intrinsic void
cache_desc_vnic_setup(struct nfd_cfg_msg *cfg_msg)
{
    struct qc_queue_config rxq;
    unsigned int queue_s;
    unsigned char ring_sz;
    unsigned int ring_base[2];
    __gpr unsigned int bmsk_queue;

    nfd_cfg_proc_msg(cfg_msg, &queue_s, &ring_sz, ring_base, NFD_CFG_PCI_OUT);

    if (cfg_msg->error || !cfg_msg->interested) {
        return;
    }

    queue_s = NFD_BUILD_NATQ(cfg_msg->vnic, queue_s);
    bmsk_queue = NFD_NATQ2BMQ(queue_s);

    rxq.watermark    = NFP_QC_STS_HI_WATERMARK_32; /* XXX Tune */
    rxq.event_data   = NFD_OUT_Q_EVENT_DATA;
    rxq.ptr          = 0;

    if (cfg_msg->up_bit && !queue_data[bmsk_queue].up) {
        /* Up the queue:
         * - Set ring size and requester ID info
         * - (Re)clear queue pointers in case something changed them
         *   while down */
        queue_data[bmsk_queue].fl_w = 0;
        queue_data[bmsk_queue].fl_s = 0;
        queue_data[bmsk_queue].ring_sz_msk = ((1 << ring_sz) - 1);
        queue_data[bmsk_queue].requester_id = 0;
        if (cfg_msg->vnic != NFD_MAX_VFS) {
            queue_data[bmsk_queue].requester_id = (cfg_msg->vnic +
                                                   NFD_CFG_VF_OFFSET);
        }
        queue_data[bmsk_queue].spare0 = 0;
        queue_data[bmsk_queue].up = 1;
        queue_data[bmsk_queue].ring_base_hi = ring_base[1] & 0xFF;
        queue_data[bmsk_queue].ring_base_lo = ring_base[0];
        queue_data[bmsk_queue].fl_a = 0;
        queue_data[bmsk_queue].fl_u = 0;
        queue_data[bmsk_queue].rx_w = 0;

        /* Reset credits */
        _zero_imm(NFD_OUT_CREDITS_BASE, bmsk_queue, NFD_OUT_ATOMICS_SZ);

        rxq.event_type   = NFP_QC_STS_LO_EVENT_TYPE_HI_WATERMARK;
        rxq.size         = ring_sz - 8; /* XXX add define for size shift */
        qc_init_queue(PCIE_ISL, (queue_s<<1) | NFD_OUT_Q_START, &rxq);

    } else if (!cfg_msg->up_bit && queue_data[bmsk_queue].up) {
        /* XXX consider what is required for PCI.OUT! */
        /* Down the queue:
         * - Prevent it issuing events
         * - Clear active_msk bit
         * - Clear pending_msk bit
         * - Clear the proc bitmask bit?
         * - Clear tx_w and tx_s
         * - Try to count pending packets? Host responsibility? */

        /* Clear active and urgent bitmask bits */
        clear_queue(&bmsk_queue, &active_bmsk);
        clear_queue(&bmsk_queue, &urgent_bmsk);

        /* Clear queue LM state */
        /* XXX check what is required for recycling host buffers */
        queue_data[bmsk_queue].fl_w = 0;
        queue_data[bmsk_queue].fl_s = 0;
        queue_data[bmsk_queue].up = 0;
        queue_data[bmsk_queue].fl_a = 0;
        queue_data[bmsk_queue].fl_u = 0;
        queue_data[bmsk_queue].rx_w = 0;

        /* Reset credits */
        _zero_imm(NFD_OUT_CREDITS_BASE, bmsk_queue, NFD_OUT_ATOMICS_SZ);

        /* Set QC queue to safe state (known size, no events, zeroed ptrs) */
        /* XXX configure both queues without swapping? */
        rxq.event_type   = NFP_QC_STS_LO_EVENT_TYPE_NEVER;
        rxq.size         = 0;
        qc_init_queue(PCIE_ISL, (queue_s<<1) | NFD_OUT_Q_START, &rxq);
    }
}


/**
 * Perform checks and issue a FL batch fetch
 * @param queue     Queue selected for the fetch
 *
 * This method uses and maintains LM queue state to determine whether to fetch
 * a batch of FL descriptors.  If the state indicates that there is a batch to
 * fetch and space to put it, then the fetch will proceed.  If not, the queue
 * controller queue is reread to update the state.  The "urgent" bit for the
 * queue is also cleared by this method.
 */
__intrinsic int
_fetch_fl(__gpr unsigned int *queue)
{
    unsigned int qc_queue;
    unsigned int pcie_addr_off;
    unsigned int fl_cache_off;
    __xwrite struct nfp_pcie_dma_cmd descr;
    SIGNAL qc_sig;
    int space_chk;
    int ret;        /* Required to ensure __intrinsic_begin|end pairing */

    qc_queue = (NFD_BMQ2NATQ(*queue) << 1) | NFD_OUT_Q_START;

    /* Is there a batch to get from this queue?
     * If the queue is active or urgent there should be. */
    if ((queue_data[*queue].fl_w - queue_data[*queue].fl_s) <
        NFD_OUT_FL_BATCH_SZ) {
        __xread unsigned int wptr_raw;
        struct nfp_qc_sts_hi wptr;
        unsigned int ptr_inc;

        /* Reread fl_w and repeat check */
        __qc_read(PCIE_ISL, qc_queue, QC_WPTR, &wptr_raw, ctx_swap, &qc_sig);
        wptr.__raw = wptr_raw;

        ptr_inc = (unsigned int) wptr.writeptr - queue_data[*queue].fl_w;
        ptr_inc &= queue_data[*queue].ring_sz_msk;
        queue_data[*queue].fl_w += ptr_inc;
#ifdef NFD__OUT_CREDITS_HOST_ISSUED
        _add_imm(NFD_OUT_CREDITS_BASE, *queue, ptr_inc, NFD_OUT_ATOMICS_CREDIT);
#endif
        if (!wptr.wmreached) {
            /* Mark the queue not urgent
             * The credit schemes ensure that when the FL buffers available are
             * depleted, the queue is not entitled to have descriptors pending.
             * The queue will be (re)marked urgent as packets on the queue
             * arrive until the final buffers and credits are exhausted.
             */
            clear_queue(queue, &urgent_bmsk);

            /* Mark the queue not active */
            clear_queue(queue, &active_bmsk);
            qc_ping_queue(PCIE_ISL, qc_queue, NFD_OUT_Q_EVENT_DATA,
                          NFP_QC_STS_LO_EVENT_TYPE_HI_WATERMARK);

            /* Indicate work done on queue */
            return 0;
        }
    }

    /* We have a batch available, is there space to put it?
     * Space = ring size - (fl_s - rx_w). We require
     * space >= batch size. */
    space_chk = ((NFD_OUT_FL_BUFS_PER_QUEUE - NFD_OUT_FL_BATCH_SZ) +
                 queue_data[*queue].rx_w - queue_data[*queue].fl_s);
    if (space_chk >= 0) {
        __xwrite unsigned int qc_xfer;
        unsigned int pending_slot;
        SIGNAL dma_sig;

        /* Increment fl_cache_dma_seq_issued upfront
         * to avoid ambiguity about sequence number zero */
        fl_cache_dma_seq_issued++;

        /* Compute DMA address offsets */
        pcie_addr_off = (queue_data[*queue].fl_s &
                         queue_data[*queue].ring_sz_msk);
        pcie_addr_off = pcie_addr_off * sizeof(struct nfd_out_fl_desc);

        /* Complete descriptor */
        descr_tmp.pcie_addr_hi = queue_data[*queue].ring_base_hi;
        descr_tmp.pcie_addr_lo = (queue_data[*queue].ring_base_lo +
                                  pcie_addr_off);
        descr_tmp.cpp_addr_lo =
            cache_desc_compute_fl_addr(queue, queue_data[*queue].fl_s);
        descr_tmp.rid = queue_data[*queue].requester_id;
        /* Can replace with ld_field instruction if 8bit seqn is enough */
        pcie_dma_set_event(&descr_tmp, NFD_OUT_FL_EVENT_TYPE,
                           fl_cache_dma_seq_issued);
        descr = descr_tmp;

        /* Increment fl_s and QC FL.R before swapping
         * It is safe to increment fl_s because the host will not
         * overwrite the FL descriptor there, the firmware will overwrite
         * it once it has used the FL descriptor and written the RX descriptor.
         */
        queue_data[*queue].fl_s += NFD_OUT_FL_BATCH_SZ;
        __qc_add_to_ptr(PCIE_ISL, qc_queue, QC_RPTR, NFD_OUT_FL_BATCH_SZ,
                        &qc_xfer, sig_done, &qc_sig);

        /* Add batch message to LM queue
         * XXX check defer slots filled */
        pending_slot = (fl_cache_dma_seq_issued & (NFD_OUT_FL_MAX_IN_FLIGHT -1));
        fl_cache_pending[pending_slot] = *queue;

        /* Issue DMA */
        __pcie_dma_enq(PCIE_ISL, &descr, NFD_OUT_FL_DMA_QUEUE,
                       sig_done, &dma_sig);
        wait_for_all(&dma_sig, &qc_sig);

        /* Indicate work done on queue */
        ret = 0;
    } else {
        /* The cache is full, so the queue is not "urgent". */
        clear_queue(queue, &urgent_bmsk);

        /* Swap to give other threads a chance to run */
        ctx_swap();

        /* Indicate no work done on queue */
        ret = -1;
    }

    return ret;
}


/**
 * Check whether fl_cache_dma_seq_compl can be advanced and, if so, process
 * the messages in the fl_cache_pending queue.  Two dependent LM accesses are
 * required to process each message, so cycles lost to LM pointer setup are
 * hard to avoid.
 */
void
_complete_fetch()
{
    unsigned int queue_c;
    unsigned int pending_slot;

    if (signal_test(&fl_cache_event_sig)) {
        dma_seqn_advance(&fl_cache_event_xfer, &fl_cache_dma_seq_compl);

        event_cls_autopush_filter_reset(
            NFD_OUT_FL_EVENT_FILTER,
            NFP_CLS_AUTOPUSH_STATUS_MONITOR_ONE_SHOT_ACK,
            NFD_OUT_FL_EVENT_FILTER);
        __implicit_write(&fl_cache_event_sig);

        /* XXX how many updates can we receive at once? Do we need to
         * throttle this? */
        while (fl_cache_dma_seq_compl != fl_cache_dma_seq_served) {
            /* Increment fl_cache_dma_seq_served upfront
             * to avoid ambiguity about sequence number zero */
            fl_cache_dma_seq_served++;

            /* Extract queue from the fl_cache_pending message */
            pending_slot = (fl_cache_dma_seq_served &
                            (NFD_OUT_FL_MAX_IN_FLIGHT -1));
            queue_c = fl_cache_pending[pending_slot];

#ifdef NFD_OUT_CREDITS_NFP_CACHED
            _add_imm(NFD_OUT_CREDITS_BASE, queue_c, NFD_OUT_FL_BATCH_SZ,
                     NFD_OUT_ATOMICS_CREDIT);
#endif

            /* Increment queue available pointer by one batch
             * NB: If NFP cached credits are not used, there is nothing to
             * fill the LM pointer usage slots */
            queue_data[queue_c].fl_a += NFD_OUT_FL_BATCH_SZ;
        }
    } else {
        /* Swap to give other threads a chance to run */
        ctx_swap();
    }
}


/**
 * Check the bitmask filters and process completed fetches
 */
__forceinline void
cache_desc_complete_fetch()
{
    /* Check bitmasks */
    check_bitmask_filters(&active_bmsk, &rx_ap_xfers, &rx_ap_s0, &rx_ap_s1,
              &rx_ap_s2, &rx_ap_s3, NFD_OUT_Q_EVENT_START);

    /* Process up to the latest fl_cache_dma_seq_compl */
    _complete_fetch();
}


/**
 * Check the active bitmask for a queue to service, and
 * attempt to work on that queue.  The "urgent" and "active"
 * bitmasks are updated as processing progresses.
 *
 * This method may be called multiple separate times from the
 * dispatch loop for finer balance of FL DMAs with other tasks.
 */
__forceinline void
cache_desc_check_active()
{
    __gpr unsigned int queue;
    int ret = 0;

    if ((fl_cache_dma_seq_issued - fl_cache_dma_seq_served) <
        NFD_OUT_FL_MAX_IN_FLIGHT) {
        __critical_path();

        /* Look for an active queue */
        ret = select_queue(&queue, &active_bmsk);
        if (ret) {
            /* No active queues found */
            return;
        }

        /* Try to work on that queue */
        ret = _fetch_fl(&queue);
    }
}


/**
 * Check the urgent bitmask for a queue to service, and
 * attempt to work on that queue.  The "urgent" and "active"
 * bitmasks are updated as processing progresses.
 *
 * This method may be called multiple separate times from the
 * dispatch loop for finer balance of FL DMAs with other tasks.
 */
__forceinline void
cache_desc_check_urgent()
{
    __gpr unsigned int queue;
    int ret = 0;

    if ((fl_cache_dma_seq_issued - fl_cache_dma_seq_served) <
        NFD_OUT_FL_MAX_IN_FLIGHT) {
        __critical_path();

        /* Look for an urgent queue */
        ret = select_queue(&queue, &urgent_bmsk);
        if (ret) {
            /* No urgent queues found */
            return;
        }

        /* Try to work on that queue */
        ret = _fetch_fl(&queue);
    }
}


/**
 * Service function to determine the address of a specific FL entry
 * @param queue     Bitmask numbered queue
 * @param seq       Current "fl_u" sequence number for the queue
 */
__intrinsic unsigned int
cache_desc_compute_fl_addr(__gpr unsigned int *queue, unsigned int seq)
{
    unsigned int ret;

    ret = seq & (NFD_OUT_FL_BUFS_PER_QUEUE - 1);
    ret *= sizeof(struct nfd_out_fl_desc);
    ret |= (*queue * NFD_OUT_FL_SZ_PER_QUEUE );
    ret |= fl_cache_mem_addr_lo;

    return ret;
}

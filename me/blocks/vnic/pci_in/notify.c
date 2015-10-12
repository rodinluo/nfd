/*
 * Copyright (C) 2014 Netronome Systems, Inc.  All rights reserved.
 *
 * @file          blocks/vnic/pci_in/notify.c
 * @brief         Code to notify host and app that packet was transmitted
 */

#include <assert.h>
#include <nfp.h>

#include <nfp/me.h>

#include <nfp6000/nfp_cls.h>
#include <nfp6000/nfp_me.h>

#include <nfp/mem_ring.h>

#include <vnic/nfd_common.h>
#include <vnic/pci_in.h>
#include <vnic/shared/nfd.h>
#include <vnic/shared/nfd_internal.h>
/*#include <vnic/utils/cls_ring.h> */ /* XXX THS-50 workaround */
#include <vnic/utils/ctm_ring.h> /* XXX THS-50 workaround */
#include <vnic/utils/qc.h>
#include <vnic/utils/qcntl.h>

/* XXX assume this runs on PCI.IN ME0 */

struct _issued_pkt_batch {
    struct nfd_in_issued_desc pkt0;
    struct nfd_in_issued_desc pkt1;
    struct nfd_in_issued_desc pkt2;
    struct nfd_in_issued_desc pkt3;
};

struct _pkt_desc_batch {
    struct nfd_in_pkt_desc pkt0;
    struct nfd_in_pkt_desc pkt1;
    struct nfd_in_pkt_desc pkt2;
    struct nfd_in_pkt_desc pkt3;
};

__shared __gpr unsigned int data_dma_seq_served = 0;
__shared __gpr unsigned int data_dma_seq_compl = 0;
static __gpr unsigned int data_dma_seq_sent = 0;

/* Signals and transfer registers for sequence number reflects */
__visible volatile __xread unsigned int nfd_in_data_compl_refl_in;
__visible volatile SIGNAL nfd_in_data_compl_refl_sig;
__remote volatile __xread unsigned int nfd_in_data_served_refl_in;
__remote volatile SIGNAL nfd_in_data_served_refl_sig;
static __xwrite unsigned int nfd_in_data_served_refl_out = 0;

static unsigned int nfd_in_lso_cntr_addr = 0;
static __gpr mem_ring_addr_t nfd_in_issued_lso_ring_addr;
static __gpr unsigned int nfd_in_issued_lso_ring_num;

static SIGNAL wq_sig0, wq_sig1, wq_sig2, wq_sig3;
static SIGNAL msg_sig, qc_sig;
static SIGNAL get_order_sig;    /* Signal for reordering before issuing get */
static SIGNAL msg_order_sig;    /* Signal for reordering on message return */
static SIGNAL_MASK wait_msk;
static unsigned int next_ctx;

__xwrite struct _pkt_desc_batch batch_out;
__xread unsigned int qc_xfer;

/* XXX use CLS ring API when available */
/* XXX THS-50 workaround, use CTM instead of CLS rings */
__export __ctm __align(sizeof(struct nfd_in_issued_desc) * NFD_IN_ISSUED_RING_SZ)
    struct nfd_in_issued_desc nfd_in_issued_ring[NFD_IN_ISSUED_RING_SZ];

/* XXX declare dst_q counters in LM */

#ifdef NFD_IN_WQ_SHARED

#define NFD_IN_RINGS_MEM_IND2(_isl, _emem)                              \
    ASM(.alloc_mem nfd_in_rings_mem0 _emem global                       \
        (NFD_IN_WQ_SZ * NFD_IN_NUM_WQS) (NFD_IN_WQ_SZ * NFD_IN_NUM_WQS))
#define NFD_IN_RINGS_MEM_IND1(_isl, _emem) NFD_IN_RINGS_MEM_IND2(_isl, _emem)
#define NFD_IN_RINGS_MEM_IND0(_isl)                    \
    NFD_IN_RINGS_MEM_IND1(_isl, NFD_IN_WQ_SHARED)
#define NFD_IN_RINGS_MEM(_isl) NFD_IN_RINGS_MEM_IND0(_isl)

#define NFD_IN_RING_INIT_IND0(_isl, _num)                               \
    NFD_IN_RING_NUM_ALLOC(_isl, _num)                                   \
    ASM(.declare_resource nfd_in_ring_mem_res0##_num                    \
        global NFD_IN_WQ_SZ nfd_in_rings_mem0)                          \
    ASM(.alloc_resource nfd_in_ring_mem0##_num                          \
        nfd_in_ring_mem_res0##_num global NFD_IN_WQ_SZ NFD_IN_WQ_SZ)    \
    ASM(.init_mu_ring nfd_in_ring_num0##_num nfd_in_ring_mem0##_num)
#define NFD_IN_RING_INIT(_isl, _num) NFD_IN_RING_INIT_IND0(_isl, _num)

#else /* !NFD_IN_WQ_SHARED */

#define NFD_IN_RINGS_MEM_IND2(_isl, _emem)                              \
    ASM(.alloc_mem nfd_in_rings_mem##_isl _emem global                  \
        (NFD_IN_WQ_SZ * NFD_IN_NUM_WQS) (NFD_IN_WQ_SZ * NFD_IN_NUM_WQS))
#define NFD_IN_RINGS_MEM_IND1(_isl, _emem) NFD_IN_RINGS_MEM_IND2(_isl, _emem)
#define NFD_IN_RINGS_MEM_IND0(_isl)                    \
    NFD_IN_RINGS_MEM_IND1(_isl, NFD_PCIE##_isl##_EMEM)
#define NFD_IN_RINGS_MEM(_isl) NFD_IN_RINGS_MEM_IND0(_isl)

#define NFD_IN_RING_INIT_IND0(_isl, _num)                               \
    NFD_IN_RING_NUM_ALLOC(_isl, _num)                                   \
    ASM(.declare_resource nfd_in_ring_mem_res##_isl##_num               \
        global NFD_IN_WQ_SZ nfd_in_rings_mem##_isl)                     \
    ASM(.alloc_resource nfd_in_ring_mem##_isl##_num                     \
        nfd_in_ring_mem_res##_isl##_num global NFD_IN_WQ_SZ NFD_IN_WQ_SZ) \
    ASM(.init_mu_ring nfd_in_ring_num##_isl##_num nfd_in_ring_mem##_isl##_num)
#define NFD_IN_RING_INIT(_isl, _num) NFD_IN_RING_INIT_IND0(_isl, _num)

#endif /* NFD_IN_WQ_SHARED */


NFD_IN_RINGS_MEM(PCIE_ISL);

#if NFD_IN_NUM_WQS > 0
    NFD_IN_RING_INIT(PCIE_ISL, 0);
#else
    #error "NFD_IN_NUM_WQS must be a power of 2 between 1 and 8"
#endif

#if NFD_IN_NUM_WQS > 1
    NFD_IN_RING_INIT(PCIE_ISL, 1);
#endif

#if NFD_IN_NUM_WQS > 2
    NFD_IN_RING_INIT(PCIE_ISL, 2);
    NFD_IN_RING_INIT(PCIE_ISL, 3);
#endif

#if NFD_IN_NUM_WQS > 4
    NFD_IN_RING_INIT(PCIE_ISL, 4);
    NFD_IN_RING_INIT(PCIE_ISL, 5);
    NFD_IN_RING_INIT(PCIE_ISL, 6);
    NFD_IN_RING_INIT(PCIE_ISL, 7);
#endif

#if NFD_IN_NUM_WQS > 8
    #error "NFD_IN_NUM_WQS > 8 is not supported"
#endif


static __shared mem_ring_addr_t wq_raddr;
static __shared unsigned int wq_num_base;


#ifdef NFD_IN_ADD_SEQN
#if (NFD_IN_NUM_WQS == 1)
/* Add sequence numbers, using a shared GPR to store */
static __shared __gpr dst_q_seqn = 0;

#define NFD_IN_ADD_SEQN_PROC(_num_seq, _seq)                            \
    do {                                                                \
        _seq = dst_q_seqn;                                              \
        dst_q_seqn += _num_seq;                                         \
    } while (0)

#else
/* Add sequence numbers, using a LM to store */
static __shared __lmem dst_q_seqn[NFD_IN_NUM_WQS];

#define NFD_IN_ADD_SEQN_PROC(_num_seq, _seq)                            \
    do {                                                                \
        _seq = dst_q_seqn[dst_q];                                       \
        dst_q_seqn[dst_q] += _num_seq;                                  \
    } while (0)

#endif

#else
/* Null sequence number add */
#define NFD_IN_ADD_SEQN_PROC(_num_seq, _seq)                            \
    do {                                                                \
        _seq = 0;                                                       \
    } while (0)

#endif


/*
 * reflect_data() available from gather.c, imported before this file.
 */


/**
 * Perform shared configuration for notify
 */
void
notify_setup_shared()
{
#ifdef NFD_IN_WQ_SHARED
    wq_num_base = NFD_RING_LINK(0, nfd_in, 0);
    wq_raddr = (unsigned long long) NFD_EMEM_SHARED(NFD_IN_WQ_SHARED) >> 8;
#else
    wq_num_base = NFD_RING_LINK(PCIE_ISL, nfd_in, 0);
    wq_raddr = (unsigned long long) NFD_EMEM_LINK(PCIE_ISL) >> 8;
#endif

    nfd_in_issued_lso_ring_num = NFD_RING_LINK(PCIE_ISL, nfd_in_issued_lso,
                                               0);
    nfd_in_issued_lso_ring_addr = ((((unsigned long long)
                                      NFD_EMEM_LINK(PCIE_ISL)) >> 32) << 24);

    /* Kick off ordering */
    reorder_start(NFD_IN_NOTIFY_START_CTX, &msg_order_sig);
    reorder_start(NFD_IN_NOTIFY_START_CTX, &get_order_sig);
}


/**
 * Perform per context initialisation (for CTX 1 to 7)
 */
void
notify_setup()
{
    if (ctx() != 0) {
        wait_msk = __signals(&msg_sig, &msg_order_sig);
        next_ctx = reorder_get_next_ctx(NFD_IN_NOTIFY_START_CTX);
    }
}


#ifdef NFD_VNIC_DBG_CHKS
#define _NOTIFY_MU_CHK                                                  \
    if ((batch_in.pkt##_pkt##.__raw[1] & NFD_MU_PTR_DBG_MSK) == 0) {    \
        halt();                                                         \
    }
#else
#define _NOTIFY_MU_CHK
#endif

#ifdef NFD_IN_LSO_CNTR_ENABLE
#define _LSO_END_PKTS_TO_ME_WQ_CNTR(_flags)                              \
        if (_flags & PCIE_DESC_TX_LSO) {                          \
            NFD_IN_LSO_CNTR_INCR(nfd_in_lso_cntr_addr,                   \
                         NFD_IN_LSO_CNTR_T_NOTIFY_LSO_END_PKTS_TO_ME_WQ);\
        }
#else
#define _LSO_END_PKTS_TO_ME_WQ_CNTR(_flags)
#endif

//        _NOTIFY_MU_CHK;                       \

#define _NOTIFY_PROC(_pkt)                                                   \
do {                                                                         \
    unsigned int i;                                                          \
    unsigned int start_lso_seq = 0;                                          \
    unsigned int seq = 0;                                                    \
    unsigned int ret;                                                        \
    unsigned int num_lso_to_read = batch_in.pkt##_pkt##.sp0;                 \
    __xread struct nfd_in_pkt_desc lso_pkt;                                  \
    SIGNAL lso_sig;                                                          \
    SIGNAL_PAIR lso_sig_pair;                                                \
                                                                             \
    NFD_IN_LSO_CNTR_INCR(nfd_in_lso_cntr_addr,                               \
                         NFD_IN_LSO_CNTR_T_NOTIFY_ALL_PKT_DESC);             \
    /* finished packet and no LSO */                                         \
    if ((batch_in.pkt##_pkt##.eop) && (num_lso_to_read == 0)) {              \
        NFD_IN_LSO_CNTR_INCR(nfd_in_lso_cntr_addr,                           \
                             NFD_IN_LSO_CNTR_T_NOTIFY_NON_LSO_PKT_DESC);     \
        __critical_path();                                                   \
        pkt_desc_tmp.sp0 = 0;                                                \
        pkt_desc_tmp.offset = batch_in.pkt##_pkt##.offset;                   \
        dst_q = 0;                                                           \
        NFD_IN_ADD_SEQN_PROC(1, seq);                                        \
        pkt_desc_tmp.reserved = seq;                                         \
        dst_q |= wq_num_base;                                                \
        batch_out.pkt##_pkt##.__raw[0] = pkt_desc_tmp.__raw[0];              \
        batch_out.pkt##_pkt##.__raw[1] = batch_in.pkt##_pkt##.__raw[1];      \
        batch_out.pkt##_pkt##.__raw[2] = batch_in.pkt##_pkt##.__raw[2];      \
        batch_out.pkt##_pkt##.__raw[3] = batch_in.pkt##_pkt##.__raw[3];      \
                                                                             \
        __mem_workq_add_work(dst_q, wq_raddr, &batch_out.pkt##_pkt,          \
                             out_msg_sz, out_msg_sz, sig_done,               \
                             &wq_sig##_pkt);                                 \
    } else if (num_lso_to_read != 0) {                                       \
        dst_q = 0;                                                           \
        NFD_IN_ADD_SEQN_PROC(num_lso_to_read, start_lso_seq);                \
        /* LSO packets to read */                                            \
        NFD_IN_LSO_CNTR_INCR(nfd_in_lso_cntr_addr,                           \
                             NFD_IN_LSO_CNTR_T_NOTIFY_LSO_PKT_DESC);         \
         /* finished packet with LSO to handle */                            \
        i = 0;                                                               \
        while (i < num_lso_to_read) {                                        \
            /* read packet from nfd_in_issued_lso_ring */                    \
            __mem_ring_get(nfd_in_issued_lso_ring_num,                       \
                               nfd_in_issued_lso_ring_addr,                  \
                               &lso_pkt, sizeof(lso_pkt), sizeof(lso_pkt),   \
                               sig_done, &lso_sig_pair);                     \
            wait_for_all_single(&lso_sig_pair.even);                         \
            NFD_IN_LSO_CNTR_INCR(nfd_in_lso_cntr_addr,                       \
                              NFD_IN_LSO_CNTR_T_NOTIFY_ALL_PKT_FM_LSO_RING); \
            /* fill in pkt_desc values */                                    \
            pkt_desc_tmp.sp0 = 0;                                            \
            pkt_desc_tmp.reserved = start_lso_seq++;                         \
            dst_q |= wq_num_base;                                            \
            pkt_desc_tmp.offset = lso_pkt.offset;                            \
            batch_out.pkt##_pkt##.__raw[0] = pkt_desc_tmp.__raw[0];          \
            batch_out.pkt##_pkt##.__raw[1] = lso_pkt.__raw[1];               \
            batch_out.pkt##_pkt##.__raw[2] = lso_pkt.__raw[2];               \
            batch_out.pkt##_pkt##.__raw[3] = lso_pkt.__raw[3];               \
            /* if it is last LSO being read from ring */                     \
            if (i == (num_lso_to_read - 1)) {                                \
                __mem_workq_add_work(dst_q, wq_raddr, &batch_out.pkt##_pkt,  \
                                     out_msg_sz, out_msg_sz, sig_done,       \
                                     &wq_sig##_pkt);                         \
                NFD_IN_LSO_CNTR_INCR(nfd_in_lso_cntr_addr,                   \
                             NFD_IN_LSO_CNTR_T_NOTIFY_LAST_PKT_FM_LSO_RING); \
            } else {                                                         \
                __mem_workq_add_work(dst_q, wq_raddr, &batch_out.pkt##_pkt,  \
                                     out_msg_sz, out_msg_sz, sig_done,       \
                                     &lso_sig);                              \
                __wait_for_all(&lso_sig);                                    \
            }                                                                \
            NFD_IN_LSO_CNTR_INCR(nfd_in_lso_cntr_addr,                       \
                            NFD_IN_LSO_CNTR_T_NOTIFY_ALL_LSO_PKTS_TO_ME_WQ); \
            i++;                                                             \
            _LSO_END_PKTS_TO_ME_WQ_CNTR(lso_pkt.flags);                      \
        }                                                                    \
    } else {                                                                 \
        NFD_IN_LSO_CNTR_INCR(nfd_in_lso_cntr_addr,                           \
                             NFD_IN_LSO_CNTR_T_NOTIFY_OTHER_PKT_DESC);       \
        wait_msk &= ~__signals(&wq_sig##_pkt);                               \
    }                                                                        \
} while (0)


/**
 * Dequeue a batch of "issue_dma" messages and process that batch, incrementing
 * TX.R for the queue and adding an output message to one of the PCI.IN work
 * queueus.  An output message is only sent for the final message for a packet
 * (EOP bit set).  A count of the total number of descriptors in the batch is
 * added by the "issue_dma" block.
 *
 * We reorder before getting a batch of "issue_dma" messages and then ensure
 * batches are processed in order.  If there is no batch of messages to fetch,
 * we must still participate in the "msg_order_sig" ordering.
 */
__forceinline void
notify()
{

    unsigned int n_batch;
    unsigned int q_batch;
    unsigned int qc_queue;

    unsigned int dst_q;
    unsigned int out_msg_sz = sizeof(struct nfd_in_pkt_desc);

    __xread struct _issued_pkt_batch batch_in;
    struct _pkt_desc_batch batch_tmp;
    struct nfd_in_pkt_desc pkt_desc_tmp;

#ifdef NFD_IN_LSO_CNTR_ENABLE
    /* get the location of LSO statistics */
    if (nfd_in_lso_cntr_addr == 0) {
        nfd_in_lso_cntr_addr = cntr64_get_addr((__mem void *)
                                               nfd_in_lso_cntrs);
    }
#endif
    nfd_in_issued_lso_ring_num = NFD_RING_LINK(PCIE_ISL, nfd_in_issued_lso,
                                               0);
    nfd_in_issued_lso_ring_addr = ((((unsigned long long)
                                     NFD_EMEM_LINK(PCIE_ISL)) >> 32) << 24);

    /* Reorder before potentially issuing a ring get */
    wait_for_all(&get_order_sig);
    reorder_done_opt(&next_ctx, &get_order_sig);


    /* Is there a batch to process
     * XXX assume that issue_dma only inc's dma seq for final dma in batch */
    if (data_dma_seq_compl != data_dma_seq_served)
    {
        /* Process whole batch */
        __critical_path();

        /* Increment data_dma_seq_served before swapping */
        data_dma_seq_served += 1;

        /* XXX THS-50 workaround */
        /* cls_ring_get(NFD_IN_ISSUED_RING_NUM, &batch_in, sizeof batch_in, */
        /*              &msg_sig); */
        ctm_ring_get(NFD_IN_ISSUED_RING_NUM, &batch_in, sizeof batch_in,
                     &msg_sig);

        __asm {
            ctx_arb[--], defer[1];
            local_csr_wr[local_csr_active_ctx_wakeup_events, wait_msk];
        }

        wait_msk = __signals(&wq_sig0, &wq_sig1, &wq_sig2, &wq_sig3,
                             &qc_sig, &msg_sig, &msg_order_sig);
        __implicit_read(&wq_sig0);
        __implicit_read(&wq_sig1);
        __implicit_read(&wq_sig2);
        __implicit_read(&wq_sig3);
        __implicit_read(&qc_sig);
        __implicit_read(&msg_sig);
        __implicit_read(&msg_order_sig);
        __implicit_read(&qc_xfer);

        reorder_done_opt(&next_ctx, &msg_order_sig);

        /* Batches have a least one packet, but n_batch may still be
         * zero, meaning that the queue is down.  In this case, EOP for
         * all the packets should also be zero, so that notify will
         * essentially skip the batch.
         */
        q_batch = batch_in.pkt0.q_num;
        n_batch = batch_in.pkt0.num_batch;

#ifdef NFD_VNIC_DBG_CHKS
        if (n_batch > NFD_IN_MAX_BATCH_SZ) {
            halt();
        }
#endif

        /* Interface and queue info are the same for all packets in batch */
        pkt_desc_tmp.intf = PCIE_ISL;
        pkt_desc_tmp.q_num = q_batch;
#ifndef NFD_IN_ADD_SEQN
        pkt_desc_tmp.reserved = 0;
#endif

        _NOTIFY_PROC(0);
        _NOTIFY_PROC(1);
        _NOTIFY_PROC(2);
        _NOTIFY_PROC(3);

        /* Map batch.queue to a QC queue and increment the TX_R pointer
         * for that queue by n_batch */
        qc_queue = NFD_NATQ2QC(NFD_BMQ2NATQ(q_batch), NFD_IN_TX_QUEUE);
        __qc_add_to_ptr(PCIE_ISL, qc_queue, QC_RPTR, n_batch, &qc_xfer,
                        sig_done, &qc_sig);
    } else {
        /* Participate in msg ordering */
        wait_for_all(&msg_order_sig);
        reorder_done_opt(&next_ctx, &msg_order_sig);
        return;
    }
}


/**
 * Check autopush for seq_compl and reflect seq_served to issue_dma ME
 *
 * "data_dma_seq_compl" tracks the completed gather DMAs.  It is needed by
 * notify to determine when to service the "nfd_in_issued_ring".  The
 * issue_dma ME needs the sequence number more urgently (for in flight
 * DMA tracking) so it constructs the sequence number and reflects the
 * value to this ME.  It must be copied to shared GPRs for worker threads.
 *
 * "data_dma_seq_served" is state owned by this ME.  The issue_dma ME
 * needs the value to determine how many batches can be added to the
 * "nfd_in_issued_ring", so the current value is reflected to that
 * ME.  "data_dma_seq_sent" is used to track which sequence number
 * has been reflected, so that it is not resent.
 */
__intrinsic void
distr_notify()
{
    if (signal_test(&nfd_in_data_compl_refl_sig)) {
        data_dma_seq_compl = nfd_in_data_compl_refl_in;
    }

    /* XXX possibly throttle these reflects further */
    if (data_dma_seq_served != data_dma_seq_sent) {
        __implicit_read(&nfd_in_data_served_refl_out);

        data_dma_seq_sent = data_dma_seq_served;

        nfd_in_data_served_refl_out = data_dma_seq_sent;
        reflect_data(NFD_IN_DATA_DMA_ME,
                     __xfer_reg_number(&nfd_in_data_served_refl_in,
                                       NFD_IN_DATA_DMA_ME),
                     __signal_number(&nfd_in_data_served_refl_sig,
                                     NFD_IN_DATA_DMA_ME),
                     &nfd_in_data_served_refl_out,
                     sizeof nfd_in_data_served_refl_out);
    }
}

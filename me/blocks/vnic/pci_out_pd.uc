#ifndef __PCI_OUT_PD_UC
#define __PCI_OUT_PD_UC

#include <stdmac.uc>
#include <cycle.uc>

#include <wsm.uc>
#include <pci_out_sb.h>
#include <pci_out_sb_iface.uc>
#include <nfd_common.h>

// Send sequence numbers for send_desc
#if (NFD_OUT_MAX_QUEUES < 64)
#error "NFD_OUT_MAX_QUEUES must be >= 64 for nfd_out_send_cntrs optimization to work"
#endif
#define_eval PCIE_ISL_NUM (NFD_PCIE_ISL_BASE + PCIE_ISL)

#define NFD_OUT_CREDITS_BASE            0
.alloc_mem nfd_out_atomics/**/PCIE_ISL \
    i/**/PCIE_ISL_NUM/**/.ctm+NFD_OUT_CREDITS_BASE \
    global (NFD_OUT_MAX_QUEUES * NFD_OUT_ATOMICS_SZ)

/* XXX TODO: Need to get the emem resource from elsewhere! */
.declare_resource BLQ_EMU_RINGS global 8 emem1_queues+4

/* Required user configuration */
#ifndef NFD_OUT_BLM_POOL_START
#error "NFD_OUT_BLM_POOL_START must be defined by the user"
#endif

/* TODO: unify somehow with NFD_OUT_BLM_RADDR */
#ifndef NFD_OUT_BLM_RADDR_UC
#error "NFD_OUT_BLM_RADDR_UC must be defined by the user"
#endif

.alloc_resource NFD_OUT_BLM_POOL_START BLQ_EMU_RINGS global 1

// This QID must be a multiple of four for our optimization of ORing in
// the BLS to work.
.assert((NFD_OUT_BLM_POOL_START & 0x3) == 0)


// See NFP Databook Section 9.2.2.1.2.10 "TicketRelease Command"
#define TICKET_ERROR                    255


/* REMOVE ME */
#define ONE_PKT_AT_A_TIME 1

/* REMOVE ME */
/* These are in here only because nfd_internal.h is not microcode safe */
#define NFD_OUT_DATA_CFG_REG            6
#define NFD_OUT_DATA_CFG_REG_SIG_ONLY   7
#define NFD_OUT_DATA_DMA_TOKEN          2
#define NFD_OUT_ATOMICS_DMA_DONE        8

/*
 * DMA Descriptor
 *
 * Bit    3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1 0 0 0 0 0 0 0 0 0 0
 * -----\ 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 * Word  +---------------------------------------------------------------+
 *    0  |                        CPP Address Low                        |
 *       +---+-------------------------------+---+-------+---------------+
 *    1  |Sel|          DMA Mode             |Tok|DMACFGI|  CPP Addr Hi  |
 *       +---+-------------------------------+---+-------+---------------+
 *    2  |                       PCIE Address Low                        |
 *       +-----------------------+---------------+-+-----+---------------+
 *    3  |   Transfer Length     | Requester ID  |O| XC  | PCIe Addr Hi  |
 *       +-----------------------+---------------+-+-----+---------------+
 *
 * DMA Descriptor: signal only
 *
 * Bit    3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1 0 0 0 0 0 0 0 0 0 0
 * -----\ 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 * Word  +---------------------------------------------------------------+
 *    0  |                        CPP Address Low                        |
 *       +-+-------+-----------+-----+-------+---+-------+---------------+
 *    1  |0| MstID |Master Isl | Ctx |Signum |Tok|DMACFGI|  CPP Addr Hi  |
 *       +-+-------+-----------+-----+-------+---+-------+---------------+
 *    2  |                       PCIE Address Low                        |
 *       +-----------------------+---------------+-+-----+---------------+
 *    3  |   Transfer Length     | Requester ID  |O| XC  | PCIe Addr Hi  |
 *       +-----------------------+---------------+-+-----+---------------+
 */

// Word 0
#define PCIE_DMA_CPPA_LO_bf     0, 31, 0
#define PCIE_DMA_CPPA_LO_wrd    0
#define PCIE_DMA_CPPA_LO_shf    0
#define PCIE_DMA_CPPA_LO_msk    0xFFFFFFFF

// Word 1
#define PCIE_DMA_MODE_SEL_bf    1, 31, 30
#define PCIE_DMA_MODE_SEL_wrd   1
#define PCIE_DMA_MODE_SEL_shf   30
#define PCIE_DMA_MODE_SEL_msk   0x3
#define PCIE_DMA_MASTER_ID_bf   1, 30, 27
#define PCIE_DMA_MASTER_ID_wrd  1
#define PCIE_DMA_MASTER_ID_shf  27
#define PCIE_DMA_MASTER_ID_msk  0xF
#define PCIE_DMA_MASTER_ISL_bf  1, 26, 21
#define PCIE_DMA_MASTER_ISL_wrd 1
#define PCIE_DMA_MASTER_ISL_shf 21
#define PCIE_DMA_MASTER_ISL_msk 0x3F
#define PCIE_DMA_SIGCTX_bf      1, 20, 18
#define PCIE_DMA_SIGCTX_wrd     1
#define PCIE_DMA_SIGCTX_shf     18
#define PCIE_DMA_SIGCTX_msk     0x3
#define PCIE_DMA_SIGNUM_bf      1, 17, 14
#define PCIE_DMA_SIGNUM_wrd     1
#define PCIE_DMA_SIGNUM_shf     14
#define PCIE_DMA_SIGNUM_msk     0xF
#define PCIE_DMA_TOKEN_bf       1, 13, 12
#define PCIE_DMA_TOKEN_wrd      1
#define PCIE_DMA_TOKEN_shf      12
#define PCIE_DMA_TOKEN_msk      0x3
#define PCIE_DMA_CFG_IDX_bf     1, 11, 8
#define PCIE_DMA_CFG_IDX_wrd    1
#define PCIE_DMA_CFG_IDX_shf    8
#define PCIE_DMA_CFG_IDX_msk    0xF
#define PCIE_DMA_CPPA_HI_bf     1, 7, 0
#define PCIE_DMA_CPPA_HI_wrd    1
#define PCIE_DMA_CPPA_HI_shf    0
#define PCIE_DMA_CPPA_HI_msk    0xFF

// Word 2
#define PCIE_DMA_PCIEA_LO_bf    2, 31, 0
#define PCIE_DMA_PCIEA_LO_wrd   2
#define PCIE_DMA_PCIEA_LO_shf   0
#define PCIE_DMA_PCIEA_LO_msk   0xFFFFFFFF

// Word 3
#define PCIE_DMA_XLEN_bf        3, 31, 20
#define PCIE_DMA_XLEN_wrd       3
#define PCIE_DMA_XLEN_shf       20
#define PCIE_DMA_XLEN_msk       0xFFF
#define PCIE_DMA_RID_bf         3, 19, 12
#define PCIE_DMA_RID_wrd        3
#define PCIE_DMA_RID_shf        12
#define PCIE_DMA_RID_msk        0xFF
#define PCIE_DMA_OVDRID_bf      3, 11, 11
#define PCIE_DMA_OVDRID_wrd     3
#define PCIE_DMA_OVDRID_shf     11
#define PCIE_DMA_OVDRID_msk     0x1
#define PCIE_DMA_XCLASS_bf      3, 10, 8
#define PCIE_DMA_XCLASS_wrd     3
#define PCIE_DMA_XCLASS_shf     8
#define PCIE_DMA_XCLASS_msk     0x7
#define PCIE_DMA_PCIEA_HI_bf    3, 7, 0
#define PCIE_DMA_PCIEA_HI_wrd   3
#define PCIE_DMA_PCIEA_HI_shf   0
#define PCIE_DMA_PCIEA_HI_msk   0xFF

#define PCIE_DMA_SIZE_LW        4

#define NFP_PCIE_DMA_TOPCI_LO   0x40040

#macro wait_br_next_state(in_sig0, in_sig1, LABEL)
    wait_br_next_state(in_sig0, in_sig1, LABEL, --)
#endm


/**
 * Wait one or two signals to arrive and on receiving them branch to
 * a given location.  If one signal is specified, the wait is
 * unconditional and consumes the signal.  If two signals are specified,
 * then we wait for either one to arrive but without clearing either.
 *
 * @param in_sig0       The first signal to wait on (must be real)
 * @param in_sig1       The second signal to wait on (may be '--' if none)
 * @param LABEL         Label to branch to on signal reception.
 * @param OPT_TOK       An optional token to be added to the command
 *                      (e.g. defer[1]).  If '--' no such token.
 */
#macro wait_br_next_state(in_sig0, in_sig1, LABEL, OPT_TOK)

    .set_sig in_sig0

    #if (streq('in_sig1', '--'))
        #if (!streq('OPT_TOK', '--'))
            ctx_arb[in_sig0], br[LABEL], OPT_TOK
        #else
            ctx_arb[in_sig0], br[LABEL]
        #endif
    #else
        .set_sig in_sig1
        #if (!streq('OPT_TOK', '--'))
            ctx_arb[in_sig0, in_sig1], ANY, br[LABEL], OPT_TOK
        #else
            ctx_arb[in_sig0, in_sig1], ANY, br[LABEL]
        #endif
    #endif
#endm


#macro signal_sb_credits_update()
.begin
    .reg addr

    #define_eval SB_SIG_ADDR (((NFD_OUT_STAGE_ME & 0x3F0)  << 20) | \
                             ((NFD_OUT_STAGE_ME & 0xF)     << 9)  | \
                             (STAGE_BATCH_MANAGER_CTX      << 6)  | \
                             (PCI_OUT_SB_WQ_CREDIT_SIG_NUM << 2))
    move(addr, SB_SIG_ADDR)

    ct[interthread_signal, --, 0, addr]

    #undef SB_SIG_ADDR

.end
#endm


/**
 * Issue the DMAs required to send a packet to a host buffer.  The parameters
 * for transmission are specified in 'in_work'.  The macro is given two
 * sets of transfer registers (out_dma[01]) and two signals (out_sig[01]) to
 * use for this.  It must consume no more than two DMAs in the low priority
 * to-PCIe DMA queue.  The last DMA must be completed with signal out_sig0
 * and must not be waited on within this macro.  After issuing the last
 * DMA, the macro must invoke wait_br_next_state() to wait for the next
 * signals to arrive for state transition (in_wait_sig0 and maybe
 * in_wait_sig1), branching to LABEL on completion.  This scheme allows
 * filling defer slots on these state transitions with instructions required
 * to issue DMAs.
 *
 * @param in_work       Read-only work queue entry from stage batch used
 *                      to tell how and where to issue the DMAs.
 *                      (see: pci_out_sb.h)
 * @param out_dma0      First block of write transfer registers to use for
 *                      DMAs.
 * @param out_sig0      First signal to use for DMAs
 * @param out_dma1      Second block of write transfer registers to use for
 *                      DMAs.
 * @param out_sig1      Second signal to use for DMAs.  Must be used for
 *                      the final DMA and not waited on.
 * @param LABEL         LABEL to branch to after getting state transition
 *                      signal(s).
 * @param in_wait_sig0  First state transition signal.  Must be specified.
 * @param in_wait_sig1  Second state transition signal.  Optional.  '--' if
 *                      not used.
 */
#macro _issue_packet_dma(in_work, out_dma0, out_sig0, out_dma1, out_sig1, \
                         LABEL, in_wait_sig0, in_wait_sig1)
.begin

    .reg word
    .reg tmp
    .reg tmp2
    .reg isl
    .reg ovfl
    .reg len
    .reg addr_lo

    .io_completed in_work[0]
    .io_completed in_work[1]
    .io_completed in_work[2]
    .io_completed in_work[3]
    .io_completed in_work[4]
    .io_completed out_dma0[0]
    .io_completed out_dma0[1]
    .io_completed out_dma0[2]
    .io_completed out_dma0[3]
    .io_completed out_dma1[0]
    .io_completed out_dma1[1]
    .io_completed out_dma1[2]
    .io_completed out_dma1[3]

    alu[@ndequeued, @ndequeued, -, 1]
    blt[add_wq_credits#]

start_packet_dma#:
    wsm_test_bit_clr(in_work, SB_WQ_ENABLED, no_dma#)
    wsm_test_bit_clr(in_work, SB_WQ_CTM_ONLY, not_ctm_only#)

    // Super fast path
ctm_only#:
    // Word 0
    // XXX Optimization: packet number is already in place
    alu[word, in_work[2], AND, g_dma_word0_mask]
    alu[out_dma0[0], word, OR, 1, <<31] // Packet format bit

    // Word 1
    wsm_extract(word, in_work, SB_WQ_CTM_ISL)
    alu[word, word, OR, g_dma_word1_vals]
    alu[out_dma0[1], word, OR, (&out_sig0), <<PCIE_DMA_SIGNUM_shf]

    // Word 2
    wsm_extract(tmp2, in_work, SB_WQ_METALEN)
    alu[tmp, NFP_NET_RX_OFFSET, -, tmp2]
    alu[out_dma0[2], tmp, +, in_work[SB_WQ_HOST_ADDR_LO_wrd]]

    // Word 3
    alu[word, g_dma_word3_vals, +8, in_work[SB_WQ_HOST_ADDR_HI_wrd]], no_cc
    // XXX Uncomment if buffers can cross a 4G boundary
    // alu[word, word, +carry, 0]
    wsm_extract(tmp, in_work,  SB_WQ_RID)
    sm_set_noclr(word, PCIE_DMA_RID, tmp)
    wsm_extract(len, in_work, SB_WQ_DATALEN)

    // Gambling on being fast enough to beat the DMA pull from these registers
    #pragma warning(disable:5117)
    #pragma warning(disable:4701)
    #pragma warning(disable:5009)

    pcie[write_pci, out_dma0[0], g_pcie_addr_hi, <<8, g_pcie_addr_lo, 4]

    #pragma warning(default:5117)
    // This wait() always has 2 defer slots following it
    wait_br_next_state(in_wait_sig0, in_wait_sig1, LABEL, defer[2])
    alu[len, len, -, 1]
    sm_set_noclr_to(out_dma0[3], word, PCIE_DMA_XLEN, len, 1)
    #pragma warning(default:4701)
    #pragma warning(default:5009)

not_ctm_only#:
    ctx_arb[bpt] // XXX REMOVE ME
    wsm_extract(isl, in_work, SB_WQ_CTM_ISL)
    beq[mu_only_dma#]

ctm_and_mu_dma#:
    // TODO: implement and end with wait_br_next_state()
    // wait_br_next_state(in_wait_sig0, in_wait_sig1, LABEL)

mu_only_dma#:
    // TODO: implement and end with wait_br_next_state()
    // wait_br_next_state(in_wait_sig0, in_wait_sig1, LABEL)

no_dma#:
    ctx_arb[bpt] // XXX REMOVE ME
    // signal self w/ output signal:  "DMA" is completed
    local_csr_rd[ACTIVE_CTX_STS]
    immed[tmp, 0]
    alu[tmp, tmp, AND, 7]
    alu[tmp, tmp, OR, (&out_sig0), <<3]
    local_csr_wr[SAME_ME_SIGNAL, tmp]
    wait_br_next_state(in_wait_sig0, in_wait_sig1, LABEL)

add_wq_credits#:
    move(addr_lo, nfd_out_sb_wq_credits/**/PCIE_ISL)
    alu[--, g_add_imm_iref, OR, SB_WQ_CREDIT_BATCH, <<16]
    mem[add_imm, --, 0, addr_lo], indirect_ref

    signal_sb_credits_update()

    br[start_packet_dma#], defer[1]
    move(@ndequeued, (SB_WQ_CREDIT_BATCH - 1))

.end
#endm


/*
 * Request more work from the ingress work queue
 */
#macro request_work(in_xfer, in_sig)

    #if SB_USE_MU_WORK_QUEUES

        mem[qadd_thread, in_xfer[0], g_in_wq_hi, <<8, g_in_wq_lo, SB_WQ_SIZE_LW],
            sig_done[in_sig]

    #else /* SB_USE_MU_WORK_QUEUES */

        cls[ring_workq_add_thread, in_xfer[0], g_in_wq_hi, <<8, g_in_wq_lo, SB_WQ_SIZE_LW],
            sig_done[in_sig]

    #endif /* SB_USE_MU_WORK_QUEUES */

#endm


/**
 * Process the completion of the last DMA for a packet.  This entails:
 *  * freeing the packet's CTM and MU buffers
 *  * issuing a mem[release_ticket] with the packet sequence number
 *    on the queue's ticket release bitmap.
 *  * atomically adding the return value from mem[release_ticket] to
 *    the send_desc ME's per queue sequence numbers (if non-zero)
 * Next, the macro issues a new request for work using the transfer
 * registers and signal that held the previous work request.  Finally,
 * this macro waits on one or two state transition signals and, on
 * reception, branches to a specified label to start processing for
 * the next state.
 *
 * @param io_work       Read transfer registers containing the original
 *                      parameters for DMAing the packet to the host.
 *                      After "completing" the DMA, this macro will
 *                      ask for more work from the work queue using
 *                      these transfer registers.
 * @param io_wq_sig     Signal used to indicate completion of the original
 *                      parameters for DMAing the packet to the host.
 *                      After "completing" the DMA, this macro will
 *                      ask for more work from the work queue using
 *                      this signal.
 * @param LABEL         Label to branch to after receiving state
 *                      transition signals.
 * @param in_wait_sig0  First state transition signal.  Must be specified.
 * @param in_wait_sig0  Second state transition signal.  Can be '--'
 *                      indicating no signal.
 */
#macro _complete_packet_dma(io_work, in_wq_sig, LABEL, in_wait_sig0, \
                            in_wait_sig1)
.begin

    .reg isl
    .reg qnum
    .reg addr_hi
    .reg addr_lo
    .reg ring_num
    .reg bitmap_lo
    .reg cntr_addr_lo

    .reg $ticket
    .sig ticket_sig

    wsm_extract(qnum, io_work, SB_WQ_QNUM)
    alu[bitmap_lo, g_bitmap_base, OR, qnum, <<4]
    wsm_extract($ticket, io_work, SB_WQ_SEQ)

    wsm_extract(isl, io_work, SB_WQ_CTM_ISL)
    beq[no_ctm_buffer#]

    // Free CTM buffer
    alu[addr_hi, isl, OR, 0x80]
    alu[addr_hi, --, B, addr_hi, <<24]
    wsm_extract(addr_lo, io_work, SB_WQ_PKT_NUM)
    mem[packet_free, --, addr_hi, <<8, addr_lo]

no_ctm_buffer#:
    mem[release_ticket, $ticket, 0, bitmap_lo, 1], sig_done[ticket_sig]

    wsm_extract(addr_lo, io_work, SB_WQ_MUBUF)

    request_work(io_work, in_wq_sig)

    // Cheat and pull from the read xfers before they get clobbered
    #pragma warning(disable:5009)
    ctx_arb[ticket_sig], defer[2]
    alu[cntr_addr_lo, --, B, qnum, <<4]
    wsm_extract(ring_num, io_work, SB_WQ_BLS)
    #pragma warning(default:5009)

    // Free the MU buffer
    alu[-- , g_blm_iref, OR, ring_num, <<16]
    mem[fast_journal, --, g_blm_addr_hi, <<8, addr_lo], indirect_ref

ticket_ready#:
    br=byte[$ticket, 0, 0, complete_done#]
    br=byte[$ticket, 0, TICKET_ERROR, ticket_error#]

    alu[cntr_addr_lo, cntr_addr_lo, +, NFD_OUT_ATOMICS_DMA_DONE]
    alu[--, g_add_imm_iref, OR, $ticket, <<16]
    mem[add_imm, --, g_send_cntrs_addr_hi, <<8, cntr_addr_lo], indirect_ref

complete_done#:
    wait_br_next_state(in_wait_sig0, in_wait_sig1, LABEL)

ticket_error#:
    cycle32_sleep(250)
    mem[release_ticket, $ticket, 0, bitmap_lo, 1], sig_done[ticket_sig]
    ctx_arb[ticket_sig], br[ticket_ready#]

.end
#endm


#macro issue_packet_dma(XNUM, LABEL, wsig0, wsig1)

    .io_completed work_sig/**/XNUM
    .io_completed dma_sig/**/XNUM
    .io_completed dma_sig/**/XNUM/**/x

    _issue_packet_dma($work_in/**/XNUM,
                      $dma_out/**/XNUM,
                      dma_sig/**/XNUM,
                      $dma_out/**/XNUM/**/x,
                      dma_sig/**/XNUM/**/x,
                      LABEL,
                      wsig0,
                      wsig1)

#endm


#macro issue_packet_dma(XNUM, LABEL, wsig0)
    issue_packet_dma(XNUM, LABEL, wsig0, --)
#endm


#macro complete_packet_dma(XNUM, LABEL, wsig0, wsig1)

    _complete_packet_dma($work_in/**/XNUM,
                         work_sig/**/XNUM,
                        LABEL,
                        wsig0,
                        wsig1)

#endm


#macro complete_packet_dma(XNUM, LABEL, wsig0)
    complete_packet_dma(XNUM, LABEL, wsig0, --)
#endm


#macro die_if_debug()

    #if 1 /* XXX REMOVE ME when really integrated */

        // Fake out the assembler
        alu[--, --, B, 0]
        beq[die_now#]
        bne[die_now#]
        br[keep_going#]

    die_now#:
        ctx_arb[kill]

    keep_going#:
        // not reached
    #endif

#endm


main#:

    /*
     * DECLARATIONS
     */
    pci_out_sb_declare()

    // Global per-context constants
    .reg volatile g_dma_word0_mask
    .reg volatile g_dma_word1_vals
    .reg volatile g_dma_word3_vals
    .reg volatile g_bitmap_base
    .reg volatile g_send_cntrs_addr_hi
    .reg volatile g_blm_addr_hi
    .reg volatile g_blm_iref
    .reg volatile g_add_imm_iref
    .reg volatile g_in_wq_hi
    .reg volatile g_in_wq_lo
    .reg volatile g_pcie_addr_lo
    .reg volatile g_pcie_addr_hi

    .reg @ndequeued
    .init @ndequeued SB_WQ_CREDIT_BATCH

    // XFER and signals for work/DMA block 0
    .reg read $work_in0[SB_WQ_SIZE_LW]
    .xfer_order $work_in0
    .sig volatile work_sig0
    .reg write $dma_out0[PCIE_DMA_SIZE_LW]
    .xfer_order $dma_out0
    .sig volatile dma_sig0
    .reg write $dma_out0x[PCIE_DMA_SIZE_LW]
    .xfer_order $dma_out0x
    .sig volatile dma_sig0x

    // XFER and signals for work/DMA block 1
    .reg $work_in1[SB_WQ_SIZE_LW]
    .xfer_order $work_in1
    .sig volatile work_sig1
    .reg $dma_out1[PCIE_DMA_SIZE_LW]
    .xfer_order $dma_out1
    .sig volatile dma_sig1
    .reg $dma_out1x[PCIE_DMA_SIZE_LW]
    .xfer_order $dma_out1x
    .sig volatile dma_sig1x

    // XFER and signals for work/DMA block 2
    .reg $work_in2[SB_WQ_SIZE_LW]
    .xfer_order $work_in2
    .sig volatile work_sig2
    .reg $dma_out2[PCIE_DMA_SIZE_LW]
    .xfer_order $dma_out2
    .sig volatile dma_sig2
    .reg $dma_out2x[PCIE_DMA_SIZE_LW]
    .xfer_order $dma_out2x
    .sig volatile dma_sig2x

    // General GPRs for initialization
    .reg tmp


    /*
     * PER CONTEXT INITIALIZATION
     */

    /*
     * Maps SB_WQ_PKT_NUM and SB_WQ_OFFSET into CPP address low bits.
     * These fields are at the right place so we only need to mask out
     * certain bits and OR in bit 31 to get a packet-addressing mode
     * address.
     */
    move(g_dma_word0_mask, 0x01FF1FFF)

    /*
     * Values to OR into word 1 of a DMA descriptor
     */
    /* Valid only for CTM addresses! */
    move(g_dma_word1_vals, 0x80)
    sm_set_noclr(g_dma_word1_vals, PCIE_DMA_CFG_IDX, NFD_OUT_DATA_CFG_REG)
    sm_set_noclr(g_dma_word1_vals,  PCIE_DMA_TOKEN, NFD_OUT_DATA_DMA_TOKEN)
    local_csr_rd[ACTIVE_CTX_STS]
    immed[tmp, 0]
    alu[tmp, tmp, AND, 7]
    sm_set_noclr(g_dma_word1_vals,  PCIE_DMA_SIGCTX, tmp)
    move(tmp, __ISLAND)
    sm_set_noclr(g_dma_word1_vals,  PCIE_DMA_MASTER_ISL, tmp)
    move(tmp, (__MEID & 0xf))
    sm_set_noclr(g_dma_word1_vals,  PCIE_DMA_MASTER_ID, tmp)

    /*
     * Fixed bits to OR into word 3 of a DMA descriptor
     */
    move(g_dma_word3_vals, ((0 << PCIE_DMA_XCLASS_shf) |
                            (1 << PCIE_DMA_OVDRID_shf)))

    // Base address in local island of release bitmap
    move(g_bitmap_base, nfd_out_sb_release/**/PCIE_ISL)

    // Base address of send_desc release counters
    move(g_send_cntrs_addr_hi, (nfd_out_atomics/**/PCIE_ISL >> 8))

    // (1 << 3) == OVE_DATA == 1 -> override full dataref with iref[31:16]
    move(g_blm_iref, (1 << 3))
    move(tmp, NFD_OUT_BLM_POOL_START)
    alu[g_blm_iref, g_blm_iref, OR, tmp, <<16]

    // XXX BLM really needs an interface to speed this up for
    // cycle tight access.
    move(g_blm_addr_hi, ((NFD_OUT_BLM_RADDR_UC >> 8) & 0xFF000000))

    /*
     * Indirect reference to perform 32-bit add_imm from
     * the data16 field of an indirect reference.
     *
     * override length  = (1 << 7)
     * override dataref = (2 << 3)
     * length[2] = 0 for 32-bit operations = (0 << 10)
     * length[3] = 1 for to pull operand from dataref = (1 << 11)
     */
    move(g_add_imm_iref, ((2 << 3) | (1 << 7) | (0 << 10) | (1 << 11)))


    #if SB_USE_MU_WORK_QUEUES

        move(g_in_wq_hi, ((nfd_out_sb_ring_mem/**/PCIE_ISL >> 8) & 0xFF000000))
        move(g_in_wq_lo, nfd_out_sb_ring_num/**/PCIE_ISL)

    #else /* SB_USE_MU_WORK_QUEUES */

        move(g_in_wq_hi, 0)
        move(g_in_wq_lo, ((nfd_out_sb_ring_num/**/PCIE_ISL) << 2))

    #endif /* SB_USE_MU_WORK_QUEUES */

    move(g_pcie_addr_lo, NFP_PCIE_DMA_TOPCI_LO)
    move(g_pcie_addr_hi, (PCIE_ISL << 30))


    /*
     * STATE MACHINE
     *
     * (main loop)
     */

    die_if_debug()

#ifdef ONE_PKT_AT_A_TIME

    kickstart#:
        request_work($work_in0, work_sig0)
        ctx_arb[work_sig0]

    one_packet_issue_dma#:
        issue_packet_dma(0, one_packet_complete_dma#, dma_sig0)
    one_packet_complete_dma#:
        complete_packet_dma(0, one_packet_issue_dma#, work_sig0)

#else /* ONE_PACKET_AT_A_TIME */

    kickstart#:
        request_work($work_in0, work_sig0)
        request_work($work_in1, work_sig1)
        request_work($work_in2, work_sig2)
        ctx_arb[work_sig0], br[state_e0n0#]

    /* ------------------------------ COLUMN #1 ------------------------------ */

    state_e0n0#:
        // No demux: fall through

    transition_e0n0_e1n1#:
        issue_packet_dma(0, state_e1n1#, work_sig1, dma_sig0)

    state_e1n1#:
        br_signal[dma_sig0, transition_e1n1_e0n1#]
        br_!signal[work_sig1, unreached#]
        // fall through

    transition_e1n1_e2n2#:
        issue_packet_dma(1, state_e2n2#, work_sig2, dma_sig0)

    state_e2n2#:
        br_signal[dma_sig0, transition_e2n2_e1n2#]
        br_!signal[work_sig2, unreached#]
        // fall through

    transition_e2n2_e3n0#:
        issue_packet_dma(2, state_e3n0#, dma_sig0)

    state_e3n0#:
        // No demux: fall through

    transition_e3n0_e2n0#:
        complete_packet_dma(0, state_e2n0#, work_sig0, dma_sig1)

    transition_e1n1_e0n1#:
        complete_packet_dma(0, state_e0n1#, work_sig1)

    transition_e2n2_e1n2#:
        complete_packet_dma(0, state_e1n2#, work_sig2, dma_sig1)


    /* ------------------------------ COLUMN #2 ------------------------------ */

    state_e0n1#:
        // No demux: fall through

    transition_e0n1_e1n2#:
        issue_packet_dma(1, state_e1n2#, work_sig2, dma_sig1)

    state_e1n2#:
        br_signal[dma_sig1, transition_e1n2_e0n2#]
        br_!signal[work_sig1, unreached#]
        // fall through

    transition_e1n2_e2n0#:
        issue_packet_dma(2, state_e2n0#, work_sig0, dma_sig1)

    state_e2n0#:
        br_signal[dma_sig1, transition_e2n0_e1n0#]
        br_!signal[work_sig1, unreached#]
        // fall through

    transition_e2n0_e3n1#:
        issue_packet_dma(0, state_e3n1#, dma_sig1)

    state_e3n1#:
        // No demux: fall through

    transition_e3n1_e2n1#:
        complete_packet_dma(1, state_e2n1#, work_sig1, dma_sig2)

    transition_e1n2_e0n2#:
        complete_packet_dma(1, state_e0n2#, work_sig2)

    transition_e2n0_e1n0#:
        complete_packet_dma(1, state_e1n0#, work_sig0, dma_sig2)


    /* ------------------------------ COLUMN #3 ------------------------------ */

    state_e0n2#:
        // No demux: fall through

    transition_e0n2_e1n0#:
        issue_packet_dma(2, state_e1n0#, work_sig0, dma_sig2)

    state_e1n0#:
        br_signal[dma_sig2, transition_e1n0_e0n0#]
        br_!signal[work_sig1, unreached#]
        // fall through

    transition_e1n0_e2n1#:
        issue_packet_dma(0, state_e2n1#, work_sig1, dma_sig2)

    state_e2n1#:
        br_signal[dma_sig2, transition_e2n1_e1n1#]
        br_!signal[work_sig1, unreached#]
        // fall through

    transition_e2n1_e3n2#:
        issue_packet_dma(1, state_e3n2#, dma_sig2)

    state_e3n2#:
        // No demux: fall through

    transition_e3n2_e2n2#:
        complete_packet_dma(2, state_e2n2#, work_sig2, dma_sig0)

    transition_e1n0_e0n0#:
        complete_packet_dma(2, state_e0n0#, work_sig0)

    transition_e2n1_e1n1#:
        complete_packet_dma(2, state_e1n1#, work_sig1, dma_sig0)

    unreached#:
        ctx_arb[bpt], br[unreached#]

#endif /* ONE_PACKET_AT_A_TIME */


#endif /* __PCI_OUT_PD_UC */

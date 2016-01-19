/*
 * Copyright (C) 2014-2016,  Netronome Systems, Inc.  All rights reserved.
 *
 * @file          blocks/vnic/shared/nfd_flr.c
 * @brief         An internal API to perform NFD FLR resets
 */

#include <assert.h>
#include <nfp.h>

/* XXX nfp/xpb.h doesn't support a sig_done form of the commands */
#include <nfp/xpb.h>
#include <nfp/me.h>
#include <nfp/mem_atomic.h>
#include <nfp/mem_bulk.h>

#include <std/reg_utils.h>

#include <vnic/nfd_common.h>
#include <vnic/shared/nfd.h>
#include <vnic/shared/nfd_cfg.h>
#include <vnic/shared/nfd_internal.h>


#include <nfp_net_ctrl.h>


#ifndef NFD_OUT_RX_OFFSET
#warning "NFD_OUT_RX_OFFSET not defined: defaulting to NFP_NET_RX_OFFSET which is sub-optimal"
#define NFD_OUT_RX_OFFSET NFP_NET_RX_OFFSET
#endif /* NFD_OUT_RX_OFFSET */

/*
 * NFD FLR handling consists of 3 main components: a part that notices new
 * FLRs by receiving events and examining HW CSRs, a part that issues
 * reconfiguration messages for each pending FLR in turn, using the regular
 * NFD configuration message passing mechanisms, and a part that notices
 * the FLR configuration messages and acknowledges them to the HW.
 *
 * The HW will record an FLR as "in progress" from the time it was issued
 * until it is finally acked by the FW.  We only Ack the FLR once all NFD
 * owned MEs have had a chance to reconfigure based on the FLR, so we track
 * the FLRs that we have "seen" using atomics.  The first stage sets the
 * atomic for each new FLR that it notices, and the last stage clears the
 * atomic after it has acked the FLR.  Clearing the atomic only after the
 * ack ensures that we do not reissue the reconfiguration for the same FLR.
 */


/* Defines to control how much data we should write with bulk methods */
/* XXX confirm whether or not the FLR reset should write MSIX tables to zero.
 * XXX nfp_net_ctrl.h doesn't seem to have suitable defines to use currently. */
#define NFD_FLR_CLR_START   0x50
#define NFD_FLR_CLR_SZ      (SZ_8K - 0x50)


/* Functions called from the service ME */

/** Clear the bulk of the CFG BAR
 * @param addr      start address of the vNIC CFG BAR
 *
 * This function performs the bulk write of data that won't be reset
 * by other functions (e.g. nfd_flr_init__pf_ctrl_bar() and
 * nfd_flr_init_vf_ctrl_bar()).  "addr" should be obtained via the
 * appropriate API, e.g. NFD_CFG_BAR_ISL.
 */
__intrinsic void
nfd_flr_clr_bar(__emem char *addr)
{
    __xwrite unsigned int zero[16];
    unsigned int copied_bytes;

    ctassert(__is_aligned(NFD_FLR_CLR_SZ, 8));
    ctassert(sizeof zero > NFP_NET_CFG_VERSION - NFP_NET_CFG_TXRS_ENABLE);
    ctassert(__is_log2(sizeof zero));

    reg_zero(zero, sizeof zero);

    /* Clear the data below the RO fields */
    mem_write64(zero, addr + NFP_NET_CFG_TXRS_ENABLE,
                NFP_NET_CFG_VERSION - NFP_NET_CFG_TXRS_ENABLE);

    addr += NFD_FLR_CLR_START;

    /* Clear the data after the RO fields and up to
     * a convenient alignment */
    copied_bytes = NFD_FLR_CLR_SZ & (sizeof zero - 1);
    mem_write64(zero, addr, copied_bytes);
    addr += copied_bytes;

    /* Clear the balance of the data */
    for (; copied_bytes < NFD_FLR_CLR_SZ; copied_bytes += sizeof zero,
             addr += sizeof zero) {
        mem_write64(zero, addr, sizeof zero);
    }
}


/** Init the non-zero parts of the PF control BAR
 * @param isl_base      start address of the CFG BARs for the PCIe island
 *
 * "isl_base" should be obtained via the appropriate API,
 * e.g. NFD_CFG_BASE_LINK.
 */
void
nfd_flr_init_pf_ctrl_bar(__emem char *isl_base)
{
#if (NFD_MAX_PF_QUEUES != 0)
    unsigned int q_base = NFD_MAX_VF_QUEUES * NFD_MAX_VFS;
    __xwrite unsigned int cfg[] = {NFD_CFG_VERSION, 0, NFD_CFG_PF_CAP,
                                   NFD_MAX_PF_QUEUES, NFD_MAX_PF_QUEUES,
                                   NFD_CFG_MAX_MTU,
                                   NFD_NATQ2QC(q_base, NFD_IN_TX_QUEUE),
                                   NFD_NATQ2QC(q_base, NFD_OUT_FL_QUEUE)};
    __xwrite unsigned int exn_lsc = 0xffffffff;
    __xwrite unsigned int rx_off = NFD_OUT_RX_OFFSET;

    mem_write64(&cfg,
                NFD_CFG_BAR(isl_base, NFD_MAX_VFS) + NFP_NET_CFG_VERSION,
                sizeof cfg);

    mem_write8(&exn_lsc, NFD_CFG_BAR(isl_base, NFD_MAX_VFS) + NFP_NET_CFG_LSC,
               sizeof exn_lsc);

    mem_write8(&rx_off,
               NFD_CFG_BAR(isl_base, NFD_MAX_VFS) + NFP_NET_CFG_RX_OFFSET,
               sizeof rx_off);
#endif
}


/** Init the non-zero parts of the VF control BAR
 * @param isl_base      start address of the CFG BARs for the PCIe island
 * @param vf            VF number on the PCIe island
 *
 * "isl_base" should be obtained via the appropriate API,
 * e.g. NFD_CFG_BASE_LINK.
 */
void
nfd_flr_init_vf_ctrl_bar(__emem char *isl_base, unsigned int vf)
{
#if ((NFD_MAX_VFS != 0) && (NFD_MAX_VF_QUEUES != 0))
#ifdef NFD_NO_ISOLATION
    unsigned int q_base = NFD_MAX_VF_QUEUES * vnic;
#else
    unsigned int q_base = 0;
#endif
    __xwrite unsigned int cfg[] = {NFD_CFG_VERSION, 0, NFD_CFG_VF_CAP,
                                   NFD_MAX_VF_QUEUES, NFD_MAX_VF_QUEUES,
                                   NFD_CFG_MAX_MTU,
                                   NFD_NATQ2QC(q_base, NFD_IN_TX_QUEUE),
                                   NFD_NATQ2QC(q_base, NFD_OUT_FL_QUEUE)};
    __xwrite unsigned int exn_lsc = 0xffffffff;
    __xwrite unsigned int rx_off = NFD_OUT_RX_OFFSET;

    mem_write64(&cfg, NFD_CFG_BAR(isl_base, vf) + NFP_NET_CFG_VERSION,
                sizeof cfg);

    mem_write8(&exn_lsc, NFD_CFG_BAR(isl_base, vf) + NFP_NET_CFG_LSC,
               sizeof exn_lsc);

    mem_write8(&rx_off, NFD_CFG_BAR(isl_base, vf) + NFP_NET_CFG_RX_OFFSET,
               sizeof rx_off);
#endif
}


/* Functions called from PCI.IN ME0 */

/** Read the HW FLR and nfd_flr_atomic state
 * @param pcie_isl              PCIe island (0..3)
 * @param flr_pend_status    Internal state for FLR processing
 *
 * See nfd_cfg_internal.c for a description of the format of
 * "flr_pend_status".
 */
__intrinsic void
nfd_flr_check_pf(unsigned int pcie_isl,
                 __shared __gpr unsigned int *flr_pend_status)
{
    __xread unsigned int seen_flr;
    __mem char *atomic_addr;
    SIGNAL atomic_sig;
    __xread unsigned int cntrlr3;
    unsigned int xpb_addr;
    SIGNAL xpb_sig;
    unsigned int pf_atomic_data = 1 << NFD_FLR_PF_shf;

    /* Read state of FLR hardware and seen atomics */
    atomic_addr = (NFD_FLR_LINK(pcie_isl) +
                   sizeof pf_atomic_data * NFD_FLR_PF_ind);
    xpb_addr = NFP_PCIEX_COMPCFG_CNTRLR3;

    __mem_read_atomic(&seen_flr, atomic_addr, sizeof seen_flr, sizeof seen_flr,
                      sig_done, &atomic_sig);
    __asm ct[xpb_read, cntrlr3, xpb_addr, 0, 1], sig_done[xpb_sig];

    wait_for_all(&atomic_sig, &xpb_sig);

    /* Test state for an unseen PF FLR */
    if (cntrlr3 & (1 << NFP_PCIEX_COMPCFG_CNTRLR3_FLR_IN_PROGRESS_shf)) {
        if ((seen_flr & pf_atomic_data) == 0) {
            /* We have found an unseen PF FLR, mark it in local and
             * atomic state. */
            *flr_pend_status |= (1 << NFD_FLR_PF_ind);
            *flr_pend_status |= (1 << NFD_FLR_PEND_BUSY_shf);

            mem_bitset_imm(pf_atomic_data, atomic_addr);
        }
    }
}


/** Read the HW FLR and nfd_flr_atomic state
 * @param pcie_isl          PCIe island (0..3)
 * @param flr_pend_status   Internal state for FLR processing
 * @param flr_pend_vf       VF specific internal FLR state
 *
 * See nfd_cfg_internal.c for a description of the format of
 * "flr_pend_status" and "flr_pend_vf".
 */
__intrinsic void
nfd_flr_check_vfs(unsigned int pcie_isl,
                  __shared __gpr unsigned int *flr_pend_status,
                  __shared __gpr unsigned int flr_pend_vf[2])
{
    __xread unsigned int seen_flr[2];
    __xread unsigned int hw_flr[2];
    __mem char *atomic_addr;
    SIGNAL atomic_sig;
    __xread unsigned int cntrlr3;
    unsigned int xpb_addr;
    SIGNAL xpb_sig0, xpb_sig1;

    unsigned int new_flr;
    __xwrite unsigned int new_flr_wr[2];

    /* Read state of FLR hardware and seen atomics */
    atomic_addr = NFD_FLR_LINK(pcie_isl);
    __mem_read_atomic(seen_flr, atomic_addr, sizeof seen_flr, sizeof seen_flr,
                      sig_done, &atomic_sig);
    xpb_addr = NFP_PCIEX_COMPCFG_PCIE_VF_FLR_IN_PROGRESS0;
    __asm ct[xpb_read, hw_flr + 0, xpb_addr, 0, 1], sig_done[xpb_sig0];
    xpb_addr = NFP_PCIEX_COMPCFG_PCIE_VF_FLR_IN_PROGRESS1;
    __asm ct[xpb_read, hw_flr + 4, xpb_addr, 0, 1], sig_done[xpb_sig1];

    wait_for_all(&atomic_sig, &xpb_sig0, &xpb_sig1);

    /* Handle VFs 0 to 31 */
    new_flr = hw_flr[0] & ~seen_flr[NFD_FLR_VF_LO_ind];
    flr_pend_vf[NFD_FLR_VF_LO_ind] |= new_flr;
    if (flr_pend_vf[NFD_FLR_VF_LO_ind] != 0) {
        *flr_pend_status |= (1 << NFD_FLR_VF_LO_ind);
        *flr_pend_status |= (1 << NFD_FLR_PEND_BUSY_shf);
    }
    new_flr_wr[NFD_FLR_VF_LO_ind] = new_flr;

    /* Handle VFs 32 to 63 */
    new_flr = hw_flr[1] & ~seen_flr[NFD_FLR_VF_HI_ind];
    flr_pend_vf[NFD_FLR_VF_HI_ind] |= new_flr;
    if (flr_pend_vf[NFD_FLR_VF_HI_ind] != 0) {
        *flr_pend_status |= (1 << NFD_FLR_VF_HI_ind);
        *flr_pend_status |= (1 << NFD_FLR_PEND_BUSY_shf);
    }
    new_flr_wr[NFD_FLR_VF_HI_ind] = new_flr;

    /* Update the nfd_flr_seen atomic */
    mem_bitset(new_flr_wr, atomic_addr, sizeof new_flr_wr);
}


/** Write the CFG BAR to indicate an FLR is in process
 * @param isl_base      start address of the CFG BARs for the PCIe island
 * @param vnic          vNIC number on the PCIe island
 *
 * NFP_NET_CFG_CTRL is cleared so that the vNIC will be disabled, and
 * NFP_NET_CFG_UPDATE is set to "NFP_NET_CFG_UPDATE_GEN |
 * NFP_NET_CFG_UPDATE_RESET | NFP_NET_CFG_UPDATE_MSIX".  This means that
 * MEs processing the message can respond to it as an FLR if required, or
 * simply behave as if the vNIC was being downed.
 *
 * This method can be called for both the PF and the VFs, with suitable
 * vnic values.
 */
__intrinsic void
nfd_flr_write_cfg_msg(__emem char *isl_base, unsigned int vnic)
{
    __xwrite unsigned int cfg_bar_msg[2] = {0, 0};

    cfg_bar_msg[1] = (NFP_NET_CFG_UPDATE_GEN | NFP_NET_CFG_UPDATE_RESET |
                      NFP_NET_CFG_UPDATE_MSIX);

    mem_write64(cfg_bar_msg, NFD_CFG_BAR(isl_base, vnic),
                sizeof cfg_bar_msg);
}


/** Acknowledge the FLR to the hardware, and clear "nfd_flr_seen" bit
 * @param pcie_isl      PCIe island (0..3)
 *
 * This method issues an XPB write to acknowledge the FLR and an
 * atomic bit clear back-to-back to minimise the likelihood of
 * PCI.IN ME0 seeing an intermediate state.
 */
__intrinsic void
nfd_flr_ack_pf(unsigned int pcie_isl)
{
    __xwrite unsigned int flr_data;
    unsigned int flr_addr;

    unsigned int atomic_data;
    __mem char *atomic_addr;


    flr_addr = ((NFP_PCIEX_ISL_BASE | NFP_PCIEX_COMPCFG_CNTRLR3) |
            (pcie_isl << NFP_PCIEX_ISL_shf));
    flr_data = (1 << NFP_PCIEX_COMPCFG_CNTRLR3_FLR_DONE_shf);

    atomic_addr = (NFD_FLR_LINK(pcie_isl) +
                   sizeof atomic_data * NFD_FLR_PF_ind);
    atomic_data = (1 << NFD_FLR_PF_shf);

    /* Issue the FLR ack and then the atomic clear.  This ensures that
     * the atomic is set until the VF FLR in progress bit is cleared. */
    xpb_write(flr_addr, flr_data);
    mem_bitclr_imm(atomic_data, atomic_addr);

    /* Set NFD_CFG_FLR_AP_SIG_NO so that we recheck the FLR state */
    signal_ctx(NFD_CFG_FLR_AP_CTX_NO, NFD_CFG_FLR_AP_SIG_NO);
}


/** Acknowledge the FLR to the hardware, and clear "nfd_flr_seen" bit
 * @param pcie_isl      PCIe island (0..3)
 * @param vf            VF number on the PCIe island
 *
 * This method issues an XPB write to acknowledge the FLR and an
 * atomic bit clear back-to-back to minimise the likelihood of
 * PCI.IN ME0 seeing an intermediate state.
 */
__intrinsic void
nfd_flr_ack_vf(unsigned int pcie_isl, unsigned int vf)
{
    unsigned int flr_data;
    unsigned int flr_addr;

    __xwrite unsigned int atomic_data;
    __mem char *atomic_addr;

    flr_addr = ((NFP_PCIEX_ISL_BASE | NFP_PCIEX_COMPCFG_CNTRLR3) |
            (pcie_isl << NFP_PCIEX_ISL_shf));
    flr_data = (1 << NFP_PCIEX_COMPCFG_CNTRLR3_VF_FLR_DONE_shf);
    flr_data |= ((vf & NFP_PCIEX_COMPCFG_CNTRLR3_VF_FLR_DONE_CHANNEL_msk) <<
                 NFP_PCIEX_COMPCFG_CNTRLR3_VF_FLR_DONE_CHANNEL_shf);

    /* nfd_flr_seen is a 64bit mask, sorted from LSB to MSB by NFP
     * address definitions.  This places VFs 0..31 in the 4B from
     * offset 0, and VFs 32..63 from offset 4.  "atomic_addr" should
     * be 4B aligned, so we divide vf by 32 and multiply by 4 to
     * obtain the byte offset. */
    atomic_addr = (NFD_FLR_LINK(pcie_isl) + ((vf / 32) * 4));
    atomic_data = 1 << (vf & (32 - 1));

    /* Issue the FLR ack and then the atomic clear.  This ensures that
     * the atomic is set until the VF FLR in progress bit is cleared. */
    xpb_write(flr_addr, flr_data);
    mem_bitclr(&atomic_data, atomic_addr, sizeof atomic_data);

    /* Set NFD_CFG_FLR_AP_SIG_NO so that we recheck the FLR state */
    signal_ctx(NFD_CFG_FLR_AP_CTX_NO, NFD_CFG_FLR_AP_SIG_NO);
}

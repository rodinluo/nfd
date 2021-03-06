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
 * @file          blocks/vnic/shared/nfd_cfg.c
 * @brief         An API to manage access to NFD configuration data
 */

#include <assert.h>
#include <nfp.h>

#include <nfp/me.h>
#include <nfp/mem_bulk.h>
#include <nfp/mem_ring.h>
#include <nfp/xpb.h>

#include <vnic/nfd_common.h>
#include <vnic/shared/nfd.h>
#include <vnic/shared/nfd_cfg.h>
#include <vnic/shared/nfd_xpb.h>
#include <vnic/svc/msix.h>

#include <nfp_net_ctrl.h>



__intrinsic void
nfd_cfg_init_cfg_msg(struct nfd_cfg_msg *cfg_msg, unsigned int rnum,
                      __xread struct nfd_cfg_msg *cfg_msg_rd, SIGNAL *cfg_sig)
{
    mem_ring_addr_t ring_addr;

    /* Zero the current message */
    cfg_msg->__raw = 0;

    /* Prepare to receive a message */
    ring_addr = (unsigned long long) NFD_CFG_EMEM >> 8;
    __mem_workq_add_thread(rnum, ring_addr, cfg_msg_rd, sizeof cfg_msg_rd,
                           sizeof cfg_msg_rd, sig_done, cfg_sig);
}

__intrinsic void
send_interthread_sig(unsigned int dst_me, unsigned int ctx, unsigned int sig_no)
{
    unsigned int addr;

    /* Generic address computation.
     * Could be expensive if dst_me, or dst_xfer
     * not compile time constants */
    addr = ((dst_me & 0x3F0)<<20 | (dst_me & 15)<<9 | (ctx & 7) << 6 |
            (sig_no & 15)<<2);

    __asm ct[interthread_signal, --, addr, 0, --];
}


__intrinsic void
nfd_cfg_check_cfg_msg(struct nfd_cfg_msg *cfg_msg, unsigned int rnum,
                      __xread struct nfd_cfg_msg *cfg_msg_rd, SIGNAL *cfg_sig)
{
    if (signal_test(cfg_sig)) {
        mem_ring_addr_t ring_addr;

        /* Copy message we received into GPRs */
        *cfg_msg = *cfg_msg_rd;

        /* Prepare to receive another message */
        ring_addr = (unsigned long long) NFD_CFG_EMEM >> 8;
        __mem_workq_add_thread(rnum, ring_addr, cfg_msg_rd, sizeof cfg_msg_rd,
                               sizeof cfg_msg_rd, sig_done, cfg_sig);
    }
}


/** Acknowledge PCIe island resets by clearing the "nfd_flr_seen" bit
 * @param pcie_isl      PCIe island (0..3)
 */
__intrinsic void
_nfd_flr_ack_link_reset(unsigned int pcie_isl)
{
    unsigned int atomic_data;
    __mem40 char *atomic_addr;

    atomic_addr = (NFD_FLR_LINK(pcie_isl) +
                   sizeof atomic_data * NFD_FLR_PF_ind);
    atomic_data = (1 << NFD_FLR_PCIE_RESET_shf);
    mem_bitclr_imm(atomic_data, atomic_addr);
}


/** Acknowledge the FLR to the hardware, and clear "nfd_flr_seen" bit
 * @param pcie_isl      PCIe island (0..3)
 *
 * This method issues an XPB write to acknowledge the FLR and an
 * atomic bit clear back-to-back to minimise the likelihood of
 * PCI.IN ME0 seeing an intermediate state.
 */
__intrinsic void
_nfd_flr_ack_pf(unsigned int pcie_isl)
{
    __xwrite unsigned int flr_data;
    unsigned int flr_addr;

    unsigned int atomic_data;
    __mem40 char *atomic_addr;


    flr_addr = ((NFP_PCIEX_ISL_BASE | NFP_PCIEX_FLR_CSR) |
            (pcie_isl << NFP_PCIEX_ISL_shf));
    flr_data = (1 << NFP_PCIEX_FLR_CSR_PF_FLR_DONE_shf);

    atomic_addr = (NFD_FLR_LINK(pcie_isl) +
                   sizeof atomic_data * NFD_FLR_PF_ind);
    atomic_data = (1 << NFD_FLR_PF_shf);

    /* Issue the FLR ack and then the atomic clear.  This ensures that
     * the atomic is set until the VF FLR in progress bit is cleared. */
    xpb_write(flr_addr, flr_data);
    mem_bitclr_imm(atomic_data, atomic_addr);
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
_nfd_flr_ack_vf(unsigned int pcie_isl, unsigned int vf)
{
    unsigned int flr_data;
    unsigned int flr_addr;

    __xwrite unsigned int atomic_data;
    __mem40 char *atomic_addr;

    flr_addr = ((NFP_PCIEX_ISL_BASE | NFP_PCIEX_FLR_CSR) |
            (pcie_isl << NFP_PCIEX_ISL_shf));
    flr_data = (1 << NFP_PCIEX_FLR_CSR_VF_FLR_DONE_shf);
    flr_data |= ((vf & NFP_PCIEX_FLR_CSR_VF_FLR_DONE_CHANNEL_msk) <<
                 NFP_PCIEX_FLR_CSR_VF_FLR_DONE_CHANNEL_shf);

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
}


__intrinsic void
nfd_cfg_app_complete_cfg_msg(unsigned int pcie_isl,
                             struct nfd_cfg_msg *cfg_msg,
                             __dram void *isl_base)
{
    __xwrite unsigned int result;
    __dram char *addr = (__dram char *) isl_base;
    __xread unsigned int update_request;
    SIGNAL read_sig, write_sig;
    unsigned int nfd_isl_master;

    /* Compute the address of the update field */
    addr += cfg_msg->vid * NFP_NET_CFG_BAR_SZ;
    addr += NFP_NET_CFG_UPDATE;

    if (cfg_msg->error) {
        result = NFP_NET_CFG_UPDATE_ERR;
    } else {
        /* XXX add NFP_NET_CFG_UPDATE_SUCCESS value to nfp_net_ctrl.h */
        result = 0;
    }

    /* We could use a mem[swap] here, but the host might be polling
     * the address via the bulk engine, so using separate commands via the
     * bulk engine is safer. */
    __mem_read32(&update_request, addr, sizeof(update_request),
                 sizeof(update_request), sig_done, &read_sig);
    __mem_write32(&result, addr, sizeof(result), sizeof(result),
                  sig_done, &write_sig);
    wait_for_all(&read_sig, &write_sig);

    /* Check for and handle FLRs */
    if (update_request & NFP_NET_CFG_UPDATE_RESET) {
        if ((NFD_MAX_VFS != 0) && NFD_VID_IS_VF(cfg_msg->vid)) {
            /* We have a VF FLR */
            _nfd_flr_ack_vf(pcie_isl, cfg_msg->vid);
        } else {
            /* We have a PF FLR, but is it the last PF vNIC? */
            if (cfg_msg->vid == NFD_LAST_PF) {
                /* Only ack the FLR on the message for the last PF vNIC */
                _nfd_flr_ack_pf(pcie_isl);
            }
        }

        /* Notify the relevant PCIe island master to recheck
         * the FLR configuration */
        nfd_isl_master = NFD_CFG_ISL_MASTER_BASE | (pcie_isl << 4);
        send_interthread_sig(nfd_isl_master, NFD_CFG_FLR_AP_CTX_NO,
                             NFD_CFG_FLR_AP_SIG_NO);
    }

    /* Check for and handle PCIe island resets */
    if (cfg_msg->pci_reset) {
        if (cfg_msg->vid == NFD_LAST_PF) {
            /* All vNICs on this island have started and completed
             * PCIe resets. */

            /* The app master cannot send link state interrupts
             * at this point, because it should be checking
             * NFP_NET_CFG_LSC, and that has been reset for all
             * vNICs.  This means we can clear msix_cur_cpp2pci_addr */
            msix_rst_curr_cpp2pci_addr(pcie_isl);

            /* Only ack the reset on the message for the last VID */
            _nfd_flr_ack_link_reset(pcie_isl);

            /* Notify the relevant PCIe island master to recheck
             * the FLR configuration */
            nfd_isl_master = NFD_CFG_ISL_MASTER_BASE | (pcie_isl << 4);
            send_interthread_sig(nfd_isl_master, NFD_CFG_FLR_AP_CTX_NO,
                                 NFD_CFG_FLR_AP_SIG_NO);
        }
    }
}


__intrinsic void
nfd_cfg_complete_cfg_msg(struct nfd_cfg_msg *cfg_msg, unsigned int rnum)
{
    struct nfd_cfg_msg cfg_msg_tmp;
    __xwrite struct nfd_cfg_msg cfg_msg_wr;
    mem_ring_addr_t ring_addr = (unsigned long long) NFD_CFG_EMEM >> 8;

    /* Clear the internal state fields and set msg_valid before sending  */
    cfg_msg_tmp.__raw = 0;
    cfg_msg_tmp.msg_valid = 1;
    cfg_msg_tmp.error = cfg_msg->error;
    cfg_msg_tmp.pci_reset = cfg_msg->pci_reset;
    cfg_msg_tmp.vid = cfg_msg->vid;
    cfg_msg_wr.__raw = cfg_msg_tmp.__raw;

    /* Add work is guaranteed to not overflow by design (it is larger than
     * the number of possible vNICs). */
    mem_workq_add_work(rnum, ring_addr, &cfg_msg_wr, sizeof cfg_msg_wr);
}


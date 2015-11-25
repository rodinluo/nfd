/*
 * Copyright (C) 2014 Netronome Systems, Inc.  All rights reserved.
 *
 * @file          blocks/vnic/shared/nfd.h
 * @brief         NFD shared defines and macros
 */
#ifndef _BLOCKS__VNIC_SHARED_NFD_H_
#define _BLOCKS__VNIC_SHARED_NFD_H_

#include <nfp_chipres.h>
#include <nfp/mem_atomic.h>     /* TEMP */

#include "nfd_user_cfg.h"

/* Set defines */
#define NFD_MAX_ISL     4   /* Maximum number of PCIe islands NFD may support */

/*
 * CPP2PCIe BAR allocation
 * XXX This should go into chip_res
 */
enum pcie_cpp2pcie_bar {
    PCIE_CPP2PCIE_QMON = 0,
    PCIE_CPP2PCIE_LSC  = 1,
    PCIE_CPP2PCIE_FREE2,
    PCIE_CPP2PCIE_FREE3,
    PCIE_CPP2PCIE_FREE4,
    PCIE_CPP2PCIE_FREE5,
    PCIE_CPP2PCIE_FREE6,
    PCIE_CPP2PCIE_FREE7
};

/* Helper macros */

/* Expand a constant PCIe number (0..3) to an EMEM island (emem0..emem2),
 * using the customer defined mapping provided by the NFD_PCIEX_EMEM defines.
 */
#define NFD_EMEM_IND1(_emem) _emem
#define NFD_EMEM_IND0(_isl) NFD_EMEM_IND1(NFD_PCIE##_isl##_EMEM)
#define NFD_EMEM(_isl) NFD_EMEM_IND0(_isl)

/* Get a load time constant EMEM address for a specific
 * (constant) PCIe island */
#define NFD_EMEM_LINK_IND2(_emem) __LoadTimeConstant("__addr_" #_emem)
#define NFD_EMEM_LINK_IND1(_emem) NFD_EMEM_LINK_IND2(_emem)
#define NFD_EMEM_LINK_IND0(_isl) NFD_EMEM_LINK_IND1(NFD_EMEM(_isl))
#define NFD_EMEM_LINK(_isl) NFD_EMEM_LINK_IND0(_isl)

/* Expand an EMEM island to a load time constant.  This macro is used
 * specifically for the NFD_IN_WQ_SHARED define. */
#define NFD_EMEM_SHARED_IND(_emem) __LoadTimeConstant("__addr_" #_emem)
#define NFD_EMEM_SHARED(_emem) NFD_EMEM_SHARED_IND(_emem)


/* Access the generic resource ring number for the input and output rings
 * "_isl" is a constant island number, "_comp" is either "nfd_out" or
 * "nfd_in", and "_num" is the ring number.  For "nfd_out", "_num"
 * must be zero.  For "nfd_in", "_num" may be [0 .. (NFD_IN_NUM_WQS-1)]. */
 /* Added "nfd_in_issued_lso" "_num" should always for 0 */
#define NFD_RING_LINK_IND(_isl, _comp, _num) \
    _link_sym(_comp##_ring_num##_isl##_num)
#define NFD_RING_LINK(_isl, _comp, _num) NFD_RING_LINK_IND(_isl, _comp, _num)


/* The "init_done" atomic is a bitmask where each ME within the PCIe islands
 * is given one bit.  When each ME completes it initialisation, it sets
 * its bit.  NFD users can inspect the "init_done" atomic to determine
 * whether NFD has finished its initialisation. */

/* Declare memory for "init_done" atomic. */
/* XXX Remove NFD_INIT_DONE_DECLARE or leave?
 * NB size 8 is minimum that NFCC and NFAS can share */
#define NFD_INIT_DONE_DECLARE_IND1(_emem)                               \
    _NFP_CHIPRES_ASM(.alloc_mem nfd_init_done_atomic _emem global 64 64) \
    _NFP_CHIPRES_ASM(.declare_resource nfd_init_done_mem                \
                     global 8 nfd_init_done_atomic)                     \
    _NFP_CHIPRES_ASM(.alloc_resource nfd_init_done nfd_init_done_mem global 8 8)

#define NFD_INIT_DONE_DECLARE_IND0(_emem) NFD_INIT_DONE_DECLARE_IND1(_emem)
#define NFD_INIT_DONE_DECLARE NFD_INIT_DONE_DECLARE_IND0(NFD_CFG_RING_EMEM)

/* Abstract away details of the island:ME to bit mapping.
 * PCIe islands have 4 MEs. */
#define NFD_INIT_DONE_SET_IND0(_isl, _me)                       \
    mem_bitset_imm(1<<(_isl * 4 + _me),                         \
                   (__dram void *) _link_sym(nfd_init_done))
#define NFD_INIT_DONE_SET(_isl, _me) NFD_INIT_DONE_SET_IND0(_isl, _me)

/* Stringify an EMEM name for use in _nfp_has_island() tests. */
#define NFD_EMEM_CHK_IND(_emem) #_emem
#define NFD_EMEM_CHK(_emem) NFD_EMEM_CHK_IND(_emem)

/* User define consistency checks */
#ifndef NFD_MAX_VF_QUEUES
#error "NFD_MAX_VF_QUEUES is not defined but is required"
#endif

#ifndef NFD_MAX_PF_QUEUES
#error "NFD_MAX_PF_QUEUES is not defined but is required"
#endif

#ifndef NFD_MAX_VFS
#error "NFD_MAX_VFS is not defined but is required"
#endif


/* Require that at least some queues are used by NFD. */
#if ((NFD_MAX_VF_QUEUES * NFD_MAX_VFS) + NFD_MAX_PF_QUEUES) == 0)
#error "PF and VF options imply that no queues are in use"
#endif


/* NFD_MAX_VFS is used to determine PF vNIC number, so must
 * always be consistent with VFs in use.  The ambiguous case
 * where N VFs with 0 queues are requested is illegal. */
#if ((NFD_MAX_VF_QUEUES == 0) && (NFD_MAX_VFS != 0))
#error "NFD_MAX_VFS must be zero if NFD_MAX_VF_QUEUES equals zero"
#endif


/* Ensure that the user provides NFD_CFG_VF_CAP if they
 * want to use VFs. */
#if NFD_MAX_VF_QUEUES != 0
#ifndef NFD_CFG_VF_CAP
#error NFD_CFG_VF_CAP must be defined
#endif
#endif

/* Ensure that the user provides NFD_CFG_PF_CAP if they
 * want to use PFs. */
#if NFD_MAX_PF_QUEUES != 0
#ifndef NFD_CFG_PF_CAP
#error NFD_CFG_PF_CAP must be defined
#endif
#endif


/* Check that the user points NFD to usable EMUs for each EMU specified */
#ifdef NFD_IN_WQ_SHARED
    #if !_nfp_has_island(NFD_EMEM_CHK(NFD_IN_WQ_SHARED))
        #error "NFD_IN_WQ_SHARED specifies an unavailable EMU"
    #endif
#endif

#ifdef NFD_PCIE0_EMEM
    #if _nfp_has_island("pcie0")
        #if !_nfp_has_island(NFD_EMEM_CHK(NFD_PCIE0_EMEM))
            #error "NFD_PCIE0_EMEM specifies an unavailable EMU"
        #endif
    #else
        #error "NFD_PCIE0_EMEM defined but pcie0 unavailable"
    #endif
#endif

#ifdef NFD_PCIE1_EMEM
    #if _nfp_has_island("pcie1")
        #if !_nfp_has_island(NFD_EMEM_CHK(NFD_PCIE1_EMEM))
            #error "NFD_PCIE1_EMEM specifies an unavailable EMU"
        #endif
    #else
        #error "NFD_PCIE1_EMEM defined but pcie1 unavailable"
    #endif
#endif

#ifdef NFD_PCIE2_EMEM
    #if _nfp_has_island("pcie2")
        #if !_nfp_has_island(NFD_EMEM_CHK(NFD_PCIE2_EMEM))
            #error "NFD_PCIE2_EMEM specifies an unavailable EMU"
        #endif
    #else
        #error "NFD_PCIE2_EMEM defined but pcie2 unavailable"
    #endif
#endif

#ifdef NFD_PCIE3_EMEM
    #if _nfp_has_island("pcie3")
        #if !_nfp_has_island(NFD_EMEM_CHK(NFD_PCIE3_EMEM))
            #error "NFD_PCIE3_EMEM specifies an unavailable EMU"
        #endif
    #else
        #error "NFD_PCIE3_EMEM defined but pcie3 unavailable"
    #endif
#endif


/* For NFP6000 chips < revision B0 a software workaround is used
 * to ensure that the final bytes of a packet that is not a 4B
 * multiple in size are byte swapped correctly.  This workaround
 * is incompatible with gather support.  If the capabilities
 * advertise gather, throw an error. */
#if __REVISION_MIN < __REVISION_B0
#if ((NFD_CFG_VF_CAP & NFP_NET_CFG_CTRL_GATHER) || \
     (NFD_CFG_PF_CAP & NFP_NET_CFG_CTRL_GATHER))
#error "NFP_NET_CFG_CTRL_GATHER not supported for A0 chips"
#endif
#endif

/* Debug defines */
#ifdef NFD_VNIC_DBG_CHKS

/* A mask used to test whether a value may be a legal MU buffer.
 * This provides very coarse checking for illegal MU buffers in NFD. */
#ifndef NFD_MU_PTR_DBG_MSK
#define NFD_MU_PTR_DBG_MSK 0x1f000000
#endif

#endif


/* Shared structures */
/**
 * Compact storage of NFD ring addresses and ring numbers
 */
struct nfd_ring_info {
    union {
        struct {
            unsigned int addr_hi:8;     /**< EMU access bits */
            unsigned int sp0:14;        /**< Spare */
            unsigned int rnum:10;       /**< Ring number */
        };
        unsigned int __raw;
    };
};


#endif /* !_BLOCKS__VNIC_SHARED_NFD_H_ */

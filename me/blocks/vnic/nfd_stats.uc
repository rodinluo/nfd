/*
 * Copyright (C) 2014 - 2016 Netronome Systems, Inc.  All rights reserved.
 *
 * @file          blocks/vnic/nfd_stats.uc
 * @brief         Microcode interface to NFD atomic stats
 */

#ifndef __NFD_STATS_UC
#define __NFD_STATS_UC

#include <nfd_user_cfg.h>

/* Should be in nfd_user_cfg.h, but issues in nfd_cfg_pf_bars.h prevents this */
#ifndef NFD_MAX_PFS
#if NFD_MAX_PF_QUEUES != 0
#define NFD_MAX_PFS 1
#else /* NFD_MAX_PF_QUEUES != 0 */
#define NFD_MAX_PFS 0
#endif /* NFD_MAX_PF_QUEUES != 0 */
#endif /* NFD_MAX_PFS */


/* Each statistic is a 64-bit count + a 64-bit byte-count */
#define NFD_STAT_SIZE   16


#define NFD_STAT_MAX_QUEUES     64

#macro _nfd_stat_declare_help(NAME)

    #if ((NFD_MAX_VFS * NFD_MAX_VF_QUEUES + NFD_MAX_PFS * NFD_MAX_PF_QUEUES) > NFD_STAT_MAX_QUEUES)
        #error "Total number of NFD queues per island cannot exceed NFD_STAT_MAX_QUEUES"
    #endif

    #define_eval __STATSZ (NFD_MAX_ISL * NFD_STAT_MAX_QUEUES * NFD_STAT_SIZE)
    #if (__STATSZ < 256)
        #error "expecting each statistic to take up at least 256 bytes"
    #endif

    .alloc_mem NAME imem global __STATSZ __STATSZ

    #undef __STATSZ

#endm


#macro nfd_stats_declare_out()

    _nfd_stat_declare_help(_nfd_stats_out_sent)
    _nfd_stat_declare_help(_nfd_stats_out_drop)

#endm

#macro nfd_stats_declare_in()

    _nfd_stat_declare_help(_nfd_stats_in_recv)

#endm

#macro _nfd_stats_update(STAT, in_pcie, in_qid, in_nbytes)
.begin
    .reg addr_hi
    .reg addr_lo
    .reg iref

    move(addr_hi, (STAT >> 8))
    alu[addr_lo, --, B, in_pcie, <<(log2(NFD_STAT_MAX_QUEUES * NFD_STAT_SIZE))]
    alu[addr_lo, addr_lo, OR, in_qid, <<(log2(NFD_STAT_SIZE))]
    mem[incr64, --, addr_hi, <<8, addr_lo]

    #if (!isnum(in_nbytes) || (in_nbytes != 0))
        alu[addr_lo, addr_lo, +, 8]
        ; override length  = (1 << 7)
        ; override dataref = (2 << 3)
        ; length[2] = 1 for 64-bit operations = (1 << 10)
        ; length[3] = 1 for to pull operand from dataref = (1 << 11)
        move(iref, ( (2 << 3) | (1 << 7) | (1 << 10) | (1 << 11)))
        alu[--, iref, OR, in_nbytes, <<16]
        mem[add64_imm, --, addr_hi, <<8, addr_lo], indirect_ref
    #endif

.end
#endm


#macro _nfd_stats_extract(STAT, in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
.begin
    .reg addr_hi
    .reg addr_lo
    .sig read_byte_cnt
    .sig read_cnt

    move(addr_hi, (STAT >> 8))
    alu[addr_lo, --, B, in_pcie, <<(log2(NFD_STAT_MAX_QUEUES * NFD_STAT_SIZE))]
    alu[addr_lo, addr_lo, OR, in_qid, <<(log2(NFD_STAT_SIZE))]
    mem[atomic_read, count_dst_xfer, addr_hi, <<8, addr_lo, 2], sig_done[read_cnt]

    alu[addr_lo, addr_lo, +, 8]
    mem[atomic_read, byte_count_dst_xfer, addr_hi, <<8, addr_lo, 2], sig_done[read_byte_cnt]

    ctx_arb[read_byte_cnt, read_cnt]

.end
#endm


#macro nfd_stats_update_sent(in_pcie, in_qid, in_nbytes)
    _nfd_stats_update(_nfd_stats_out_sent, in_pcie, in_qid, in_nbytes)
#endm


#macro nfd_stats_update_sent(in_pcie, in_qid)
    nfd_stats_update_sent(in_pcie, in_qid, 0)
#endm


#macro nfd_stats_update_dropped(in_pcie, in_qid, in_nbytes)
    _nfd_stats_update(_nfd_stats_out_drop, in_pcie, in_qid, in_nbytes)
#endm


#macro nfd_stats_update_dropped(in_pcie, in_qid)
    nfd_stats_update_dropped(in_pcie, in_qid, 0)
#endm


#macro nfd_stats_update_received(in_pcie, in_qid, in_nbytes)
    _nfd_stats_update(_nfd_stats_in_recv, in_pcie, in_qid, in_nbytes)
#endm


#macro nfd_stats_update_received(in_pcie, in_qid)
    nfd_stats_update_received(in_pcie, in_qid, 0)
#endm


#macro nfd_stats_update_debug1(in_pcie, in_qid, in_nbytes)
    _nfd_stats_update(_nfd_stats_debug1, in_pcie, in_qid, in_nbytes)
#endm


#macro nfd_stats_update_debug1(in_pcie, in_qid)
    nfd_stats_update_debug1(in_pcie, in_qid, 0)
#endm


#macro nfd_stats_update_debug2(in_pcie, in_qid, in_nbytes)
    _nfd_stats_update(_nfd_stats_debug2, in_pcie, in_qid, in_nbytes)
#endm


#macro nfd_stats_update_debug2(in_pcie, in_qid)
    nfd_stats_update_debug1(in_pcie, in_qid, 0)
#endm


#macro nfd_stats_extract_sent(in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
    _nfd_stats_extract(_nfd_stats_out_sent, in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
#endm


#macro nfd_stats_extract_dropped(in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
    _nfd_stats_extract(_nfd_stats_out_drop, in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
#endm


#macro nfd_stats_extract_received(in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
    _nfd_stats_extract(_nfd_stats_in_recv, in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
#endm


#macro nfd_stats_extract_debug1(in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
    _nfd_stats_extract(_nfd_stats_debug1, in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
#endm


#macro nfd_stats_extract_debug2(in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
    _nfd_stats_extract(_nfd_stats_debug2, in_pcie, in_qid, count_dst_xfer, byte_count_dst_xfer)
#endm


#endif /* __NFD_STATS_UC */

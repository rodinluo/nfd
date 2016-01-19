/*
 * Copyright (C) 2014-2016,  Netronome Systems, Inc.  All rights reserved.
 *
 * @file          blocks/vnic/pci_in/gather_status.h
 * @brief         Display the state of the gather and service_qc blocks
 */
#ifndef _BLOCKS__VNIC_PCI_IN_GATHER_STATUS_H_
#define _BLOCKS__VNIC_PCI_IN_GATHER_STATUS_H_

#define STATUS_GATHER_START      16
#define STATUS_QUEUE_START       24
#define STATUS_Q_SEL_START       31

struct nfd_in_gather_status {
    unsigned int actv_bmsk_hi;
    unsigned int actv_bmsk_lo;
    unsigned int actv_bmsk_proc;
    unsigned int pend_bmsk_hi;
    unsigned int pend_bmsk_lo;
    unsigned int pend_bmsk_proc;
    unsigned int dma_issued;
    unsigned int dma_compl;
};


#if defined (__NFP_LANG_MICROC)

/**
 * Initialise the gather and notify status output
 */
extern void gather_status_setup();

/**
 * Show gather and notify status in transfer registers
 */
extern void gather_status();

#endif /* __NFP_LANG_MICROC */

#endif /* !_BLOCKS__VNIC_PCI_IN_GATHER_STATUS_H_ */


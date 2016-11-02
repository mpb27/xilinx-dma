/*
 * DMA driver for Xilinx DMA Engine
 *
 * Copyright (C) 2010 - 2015 Xilinx, Inc. All rights reserved.
 * Copyright (C) 2016 Ping DSP, Inc. All rights reserved.
 *
 * Based on the Freescale DMA driver.
 *
 * Description:
 *  The AXI DMA, is a soft IP, which provides high-bandwidth Direct Memory
 *  Access between memory and AXI4-Stream-type target peripherals. It can be
 *  configured to have one channel or two channels and if configured as two
 *  channels, one is to transmit data from memory to a device and another is
 *  to receive from a device.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/dma/xilinx_dma.h>
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_dma.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/slab.h>

#include "dmaengine.h"

/* Register Offsets */
#define XILINX_DMA_REG_CONTROL		0x00
#define XILINX_DMA_REG_STATUS		0x04
#define XILINX_DMA_REG_SRCDSTADDR	0x18
#define XILINX_DMA_REG_SRCDSTADDRMSB	0x1C
#define XILINX_DMA_REG_BTT		0x28

/* Channel/Descriptor Offsets */
#define XILINX_DMA_MM2S_CTRL_OFFSET	0x00
#define XILINX_DMA_S2MM_CTRL_OFFSET	0x30

/* General register bits definitions */
#define XILINX_DMA_CR_RUNSTOP_MASK	BIT(0)
#define XILINX_DMA_CR_RESET_MASK	BIT(2)

#define XILINX_DMA_SR_HALTED_MASK	BIT(0)
#define XILINX_DMA_SR_IDLE_MASK		BIT(1)

#define XILINX_DMA_XR_IRQ_IOC_MASK	BIT(12)
#define XILINX_DMA_XR_IRQ_ERROR_MASK	BIT(14)
#define XILINX_DMA_XR_IRQ_ALL_MASK	(BIT(14) | BIT(12))

/* BD definitions */
#define XILINX_DMA_BD_STS_ALL_MASK	GENMASK(31, 28)
#define XILINX_DMA_BD_SOP		BIT(27)
#define XILINX_DMA_BD_EOP		BIT(26)

/* Hw specific definitions */
#define XILINX_DMA_MAX_CHANS_PER_DEVICE	2
#define XILINX_DMA_MAX_TRANS_LEN	GENMASK(22, 0)

/* Delay loop counter to prevent hardware failure */
#define XILINX_DMA_LOOP_COUNT		1000000
#define XILINX_DMA_TX_HISTORY           32
#define XILINX_DMA_PERIPHERAL_ID	0x000A3500


#define xilinx_dma_poll_timeout(chan, reg, val, cond, delay_us, timeout_us) \
	readl_poll_timeout(chan->xdev->regs + chan->ctrl_offset + reg, val, \
			   cond, delay_us, timeout_us)

/**
 * struct xilinx_dma_tx_descriptor - Per Transaction structure
 * @async_tx: Async transaction descriptor
 * @node: Node in the channel descriptors list
 */
struct xilinx_dma_tx_descriptor {
	struct dma_async_tx_descriptor async_tx;
	struct list_head node;

	u32 requested_length;
	u32 transferred_length;
	u32 * transferred_length_ptr;
};

enum xilinx_dma_chan_status {
	CHAN_IDLE,
	CHAN_BUSY,
	CHAN_ERROR
};

/**
 * struct xilinx_dma_chan - Driver specific DMA channel structure
 * @common: DMA common channel
 * @xdev: Driver specific device structure
 * @dev: The dma device
 * @lock: Descriptor operation lock
 * @status: Channel status
 * @pending_transactions: Transactions waiting
 * @active_transaction: Currently active transaction
 * @completed_transactions: Transactions completed
 * @tasklet: Cleanup work after irq / completed transaction cleanup.

 * @ctrl_offset: Control registers offset
 * @id: Channel ID
 * @irq: Channel IRQ
 * @name: String name
 * @direction: Channel direction
 * @max_transaction_length: Maximum transaction length
 */
struct xilinx_dma_chan {
	struct dma_chan          common;
	struct xilinx_dma_device *xdev;
	struct device            *dev;

	spinlock_t lock;

	enum xilinx_dma_chan_status       status;

	struct list_head                  pending_transactions;
	struct xilinx_dma_tx_descriptor  *active_transaction;
	struct list_head                  completed_transactions;

	struct tasklet_struct             tasklet;

	/* Constant values after initialization. */
	u32 ctrl_offset;
	int id;
	int irq;
	char *name;
	enum dma_transfer_direction direction;
	u32 max_transaction_length;
	u32 peri_id;
};

/**
 * struct xilinx_dma_device - DMA device structure
 * @regs: I/O mapped base address
 * @dev: Device Structure
 * @common: DMA device structure
 * @chan: Driver specific DMA channel
 */
struct xilinx_dma_device {
	void __iomem *regs;
	struct device *dev;
	struct dma_device common;
	struct xilinx_dma_chan *chan[XILINX_DMA_MAX_CHANS_PER_DEVICE];
	u32 nr_channels;
	u32 chan_id;
};

/* Macros */
#define to_xilinx_chan(chan) \
	container_of(chan, struct xilinx_dma_chan, common)
#define to_xilinx_tx_descriptor(tx) \
	container_of(tx, struct xilinx_dma_tx_descriptor, async_tx)

/* IO accessors
 *
 * Accessors for the registers of each channel using the base address of the
 * device (xdev->regs) plus the offset of the channel plus the offset of the
 * regiseter.
 *
 * Note: Removed dma_write / dma_read to make sure we don't
 *       accidentally use them, since they are not required.
 */
static inline u32 dma_ctrl_read(struct xilinx_dma_chan *chan, u32 reg)
{
	return ioread32(chan->xdev->regs + chan->ctrl_offset + reg);
}

static inline void dma_ctrl_write(struct xilinx_dma_chan *chan, u32 reg,
				  u32 value)
{
	/* Combine the device register base address (regs) with
	 * the channel control offset, and the register offset and
	 * issue a 32-bit write.
	 */
	iowrite32(value, chan->xdev->regs + chan->ctrl_offset + reg);
}

#ifdef CONFIG_PHYS_ADDR_T_64BIT
static inline void dma_ctrl_writeq(struct xilinx_dma_chan *chan, u32 reg,
				   u64 value)
{
	/* Same but for a 64-bit write.
	 */
	writeq(value, chan->xdev->regs + chan->ctrl_offset + reg);
}
#endif

/* Automatically choose the correct of either write or writeq depending on dma_addr_t
 * or more specifically on the define that defines dma_addr_t as either u32 or u64.
 *
 * Note: we're actually using the define for phys_addr_t, which raises the question
 * should we be using phys_addr_t?
 * Purpose: to cut down on the number of ifdefs required in this driver!
 */
static inline void dma_ctrl_write_addr(struct xilinx_dma_chan *chan, u32 reg,
	                               dma_addr_t value)
{
#ifdef CONFIG_PHYS_ADDR_T_64BIT
	dma_ctrl_writeq(chan, reg, value);
#else
	dma_ctrl_write(chan, reg, value);
#endif
}

static inline void dma_ctrl_clear(struct xilinx_dma_chan *chan, u32 reg, u32 clear)
{
	dma_ctrl_write(chan, reg, dma_ctrl_read(chan, reg) & ~clear);
}

static inline void dma_ctrl_set(struct xilinx_dma_chan *chan, u32 reg, u32 set)
{
	dma_ctrl_write(chan, reg, dma_ctrl_read(chan, reg) | set);
}


/**
 * xilinx_dma_tx_descriptor - Allocate transaction descriptor
 * @chan: Driver specific dma channel
 *
 * Return: The allocated descriptor on success and NULL on failure.
 */
static struct xilinx_dma_tx_descriptor *
xilinx_dma_alloc_tx_descriptor(struct xilinx_dma_chan *chan)
{
	struct xilinx_dma_tx_descriptor *desc;

	/* Allocate memory using devm_kzalloc() which guarentees to free
	 * automatically when the driver exits.
	 */
	desc = devm_kzalloc(chan->dev, sizeof(*desc), GFP_KERNEL);

	if (!desc)
		return NULL;

	return desc;
}

/**
 * xilinx_dma_free_tx_descriptor - Free transaction descriptor
 * @chan: Driver specific dma channel
 * @desc: dma transaction descriptor
 */
static void
xilinx_dma_free_tx_descriptor(struct xilinx_dma_chan *chan,
			      struct xilinx_dma_tx_descriptor *desc)
{
	if (!desc)
		return;

	devm_kfree(chan->dev, desc);
}

/**
 * xilinx_dma_alloc_chan_resources - Allocate channel resources
 * @dchan: DMA channel
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_dma_alloc_chan_resources(struct dma_chan *dchan)
{
	struct xilinx_dma_chan *chan = to_xilinx_chan(dchan);

	/* Initialize the channel cookie counter, which sets both
	 * cookie, and completed_cookie to 1.
	 */
	dma_cookie_init(dchan);

	/* Enable interrupts */
	// NOT SURE WHY WE WOULD ENABLE INTERRUPTS HERE
	// SO MUST CHECK THIS :TODO:
	dma_ctrl_set(chan, XILINX_DMA_REG_CONTROL, XILINX_DMA_XR_IRQ_ALL_MASK);

	return 0;
}

/**
 * xilinx_dma_free_desc_list - Free descriptors list
 * @chan: Driver specific dma channel
 * @list: List to parse and delete the descriptor
 */
static void xilinx_dma_free_desc_list(struct xilinx_dma_chan *chan,
				      struct list_head *list)
{
	struct xilinx_dma_tx_descriptor *desc, *next;

	/* Remove each descriptor from the list, and free it. */
	list_for_each_entry_safe(desc, next, list, node) {
		list_del(&desc->node);
		xilinx_dma_free_tx_descriptor(chan, desc);
	}
}

/**
 * xilinx_dma_free_descriptors - Free channel descriptors
 * @chan: Driver specific dma channel
 */
static void xilinx_dma_free_descriptors(struct xilinx_dma_chan *chan)
{
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);

	/* Free all the descriptors in all lists. */
	xilinx_dma_free_desc_list(chan, &chan->pending_transactions);
	xilinx_dma_free_desc_list(chan, &chan->completed_transactions);
	xilinx_dma_free_tx_descriptor(chan, chan->active_transaction);
	chan->active_transaction = NULL;

	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_dma_free_chan_resources - Free channel resources
 * @dchan: DMA channel
 */
static void xilinx_dma_free_chan_resources(struct dma_chan *dchan)
{
	struct xilinx_dma_chan *chan = to_xilinx_chan(dchan);

	xilinx_dma_free_descriptors(chan);
}

/**
 * xilinx_dma_tx_status - Get dma transaction status
 * @dchan: DMA channel
 * @cookie: Transaction identifier
 * @txstate: Transaction state
 *
 * Return: DMA transaction status
 */
static enum dma_status xilinx_dma_tx_status(struct dma_chan *dchan,
					    dma_cookie_t cookie,
					    struct dma_tx_state *txstate)
{
	struct xilinx_dma_chan *chan = to_xilinx_chan(dchan);
	struct xilinx_dma_tx_descriptor *transaction, *at;
	enum dma_status ret;
	unsigned long flags;
	u32 residue = -1;

	/* Check if asking for the active transaction. */
	spin_lock_irqsave(&chan->lock, flags);
	at = chan->active_transaction;
	if (at && at->async_tx.cookie == cookie) {

		/* Active transaction!  Lets get it from register with IRQ disabled. */
		residue = at->requested_length - dma_ctrl_read(chan, XILINX_DMA_REG_BTT);

		dma_cookie_status(dchan, cookie, txstate);
		dma_set_residue(txstate, residue);
		spin_unlock_irqrestore(&chan->lock, flags);
		return DMA_IN_PROGRESS;
	}
	spin_unlock_irqrestore(&chan->lock, flags);


	/* Not an active transaction. */

	/* Check every completed transaction for this cookie, and
	 * update the residue if found.
	 *
	 * note: check in reverse order because newest entries are
	 *       at the tail, and its likely the newest is being asked for.
	 */
	list_for_each_entry_reverse(transaction, &chan->completed_transactions, node) {
		if (transaction->async_tx.cookie == cookie) {
			residue = (transaction->requested_length
				   - transaction->transferred_length);
			break;
		}
	}

	/* Get the transaction status based on the cookie value, vs the
	 * completed_cookie value in the channel structure.
	 *
	 * note: this also updates residue to 0 in txstate
	 */
	ret = dma_cookie_status(dchan, cookie, txstate);

	/* Update the proper residue.  This will be either -1 if not found,
	 * or the value from the completed transactions.
	 */
	dma_set_residue(txstate, residue);

	return ret;
}

/**
 * xilinx_dma_halt - Halt DMA channel
 * @chan: Driver specific DMA channel
 */
static void xilinx_dma_hw_halt(struct xilinx_dma_chan *chan)
{
	int err = 0;
	u32 val;

	dma_ctrl_clear(chan, XILINX_DMA_REG_CONTROL, XILINX_DMA_CR_RUNSTOP_MASK);

	/* Wait for the hardware to halt.
	 *
	 * FIXME: This is a problem at the moment because the Xilinx AXI DMA
	 *        S2MM hardware does not assert HALTED if there is no AXI-Stream
	 *        data available.  For now, just warn and ignore.
	 *
	 *        One fix would be to issue a reset of the core when we want to
	 *        halt it, however that resets both the S2MM and MM2S channels!
	 */

	err = xilinx_dma_poll_timeout(chan, XILINX_DMA_REG_STATUS, val,
				      (val & XILINX_DMA_SR_HALTED_MASK), 0,
				      XILINX_DMA_LOOP_COUNT);

	if (err) {
		dev_warn(chan->dev, "Cannot stop channel %s : SR = %x\n",
			chan->name, dma_ctrl_read(chan, XILINX_DMA_REG_STATUS));
	}

	chan->status = CHAN_IDLE;
}

/**
 * xilinx_dma_start - Start DMA channel
 * @chan: Driver specific DMA channel
 */
static void xilinx_dma_hw_start(struct xilinx_dma_chan *chan)
{
	int err = 0;
	u32 val;

	/* Set the RUN bit in DMA Control Register. */
	dma_ctrl_set(chan, XILINX_DMA_REG_CONTROL, XILINX_DMA_CR_RUNSTOP_MASK);

	/* Wait for the hardware to start.  HALTED bit must go low in the Status Register. */
	err = xilinx_dma_poll_timeout(chan, XILINX_DMA_REG_STATUS, val,   // poll register
				      !(val & XILINX_DMA_SR_HALTED_MASK), // exit condition
				      0, XILINX_DMA_LOOP_COUNT);          // loop delay and timeout

	if (err) {
		dev_err(chan->dev, "Cannot start channel %s (%p) : SR = %x\n",
			chan->name, chan, dma_ctrl_read(chan, XILINX_DMA_REG_STATUS));
		chan->status = CHAN_ERROR;
		return;
	}

	chan->status = CHAN_IDLE;
}

/**
 * xilinx_dma_start_transfer_irq - Starts DMA transfer
 * @chan: Driver specific channel struct pointer
 */
static void xilinx_dma_start_transfer_irq(struct xilinx_dma_chan *chan)
{
	struct xilinx_dma_tx_descriptor *transaction;

	if (chan->status != CHAN_IDLE || list_empty(&chan->pending_transactions)) {
		/* No need to start the channel if it isn't IDLE or there are
		 * no pending transactions.
		 */
		return;
	}

	if (unlikely(chan->active_transaction != NULL)) {
		dev_err(chan->dev, "Channel %s has active transaction but status is IDLE?\n", chan->name);
		return;
	}

	/* Get the next transaction. */
	transaction = list_first_entry(&chan->pending_transactions, struct xilinx_dma_tx_descriptor, node);

	/* Assign the transactions source/destination memory address to the Xilinx DMA hardware. */
	dma_ctrl_write_addr(chan, XILINX_DMA_REG_SRCDSTADDR, transaction->async_tx.phys);

	/* Enable the DMA hardware if it is not already started. */
	xilinx_dma_hw_start(chan);

	if (chan->status != CHAN_IDLE) {
		return;
	}

	/* Remove the transaction from the pending list. */
	list_del(&transaction->node);

	/* Start the transfer */
	chan->status = CHAN_BUSY;
	chan->active_transaction = transaction;
	dma_ctrl_write(chan, XILINX_DMA_REG_BTT, transaction->requested_length);
}

/**
 * xilinx_dma_issue_pending - Issue pending transactions
 * @dchan: DMA channel
 */
static void xilinx_dma_issue_pending(struct dma_chan *dchan)
{
	struct xilinx_dma_chan *chan = to_xilinx_chan(dchan);
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);
	xilinx_dma_start_transfer_irq(chan);
	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_dma_chan_tx_completed_cleanup - Execute any callbacks that need to be executed.
 */
static void xilinx_dma_chan_tx_completed_cleanup(struct xilinx_dma_chan *chan)
{
	struct xilinx_dma_tx_descriptor *desc, *next;
	unsigned long flags;
	unsigned long count = 0;

	spin_lock_irqsave(&chan->lock, flags);

	/* Run any callbacks which have not been executed, and count the number
	 * of completed transactions.
	 */
	list_for_each_entry_safe(desc, next, &chan->completed_transactions, node) {

		count++;

		if (desc->async_tx.callback) {
			dma_async_tx_callback callback = desc->async_tx.callback;

			spin_unlock_irqrestore(&chan->lock, flags);
			callback(desc->async_tx.callback_param);
			spin_lock_irqsave(&chan->lock, flags);

			desc->async_tx.callback = NULL;
		}
	}

	/* Finalize and free any completed transactions above some number that are kept
	 * for dma_status calls.
	 */
	while (count > XILINX_DMA_TX_HISTORY) {

		/* Get the first descriptor. */
		desc = list_first_entry(&chan->completed_transactions,
				struct xilinx_dma_tx_descriptor, node);

		/* Remove from the list of completed transactions. */
		list_del(&desc->node);

		/* Run any dependencies, then free the descriptor. */
		dma_run_dependencies(&desc->async_tx);
		xilinx_dma_free_tx_descriptor(chan, desc);

		/* Decrement the number of transactions. */
		count--;

	}

	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_dma_complete_active_irq - Mark the active descriptor as complete
 * @chan : xilinx DMA channel
 *
 * Context: IRQ Handler
 */
static void xilinx_dma_complete_active_irq(struct xilinx_dma_chan *chan)
{
	dma_cookie_t save_cookie;
	struct xilinx_dma_tx_descriptor *transaction;

	transaction = chan->active_transaction;

	if (transaction == NULL) {
		return;
	}

	/* Update the transfered length in the context if it was provided. */
	if (transaction->transferred_length_ptr) {
		*(transaction->transferred_length_ptr) =
			transaction->transferred_length;
	}

	/* NULL out the active transaction since it is no longer active. */
	chan->active_transaction = NULL;

	/* Complete the cookie for this transaction, but maintain the cookie value. */
	/* All this function does is set the chan->completed_cookie to this transactions cookie value,
	 * and resets this transactions cookie value, but since we still want to use it
	 * as a loookup for dma_status / dma_state, we will keep it.
	 */
	save_cookie = transaction->async_tx.cookie;
	dma_cookie_complete(&transaction->async_tx);
	transaction->async_tx.cookie = save_cookie;

	/* Add the completed transaction to the completed_transactions list. */
	list_add_tail(&transaction->node, &chan->completed_transactions);

	/* Change channel status to IDLE. */
	chan->status = CHAN_IDLE;

}

/**
 * xilinx_dma_hw_reset - Reset DMA channel
 * @chan: Driver specific DMA channel
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_dma_hw_reset(struct xilinx_dma_chan *chan)
{
	int err = 0;
	u32 val;

	dma_ctrl_set(chan, XILINX_DMA_REG_CONTROL, XILINX_DMA_CR_RESET_MASK);

	/* Wait for the hardware to finish reset */
	err = xilinx_dma_poll_timeout(chan, XILINX_DMA_REG_CONTROL, val,
				      !(val & XILINX_DMA_CR_RESET_MASK),
				      1, XILINX_DMA_LOOP_COUNT);

	if (err) {
		dev_err(chan->dev, "reset timeout, cr %x, sr %x\n",
			dma_ctrl_read(chan, XILINX_DMA_REG_CONTROL),
			dma_ctrl_read(chan, XILINX_DMA_REG_STATUS));
		chan->status = CHAN_ERROR;
		return -EBUSY;
	}

	chan->status = CHAN_IDLE;

	return err;
}

/**
 * xilinx_dma_irq_handler - DMA Interrupt handler
 * @irq: IRQ number
 * @data: Pointer to the Xilinx DMA channel structure
 *
 * Return: IRQ_HANDLED/IRQ_NONE
 */
static irqreturn_t xilinx_dma_irq_handler(int irq, void *data)
{
	struct xilinx_dma_chan *chan = data;
	u32 status;

	/* Read the status and ack the interrupts. */
	status = dma_ctrl_read(chan, XILINX_DMA_REG_STATUS);
	if (!(status & XILINX_DMA_XR_IRQ_ALL_MASK)) {
		return IRQ_NONE;
	}

	dma_ctrl_write(chan, XILINX_DMA_REG_STATUS,
		       status & XILINX_DMA_XR_IRQ_ALL_MASK);

	if (status & XILINX_DMA_XR_IRQ_ERROR_MASK) {
		dev_err(chan->dev,
			"Channel %s (%p) has errors.  DMACR: %x  DMASR: %x .\n",
			chan->name, chan,
			dma_ctrl_read(chan, XILINX_DMA_REG_CONTROL),
			dma_ctrl_read(chan, XILINX_DMA_REG_STATUS));
		chan->status = CHAN_ERROR;
		return IRQ_HANDLED;
	}

	/* Check if Interrupt on Complete (IOC) has occured.
	 */
	if (status & XILINX_DMA_XR_IRQ_IOC_MASK) {
		struct xilinx_dma_tx_descriptor *at = chan->active_transaction;

		/* Check that there is an active transaction. */
		if (!at) {
			/* This is caused by the way terminate_all() is implemented
			 * and the Xilinx DMA soft IP core's halt mechanism.
			 * Terminate all tells the HW core to halt, and frees
			 * all of the descriptors (active, pending and completed).
			 * But the HW core will not halt until after the active
			 * transaction is completed, and since we cannot guarentee
			 * there will be one, we cannot wait for this to occur.
			 *
			 * There is a potential danger here:
			 * If the client calls terminate_all() and then frees
			 * the DMA buffer backing the active transaction, the
			 * HW core will still try to complete the active transaction.
			 * In order to prevent this, the DMA core needs to be
			 * reset in terminate_all() but that resets BOTH the
			 * channels! FUBAR.
			 *
			 * Fortunately, my axis-reader module does not free DMA
			 * buffers unless it is being unloaded.
			 */
			dev_err(chan->dev, "Channel %s fired interrupt without "
				"an active transaction!\n", chan->name);
			return IRQ_HANDLED;
		}

		/* Update the transferred number of bytes. */
		at->transferred_length = dma_ctrl_read(chan, XILINX_DMA_REG_BTT);

		spin_lock(&chan->lock);
		xilinx_dma_complete_active_irq(chan);
		xilinx_dma_start_transfer_irq(chan);
		spin_unlock(&chan->lock);
	}

	tasklet_schedule(&chan->tasklet);
	return IRQ_HANDLED;
}

/**
 * xilinx_dma_do_tasklet - Schedule completion tasklet
 * @data: Pointer to the Xilinx dma channel structure
 */
static void xilinx_dma_do_tasklet(unsigned long data)
{
	struct xilinx_dma_chan *chan = (struct xilinx_dma_chan *)data;

	xilinx_dma_chan_tx_completed_cleanup(chan);
}


/**
 * xilinx_dma_tx_submit - Submit DMA transaction
 * @tx: Async transaction descriptor
 *
 * Return: cookie value on success and failure value on error
 */
static dma_cookie_t xilinx_dma_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct xilinx_dma_tx_descriptor *desc = to_xilinx_tx_descriptor(tx);
	struct xilinx_dma_chan          *chan = to_xilinx_chan(tx->chan);

	dma_cookie_t cookie;
	unsigned long flags;
	int err;

	if (chan->status == CHAN_ERROR) {
		/* Try recovery */
		dev_warn(chan->dev, "Channel %s (%p) is in error state.  Attempting reset.\n",
			chan->name, chan);

		err = xilinx_dma_hw_reset(chan); /* careful, DMA reset, resets both channels! */
		if (err < 0) {

			dev_err(chan->dev, "Reset failed for channel %s (%p).  Driver in-operable.\n",
				chan->name, chan);

			xilinx_dma_free_tx_descriptor(chan, desc);
			return -EIO;
		}
	}

	/* Don't interrupt adding the transaction. */
	spin_lock_irqsave(&chan->lock, flags);

	/* Assign a new cookie to this transaction.
	 * This assignment assigns cookie+1 and sets chan and tx cookie to the new value.
	 * If an overflow happens, cookie is assigned to 1.
	 */
	cookie = dma_cookie_assign(tx);

	/* Put this transaction onto the tail of the pending queue. */
	list_add_tail(&desc->node, &chan->pending_transactions);

	spin_unlock_irqrestore(&chan->lock, flags);

	return cookie;
}



/**
 * xilinx_dma_prep_slave_sg - prepare descriptors for a DMA_SLAVE transaction
 * @dchan: DMA channel
 * @sgl: scatterlist to transfer to/from
 * @sg_len: number of entries in @scatterlist
 * @direction: DMA direction
 * @flags: transfer ack flags
 * @context: APP words of the descriptor
 *
 * Return: Async transaction descriptor on success and NULL on failure
 */
static struct dma_async_tx_descriptor *xilinx_dma_prep_slave_sg(
	struct dma_chan *dchan, struct scatterlist *sgl, unsigned int sg_len,
	enum dma_transfer_direction direction, unsigned long flags,
	void *context)
{
	struct xilinx_dma_chan *chan = to_xilinx_chan(dchan);
	struct xilinx_dma_tx_descriptor *desc;

	if (direction != chan->direction) {
		dev_warn(chan->dev, "Direction of transaction and channel must be the same.\n");
		return NULL;
	}

	if (sg_len != 1) {
		dev_warn(chan->dev, "Driver only supports 1 SG per transaction.\n");
		return NULL;
	}

	if (sg_dma_len(sgl) > chan->max_transaction_length) {
		dev_warn(chan->dev,
			"Tranaction longer than maximum allowed by the Xilinx core (%d).\n",
			chan->max_transaction_length);
		return NULL;
	}

	/* Allocate a transaction descriptor. */
	desc = xilinx_dma_alloc_tx_descriptor(chan);
	if (!desc)
		return NULL;

	/* Assign context pointer to transaction transferred bytes. */
	if (context) {
		desc->transferred_length_ptr = (u32 *) context;
		*(desc->transferred_length_ptr) = 0;
	}

	/* Initialize the common descriptor from dmaengine.h. */
	dma_async_tx_descriptor_init(&desc->async_tx, &chan->common);
	desc->async_tx.tx_submit = xilinx_dma_tx_submit;


	desc->async_tx.phys = sg_dma_address(sgl);
	desc->requested_length =  sg_dma_len(sgl);
	desc->transferred_length = 0;

	return &desc->async_tx;

//error:
//	xilinx_dma_free_tx_descriptor(chan, desc);
//	return NULL;
}


/**
 * xilinx_dma_terminate_all - Halt the channel and free descriptors
 * @dchan: DMA Channel pointer
 *
 * Return: '0' always
 */
static int xilinx_dma_terminate_all(struct dma_chan *dchan)
{
	struct xilinx_dma_chan *chan = to_xilinx_chan(dchan);

	/* Halt the DMA engine */
	xilinx_dma_hw_halt(chan);

	/* Remove and free all of the descriptors in the lists */
	xilinx_dma_free_descriptors(chan);

	return 0;
}

/**
 * xilinx_dma_chan_probe - Per Channel Probing
 * It get channel features from the device tree entry and
 * initialize special channel handling routines
 *
 * @xdev: Driver specific device structure
 * @node: Device node
 * @chan_id: Channel id
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_dma_chan_probe(struct xilinx_dma_device *xdev,
				 struct device_node *node, int chan_id)
{
	struct xilinx_dma_chan *chan;
	bool has_dre;
	u32 width;
	int err;

	/* Read parameters from device tree. */
	has_dre = of_property_read_bool(node, "xlnx,include-dre");

	err = of_property_read_u32(node, "xlnx,datawidth", &width);
	if (err) {
		dev_err(xdev->dev, "Missing datawidth property.\n");
		return err;
	}
	width = width >> 3; /* Convert bits to bytes */

	/* If data width is greater than 8 bytes, DRE is not in hw. */
	/* can data widths be larger than 8 bytes? should this just return an error? */
	if (width > 8)
		has_dre = false;

	if (!has_dre)
		xdev->common.copy_align = fls(width - 1);


	/* Allocate memory for the channel. */
	/* note: devm_kzalloc'd memory is automatically freed when the driver unloads */
	chan = devm_kzalloc(xdev->dev, sizeof(*chan), GFP_KERNEL);
	if (!chan) {
		return -ENOMEM;
	}

	/* Initialize parameters */
	chan->dev = xdev->dev;
	chan->xdev = xdev;
	chan->status = CHAN_IDLE;
	chan->id = chan_id;

	if (of_device_is_compatible(node, "xlnx,axi-dma-mm2s-channel")) {
		/* Set channel as a Memory to Stream. */
		chan->direction = DMA_MEM_TO_DEV;
		chan->ctrl_offset = XILINX_DMA_MM2S_CTRL_OFFSET;
		chan->name = "xilinx-dma-mm2s";
		chan->peri_id = XILINX_DMA_PERIPHERAL_ID | DMA_MEM_TO_DEV;
		chan->common.private = &(chan->peri_id);
	} else if (of_device_is_compatible(node, "xlnx,axi-dma-s2mm-channel")) {
		/* Set channel as a Stream to Memory. */
		chan->direction = DMA_DEV_TO_MEM;
		chan->ctrl_offset = XILINX_DMA_S2MM_CTRL_OFFSET;
		chan->name = "xilinx-dma-s2mm";
		chan->peri_id = XILINX_DMA_PERIPHERAL_ID | DMA_DEV_TO_MEM;
		chan->common.private = &(chan->peri_id);
	} else {
		/* Incompatible channel. */
		dev_err(xdev->dev, "Invalid channel compatible node.\n");
		return -EINVAL;
	}

	spin_lock_init(&chan->lock);
	INIT_LIST_HEAD(&chan->pending_transactions);
	INIT_LIST_HEAD(&chan->completed_transactions);
	chan->active_transaction = NULL;

	/* Reset the hardware. */
	err = xilinx_dma_hw_reset(chan);  /* careful, dma reset must reset both channels */
	if (err) {
		dev_err(xdev->dev, "Reset channel %s (%p) failed.\n",
			chan->name, chan);
		return err;
	}

	/* DMA is reset and halted, so we can write to BTT register without
	 * issues.  Use a dummy write to the BTT register to determine its
	 * size.
	 */
	dma_ctrl_write(chan, XILINX_DMA_REG_BTT, 0xFFFFFFFF);
	chan->max_transaction_length = dma_ctrl_read(chan, XILINX_DMA_REG_BTT);
	dma_ctrl_write(chan, XILINX_DMA_REG_BTT, 0x00000000);

	/* Also use the max_transaction_length to determine if the BTT register exists. */
	if (chan->max_transaction_length == 0) {
		dev_err(xdev->dev, "Unable to determine max transaction length for channel %s.\n",
			chan->name);
		dev_err(xdev->dev, "The Xilinx DMA core is likely configured in scatter-gather mode "
				"instead of direct-register mode.\n");
		return -EIO;
	}


	/* Find the IRQ line, if it exists in the device tree. */
	chan->irq = irq_of_parse_and_map(node, 0);

	/* Request the interrupt and link it to a handler. */
	err = request_irq(chan->irq, xilinx_dma_irq_handler,
			  IRQF_SHARED, chan->name, chan);
	if (err) {
		dev_err(xdev->dev, "Unable to request IRQ %d for channel %s (%p).\n",
			chan->irq, chan->name, chan);
		return err;
	}

	/* Initialize the tasklet */
	tasklet_init(&chan->tasklet, xilinx_dma_do_tasklet,
		     (unsigned long)chan);

	/* Initialize DMA channel and add it to the DMA engine channels list. */
	chan->common.device = &xdev->common;
	list_add_tail(&chan->common.device_node, &xdev->common.channels);
	xdev->chan[chan->id] = chan;

	dev_info(xdev->dev, "Probed channel %s with IRQ %d and max transaction length of %d.\n",
		chan->name, chan->irq, chan->max_transaction_length);

	return 0;
}

/**
 * xilinx_dma_chan_remove - Per Channel remove function
 * @chan: Driver specific DMA channel
 */
static void xilinx_dma_chan_remove(struct xilinx_dma_chan *chan)
{
	/* Disable interrupts */
	dma_ctrl_clear(chan, XILINX_DMA_REG_CONTROL, XILINX_DMA_XR_IRQ_ALL_MASK);

	if (chan->irq > 0)
		free_irq(chan->irq, chan);

	tasklet_kill(&chan->tasklet);

	list_del(&chan->common.device_node);
}


/**
 * xilinx_dma_channel_probe - Per channel node probe
 * It get channel features from the device tree entry and
 * initialize special channel handling routines
 *
 * @xdev: Driver specific device structure
 * @node: Device node
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_dma_channel_probe(struct xilinx_dma_device *xdev,
				    struct device_node *node) {
	int ret, i, nr_channels;

	ret = of_property_read_u32(node, "dma-channels", &nr_channels);
	if (ret) {
		dev_err(xdev->dev, "Unable to read dma-channels property.\n");
		return ret;
	}

	xdev->nr_channels += nr_channels;

	for (i = 0; i < nr_channels; i++)
		xilinx_dma_chan_probe(xdev, node, xdev->chan_id++);

	return 0;
}


/**
 * of_dma_xilinx_xlate - Translation function
 * @dma_spec: Pointer to DMA specifier as found in the device tree
 * @ofdma: Pointer to DMA controller data
 *
 * Return: DMA channel pointer on success and NULL on error
 */
static struct dma_chan *of_dma_xilinx_xlate(struct of_phandle_args *dma_spec,
					    struct of_dma *ofdma)
{
	struct xilinx_dma_device *xdev = ofdma->of_dma_data;
	int chan_id = dma_spec->args[0];

	if (chan_id >= xdev->nr_channels)
		return NULL;

	return dma_get_slave_channel(&xdev->chan[chan_id]->common);
}

/**
 * xilinx_dma_probe - Driver probe function
 * @pdev: Pointer to the platform_device structure
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_dma_probe(struct platform_device *pdev)
{
	struct xilinx_dma_device *xdev;
	struct device_node *child, *node;
	struct resource *res;
	int i, ret;

	/* Allocate Xilinx version of the device. */
	xdev = devm_kzalloc(&pdev->dev, sizeof(*xdev), GFP_KERNEL);
	if (!xdev)
		return -ENOMEM;

	xdev->dev = &(pdev->dev);
	INIT_LIST_HEAD(&xdev->common.channels);

	node = pdev->dev.of_node;

	/* Map the registers */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	xdev->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(xdev->regs))
		return PTR_ERR(xdev->regs);

	/* Check if SG is enabled */
	if (of_property_read_bool(node, "xlnx,include-sg")) {
		dev_err(&pdev->dev, "Driver does not support SG mode.\n");
		return -EIO;
	}

	if (of_property_read_bool(node, "xlnx,multichannel-dma")) {
		dev_err(&pdev->dev, "Driver does not support multichannel DMA.\n");
		return -EIO;
	}

	/* Axi DMA only do slave transfers */
	dma_cap_set(DMA_SLAVE, xdev->common.cap_mask);
	dma_cap_set(DMA_PRIVATE, xdev->common.cap_mask);

	xdev->common.device_prep_slave_sg = xilinx_dma_prep_slave_sg;
	xdev->common.device_terminate_all = xilinx_dma_terminate_all;
	xdev->common.device_issue_pending = xilinx_dma_issue_pending;
	xdev->common.device_alloc_chan_resources =
		xilinx_dma_alloc_chan_resources;
	xdev->common.device_free_chan_resources =
		xilinx_dma_free_chan_resources;
	xdev->common.device_tx_status = xilinx_dma_tx_status;

	/* Device supports both S2MM and MM2S. */
	xdev->common.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);

	/* Residue is updated with each completed segment in the descriptor. */
	xdev->common.residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;

	xdev->common.dev = &pdev->dev;
	xdev->chan_id = 0;

	platform_set_drvdata(pdev, xdev);

	for_each_child_of_node(node, child) {
		ret = xilinx_dma_channel_probe(xdev, child);
		if (ret) {
			dev_err(&pdev->dev, "Probing channels failed.\n");
			goto free_chan_resources;
		}
	}

	dma_async_device_register(&xdev->common);

	ret = of_dma_controller_register(node, of_dma_xilinx_xlate, xdev);
	if (ret) {
		dev_err(&pdev->dev, "Unable to register DMA to DT.\n");
		dma_async_device_unregister(&xdev->common);
		goto free_chan_resources;
	}

	dev_info(&pdev->dev, "Xilinx AXI DMA Engine driver probed! (direct-register mode)\n");

	return 0;

free_chan_resources:
	for (i = 0; i < xdev->nr_channels; i++)
		if (xdev->chan[i])
			xilinx_dma_chan_remove(xdev->chan[i]);

	return ret;
}

/**
 * xilinx_dma_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 *
 * Return: Always '0'
 */
static int xilinx_dma_remove(struct platform_device *pdev)
{
	struct xilinx_dma_device *xdev = platform_get_drvdata(pdev);
	int i;

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&xdev->common);

	for (i = 0; i < xdev->nr_channels; i++)
		if (xdev->chan[i])
			xilinx_dma_chan_remove(xdev->chan[i]);

	dev_info(&pdev->dev, "module exited\n");
	return 0;
}

static const struct of_device_id xilinx_dma_of_match[] = {
	{ .compatible = "xlnx,axi-dma-1.00.a",},
	{}
};
MODULE_DEVICE_TABLE(of, xilinx_dma_of_match);

static struct platform_driver xilinx_dma_driver = {
	.driver = {
		.name = "xilinx-dma-dr",
		.of_match_table = xilinx_dma_of_match,
	},
	.probe = xilinx_dma_probe,
	.remove = xilinx_dma_remove,
};

module_platform_driver(xilinx_dma_driver);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_AUTHOR("Ping DSP, Inc.");
MODULE_DESCRIPTION("Xilinx AXI-Stream DMA Driver (direct-register mode)");
MODULE_LICENSE("GPL");

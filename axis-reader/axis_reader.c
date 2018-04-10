/*
 * AXI4-Stream Reader character device driver for Xilinx DMA S2MM driver.
 *
 * Copyright (C) 2016 Ping DSP, Inc.
 *
 * Description:
 *  This driver creates a character device (/dev/axisreader0) that can be used
 *  to read complete AXI4-Stream packets.  It uses an S2MM (DMA_DEV_TO_MEM)
 *  channel provided by the xilinx-dma-dr DMA driver and creates a 4 packet
 *  circular buffer.  The maximum packet length is specified in bytes by the
 *  max_packet_length parameter.  The driver automatically finds the first
 *  available (not requested / taken by some other kernel module) S2MM channel
 *  and creates /dev/axisreader0.
 *
 * Example usage (Python):
 *   ar0 = os.open("/dev/axisreader0", os.O_RDONLY)
 *   data = os.read(ar0, 1024*1024)
 *   if len(data) == 0:
 *       print("No AXI4-Stream packet available.")
 *   else:
 *       print("Got AXI4-Stream packet of length %d." % len(data))
 *   os.close(ar0)
 *
 * License:
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 * References:
 *   - https://stackoverflow.com/a/34032364/953414
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/ioctl.h>
#include <asm/ioctls.h>

#define IS_NULL(x) (x == NULL)
#define DRIVER_NAME "axis-reader"

/* Simple example of how to receive command line parameters to your module.
   Delete if you don't need them */
int max_packet_length = 1*1024*1024;

module_param(max_packet_length, int, S_IRUGO);

static struct class * ar_class;

struct ar_transaction
{
        struct ar_channel* channel;              ///< Channel that created the transaction.
        struct list_head node;                   ///< Node for adding transaction to a list.

        dma_cookie_t     dma_cookie;             ///< Completion cookie.
        u8*              dma_buffer;             ///< Pointer to allocated buffer in virtual memory.
        dma_addr_t       dma_buffer_addr;        ///< DMA buffer physical memory address.
        u32              dma_buffer_len;         ///< Requested length of the DMA transfer.
        u32              dma_completed_len;      ///< Actual length of completed transaction.
};

struct ar_channel
{
        bool is_open;
        spinlock_t lock;
        wait_queue_head_t wait_completed;

        /* DMA */
        struct dma_chan *dma;                    ///< DMA channel.

        /* Transactions */
        struct list_head free_transactions;
        struct list_head pending_transactions;
        struct list_head completed_transactions;

        /* Character device variables. */
        dev_t           dev_number;              ///< Allocated device number major and minor.
        struct device*  dev_entry;               ///< Device for /dev/ entry.
        struct cdev     char_device;             ///< Character device.

        /* Status information */
        u64     status_dropped;
        u64     status_dropped_bytes;
        u64     status_completed;
        u64     status_completed_bytes;
        u64     status_error;
};

static struct ar_channel arc0 = {0};    ///< Zero initialized channel structure.


/* Header pre */
static int ar_transaction_submit(struct ar_transaction* tx);
static void ar_transactions_start(struct ar_channel* ch);
static void ar_transactions_stop(struct ar_channel* ch);



/* Callback executed by the DMA engine once a transaction completes.
 *
 * This callback takes the completed transaction out of the pending transactions
 * list and moves it to the completed transaction list.  It also populates the
 * actual number of bytes transferred by the DMA operation.  Finally it adds
 * a free transaction to the pending transactions list and submits it to the
 * DMA engine for processing.
 */
static void ar_transaction_callback(void *transaction)
{
        int err;
        unsigned long flags;
        enum dma_status status;
        struct dma_tx_state state;
        struct ar_transaction *tx_next;
        struct ar_transaction *tx = transaction;
        struct ar_channel *ch = tx->channel;

        /* Transaction has been completed.  Retrieve the completed size using
         * the cookie.
         */
        status = dmaengine_tx_status(ch->dma, tx->dma_cookie, &state);
        if (unlikely(status != DMA_COMPLETE || state.residue < 0)) {
                if (status != DMA_COMPLETE)
                        dev_warn(ch->dev_entry, "DMA transaction finished with"
                                " an error. (%d)", status);
                else if (state.residue < 0)
                        dev_warn(ch->dev_entry,
                                "DMA transaction residue is negative.\n");

                /* Move transaction to free list. */
                spin_lock_irqsave(&ch->lock, flags);
                list_move_tail(&tx->node, &ch->free_transactions);
                spin_unlock_irqrestore(&ch->lock, flags);

                ch->status_error++;
                return;
        }

        /* Use transaction residue to compute the actual completed bytes.
         */
        tx->dma_completed_len = tx->dma_buffer_len - state.residue;

        /* All of the list operations should be atomic, just to be safe.
         */
        spin_lock_irqsave(&ch->lock, flags);

        /* Move this transaction from the pending transactions list
         * to the completed list.
         */
        list_move_tail(&tx->node, &ch->completed_transactions);

        /* Wake up anyone waiting on the next completed transaction.
         * Typically this would be user code blocking in arf_read().
         */
        wake_up_interruptible(&ch->wait_completed);

        /* Every time a transactionsction completes, submit a new one by
         * moving one from either the free transactions list or if not
         * available by taking one from the completed transactions, and
         * incrementing a dropped tranaction counter.
         */
        if (unlikely(list_empty(&ch->free_transactions))) {
                /* We don't have a free transaction.  We need to get one from
                 * the completed transactions, and increment status.
                 * We will be getting the 2nd completed transaction (not first)
                 * so check that the list is not empty and not singular.
                 */
                if (list_empty(&ch->completed_transactions) 
                    || list_is_singular(&ch->completed_transactions)) {
                        /* Something has gone horribly wrong, there are no
                         * completed or free tranasctions!
                         */
                        spin_unlock_irqrestore(&ch->lock, flags);
                        dev_err(ch->dev_entry, "Ran out of transactions!!!");
                        ch->status_error++;
                        return;
                }

                /* Take the 2nd entry in the completed transactions list because
                 * the first's length may have been returned using ioctl to the user.
                 */
                tx_next = list_first_entry(&ch->completed_transactions, 
                        struct ar_transaction, node);
                tx_next = list_next_entry(tx_next, node);

                ch->status_dropped++;
                ch->status_dropped_bytes += tx_next->dma_completed_len;
        } else {
                /* We have free transactions.  Lets grab the first one.
                 */
                tx_next = list_first_entry(&ch->free_transactions,
                                struct ar_transaction, node);
        }

        /* Move the next transaction from its list (either free or completed)
         * to the pending list.
         */
        list_move_tail(&tx_next->node, &ch->pending_transactions);

        /* Restore interrupts now, no more list operations left.
         */
        spin_unlock_irqrestore(&ch->lock, flags);

        /* Submit the next transaction to the DMA, and start it.  This is done
         * outside of the interrupt disable.
         */
        err = ar_transaction_submit(tx_next);
        if (unlikely(err)) {
                /* On an error, take the transaction and move it back to free
                 * transaction list.
                 */
                 spin_lock_irqsave(&ch->lock, flags);
                 list_move_tail(&tx_next->node, &ch->free_transactions);
                 spin_unlock_irqrestore(&ch->lock, flags);
                 ch->status_error++;
                 return;
        }

        ar_transactions_start(tx_next->channel);
}

static struct ar_transaction * ar_transaction_create(struct ar_channel* chan)
{
        struct ar_transaction *tx;

        /* Allocate transaction structure.
         */
        tx = devm_kzalloc(chan->dev_entry, sizeof(*tx), GFP_KERNEL);
        if (!tx) {
                dev_err(chan->dev_entry, "Failed to allocate memory for"
                        "ar-transaction descriptor.\n");
                return NULL;
        }

        /* Initialize transaction variables.
         */
        tx->channel = chan;
        tx->dma_buffer_len = max_packet_length;

        /* Allocate DMA space.
         */
        dma_set_coherent_mask(chan->dev_entry, 0xFFFFFFFF);
        tx->dma_buffer = dmam_alloc_coherent(chan->dev_entry,
                tx->dma_buffer_len, &tx->dma_buffer_addr, GFP_KERNEL);
        if (!tx->dma_buffer) {
                dev_err(chan->dev_entry, "Failed to allocate DMA continuous"
                        "memory in CMA.\n");
                devm_kfree(chan->dev_entry, tx);
                return NULL;
        }

        return tx;
}

static void ar_transaction_destroy(struct ar_transaction* tx)
{
        /* If the transaction is in a list, lets remove it from the list first. */

        dmam_free_coherent(tx->channel->dev_entry, tx->dma_buffer_len,
                tx->dma_buffer, tx->dma_buffer_addr);
        devm_kfree(tx->channel->dev_entry, tx);
}

static int ar_transaction_submit(struct ar_transaction* tx)
{
        enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
        enum dma_transfer_direction direction = DMA_DEV_TO_MEM;
        struct dma_async_tx_descriptor *tx_desc;

        /* Create a transaction descriptor for this transaction and
         * submit it to the DMA engine.
         */
        tx_desc = dmaengine_prep_slave_single(tx->channel->dma,
                tx->dma_buffer_addr, tx->dma_buffer_len, direction, flags);

        if (!tx_desc) {
                dev_err(tx->channel->dev_entry,
                        "Failed to prepare DMA tranaction.\n");
                return -EBUSY;
        }

        /* Setup the callback with a pointer to this transaction.
         */
        tx_desc->callback = ar_transaction_callback;
        tx_desc->callback_param = tx;

        /* Submit the transaction to the DMA engine.  Note we don't
         * keep tx_desc around because it is no longer under our control
         * after the transaction has been submitted.
         */
        tx->dma_cookie = dmaengine_submit(tx_desc);
        tx_desc = NULL;

        if (tx->dma_cookie < 0) {
                /* Invalid cookie, report error.  Submit free'd the tx_desc. */
                dev_err(tx->channel->dev_entry,
                        "Failed to submit DMA transaction (%d).\n", tx->dma_cookie);
                return -EBUSY;
        }

        return 0;
}

static void ar_transactions_start(struct ar_channel *ch) {
        /* Start the DMA. */
        dma_async_issue_pending(ch->dma);
}

static void ar_transactions_stop(struct ar_channel *ch) {
        dmaengine_terminate_all(ch->dma);
}

static int list_count(struct list_head *head) {
        int count = 0;
        struct list_head *pos;
        list_for_each(pos, head) {
                count++;
        }
        return count;
}

static int arf_open(struct inode *ino, struct file *file)
{
        unsigned long flags;
        struct ar_transaction *tx1, *tx2;
        struct ar_channel *ch = container_of(ino->i_cdev, struct ar_channel, char_device);

        if (ch->is_open)
                return -EBUSY;

        if (list_count(&ch->free_transactions) < 2)  {
                dev_err(ch->dev_entry, "Could not open() because there aren't"
                        " 2 free transactions.\n");
                return -EFAULT;
        }

        // TODO: improve this code, add more error checking.

        /* Move 2 transactions from free to pending list. */
        spin_lock_irqsave(&ch->lock, flags);
        tx1 = list_first_entry_or_null(&ch->free_transactions,
                        struct ar_transaction, node);
        list_move_tail(&tx1->node, &ch->pending_transactions);
        tx2 = list_first_entry_or_null(&ch->free_transactions,
                        struct ar_transaction, node);
        list_move_tail(&tx2->node, &ch->pending_transactions);
        spin_unlock_irqrestore(&ch->lock, flags);

        /* Start transactions. */
        ar_transaction_submit(tx1);
        ar_transaction_submit(tx2);
        ar_transactions_start(ch);

        file->private_data = ch;
        ch->is_open = true;
        return 0;
}

static int arf_release(struct inode *ino, struct file *file)
{
        unsigned long flags;
        //struct ar_channel *ch = container_of(ino->i_cdev, struct ar_channel, char_device);
        struct ar_channel *ch = file->private_data;
        struct ar_transaction *tx, *next;

        // Mark as closed.
        ch->is_open = false;

        // Stop transcting, move completed and pending back to free.
        ar_transactions_stop(ch);

        spin_lock_irqsave(&ch->lock, flags);
        list_for_each_entry_safe(tx, next, &ch->pending_transactions, node) {
                list_move_tail(&tx->node, &ch->free_transactions);
        }

        list_for_each_entry_safe(tx, next, &ch->completed_transactions, node) {
                list_move_tail(&tx->node, &ch->free_transactions);
        }
        spin_unlock_irqrestore(&ch->lock, flags);

        return 0;
}


static ssize_t arf_read(struct file *file, char *buffer, size_t len, loff_t *fpos)
{
        long remain, ret;
        unsigned long flags, txlen;
        size_t offset = fpos ? *fpos : 0;
        struct ar_channel *ch = file->private_data;
        struct ar_transaction *tx = NULL;

        /* Pattern is from http://stackoverflow.com/a/23493619/953414
         * Also see http://www.makelinux.net/ldd3/chp-6-sect-2
         */
        spin_lock_irqsave(&ch->lock, flags);
        while (list_empty(&ch->completed_transactions)) {

                /* Unlock while waiting, or if non-blocking. */
                spin_unlock_irqrestore(&ch->lock, flags);

                if (file->f_flags & O_NONBLOCK) {
                        /* No completed transaction, and non-blocking read so
                         * exit returning either 0 or -EAGAIN.
                         * Ref: http://www.xml.com/ldd/chapter/book/ch05.html#t3
                         */
                        return -EAGAIN;
                }

                /* Wait for a completed transaction to be added to the list.
                 */
                ret = wait_event_interruptible(ch->wait_completed,
                        !list_empty(&ch->completed_transactions));

                if (ret) {
                        dev_err(ch->dev_entry, "Blocking read() interrupted %ld.\n",
                                ret);
                        return -EFAULT;
                }

                /* Lock again for the while check, and if we exit the
                 * while loop, the list will be locked.
                 */
                spin_lock_irqsave(&ch->lock, flags);
        }

        /* A completed transaction is available.  Get it but don't remove it yet.
         */
        tx = list_first_entry(&ch->completed_transactions,
                struct ar_transaction, node);

        /* Check that there is enough space in the user buffer for transaction.
         */
        txlen = tx->dma_completed_len;        
        if ((len - offset) < txlen) {
                /* Not enough space, we haven't moved the transaction so we can just
                 * unlock and return an error code.  This is not an error in the driver
                 * so no need to increment any error codes.
                 */
                spin_unlock_irqrestore(&ch->lock, flags);
                return -EINVAL;    /* Transaciton larger than the buffer provided. */
                                   /* TODO: Enable partial reads. */
        }

        /* We can now safely remove the transaction from the completed list because
         * we know if will fit in the user space buffer.
         */
        list_del(&tx->node);

        /* Unlock for the copy and other checks.
         */
        spin_unlock_irqrestore(&ch->lock, flags);

        /* Copy transaction data to the user buffer.
         */
        remain = copy_to_user(&buffer[offset], tx->dma_buffer, txlen);

        /* Done using the transaction, we can now move it to the free transactions.
         */
        spin_lock_irqsave(&ch->lock, flags);
        list_add(&tx->node, &ch->free_transactions);
        spin_unlock_irqrestore(&ch->lock, flags);

        if (remain != 0) {
                /* Should never fail.
                 */
                ch->status_error++;
                return -EIO;
        }

        return txlen;
}

static unsigned int arf_poll(struct file *file, poll_table *wait)
{
    unsigned int ret = 0;
    unsigned long flags;
    struct ar_channel *ch = file->private_data;

    /* Add our wait queue (wait_completed) to the poll table for the kernel to use for wake up. */
    poll_wait(file, &ch->wait_completed, wait);

    /* If the completed transaction list is not empty, then a read will not block, data is available. */
    spin_lock_irqsave(&ch->lock, flags);
    if (!list_empty(&ch->completed_transactions)) {
        ret |= POLLIN | POLLRDNORM;
    }
    spin_unlock_irqrestore(&ch->lock, flags);

    return ret;
}

// Kernel 2.6.35+ simplified the ioctl interface:
// https://lwn.net/Articles/119652/
// http://opensourceforu.com/2011/08/io-control-in-linux/
static long arf_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    unsigned int nextTxLength;
    unsigned long flags;
    struct ar_transaction *tx = NULL;
    struct ar_channel *ch = file->private_data;

    switch (cmd) {

    case FIONREAD:        
        spin_lock_irqsave(&ch->lock, flags);
        tx = list_first_entry_or_null(&ch->completed_transactions, struct ar_transaction, node);        
        spin_unlock_irqrestore(&ch->lock, flags);        
        nextTxLength = (tx != NULL) ? tx->dma_completed_len : 0;
        copy_to_user((void*)arg, &nextTxLength, sizeof(nextTxLength));
        return 0;

    }
    return -EINVAL;
}

static struct file_operations arf_fileops = {
        .owner          = THIS_MODULE,
        .open           = arf_open,             ///< takes 2 or more transactions from teh free transaction list and places them in the pending list and starts them
        .release        = arf_release,          ///< terminates DMA and takes all transactions and places them in the free transaction list
        .read           = arf_read,
        .poll           = arf_poll,
        .unlocked_ioctl = arf_unlocked_ioctl
};

static int ar_chardev_create(struct ar_channel* chan)
{
        int err;
        char name[32];

        /* Dynamically allocate a number for the device.
         */
        err = alloc_chrdev_region(&chan->dev_number, 0, 1, DRIVER_NAME);
        if (err) {
                pr_err("axis-reader: Unable to allocate a device number.\n");
                return err;
        }

        /* Initialize the character device structure, and add it.
         */
        cdev_init(&chan->char_device, &arf_fileops);
        chan->char_device.owner = THIS_MODULE;
        err = cdev_add(&chan->char_device, chan->dev_number, 1);
        if (err) {
                pr_err("axis-reader: Failed to add character device.\n");
                return err;
        }

        /* Create the device node in /dev so the device is accessible as a
         * character device.
         */
        snprintf(name, 32, "axisreader%u", MINOR(chan->dev_number));
        chan->dev_entry = device_create(ar_class, NULL, chan->dev_number,
                                NULL, name);
        if (IS_ERR(chan->dev_entry)) {
                pr_err("axis-reader: Failed to create /dev character device.\n");
                err = PTR_ERR(chan->dev_entry);
                chan->dev_entry = NULL;
                return err;
        }

        return 0;
}

static void ar_chardev_destroy(struct ar_channel* chan)
{
        if (chan->dev_entry) {
                device_destroy(ar_class, chan->dev_number);
        }

        cdev_del(&chan->char_device);

        if (MAJOR(chan->dev_number) != 0 || MINOR(chan->dev_number) != 0) {
                unregister_chrdev_region(chan->dev_number, 1);
        }

        chan->dev_entry = NULL;
        chan->dev_number = MKDEV(0, 0);
}

static bool xilinx_dma_filter_s2mm(struct dma_chan* dchan, void* param)
{
        const u32 XILINX_DMA_PERIPHERAL_ID = 0x000A3500;
        const u32 match = XILINX_DMA_PERIPHERAL_ID | DMA_DEV_TO_MEM;
        u32 *peri_id = dchan->private;
        return (peri_id && (*peri_id == match));
}

static struct dma_chan* xilinx_get_dma_channel(void)
{
        dma_cap_mask_t mask;

        /* Setup the capability mask for a slave DMA channel.
         */
        dma_cap_zero(mask);
        dma_cap_set(DMA_SLAVE | DMA_PRIVATE, mask);

        /* Request the DMA channel from the DMA engine.  The channel must
         * satisfy the filter xilinx_dma_filter_s2mm().
         */
        return dma_request_channel(mask, xilinx_dma_filter_s2mm, NULL);
}

static void ar_channel_exit(struct ar_channel* chan)
{
        struct ar_transaction *tx, *next;

        /* Teriminate all DMA transactions. */
        dmaengine_terminate_all(chan->dma);

        /* Free all the transactions associated with this channel. */
        list_for_each_entry_safe(tx, next, &chan->free_transactions, node) {
                list_del(&tx->node);
                ar_transaction_destroy(tx);
        }
        list_for_each_entry_safe(tx, next, &chan->completed_transactions, node) {
                list_del(&tx->node);
                ar_transaction_destroy(tx);
        }
        list_for_each_entry_safe(tx, next, &chan->pending_transactions, node) {
                list_del(&tx->node);
                ar_transaction_destroy(tx);
        }

        if (!IS_NULL(chan->dma)) {
                dma_release_channel(chan->dma);
        }

        ar_chardev_destroy(chan);
}

static int ar_channel_init(struct ar_channel* chan)
{
        int i, err;

        chan->is_open = false;

        spin_lock_init(&chan->lock);
        init_waitqueue_head(&chan->wait_completed);
        INIT_LIST_HEAD(&chan->free_transactions);
        INIT_LIST_HEAD(&chan->pending_transactions);
        INIT_LIST_HEAD(&chan->completed_transactions);

        /* Acquire a Xilinx DMA channel.
         */
        chan->dma = xilinx_get_dma_channel();
        if (IS_NULL(chan->dma)) {
                pr_err("axis-reader: Xilinx DMA S2MM channel request failed.\n");
                return -ENODEV;
        }

        err = ar_chardev_create(chan);
        if (err) {
                ar_chardev_destroy(chan);
                return err;
        }

        /* Create some number of free transactions.
         * Must be done after chardev creation because we use the chardev device number in the CMA request.
         */
        for (i = 0; i < 4; i++) {
                struct ar_transaction *tx = ar_transaction_create(chan);
                if (IS_NULL(tx)) {
                        pr_err("axis-reader: Failed to allocate ar-transaction.\n");
                        ar_channel_exit(chan);
                        return -ENODEV;
                }
                list_add(&tx->node, &chan->free_transactions);
        }

        return 0;
}




/* Initialize the axis-reader device driver module, which includes acquiring
 * a Xilinx DMA S2MM channel (DMA_DEV_TO_MEM), creating a character device,
 * and starting the reception of packets.
 */
static int __init axis_reader_init(void)
{
        int err;

        /* Create one class for multiple channels.
         */
        ar_class = class_create(THIS_MODULE, DRIVER_NAME);
        if (IS_ERR(ar_class)) {
                pr_err("axis-reader: Failed to register device class.\n");
                return PTR_ERR(ar_class);
        }

        /* Create 1 channel with DMA and all.*/
        err = ar_channel_init(&arc0);
        if (err) {
                pr_err("axis-reader: Failed to initialize axis-reader channel.\n");
                class_destroy(ar_class);
                return err;
        }

        pr_info("axis-reader: module initialized\n");
	return 0;
}


static void __exit axis_reader_exit(void)
{
        ar_channel_exit(&arc0);
        class_destroy(ar_class);
	pr_info("axis-reader: module exited\n");
}

module_init(axis_reader_init);
module_exit(axis_reader_exit);

MODULE_AUTHOR("Ping DSP Inc.");
MODULE_DESCRIPTION("AXI-Stream Reader Driver");
MODULE_LICENSE("GPL");

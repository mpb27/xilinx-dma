# xilinx-dma-dr

This folder contains a direct-register mode driver for the Xilinx AXI DMA core.

Status: **Working and provides actual receive packet length.**

### Quick summary of how this driver works.

- Transaction descriptors are allocated using `dmaengine_prep_slave_single()` or `dmaengine_prep_slave_sg()` with `sg_len = 1`.
- Transaction descriptors are queued to a pending transactions list by `dmaengine_submit()`.
- The driver dequeues a transaction from the pending transactions list and issues it to the DMA hardware as an active transaction.
- Once the active transaction is completed (signaled by an interrupt), the transaction is added to a completed transactions list, and a new transaction is made active from the pending transactions if available.
- The `callback` function of a completed transaction is also scheduled to be called in the IRQ, but the call itself occurs in a `tasklet` some time after the interrupt.
- `dma_async_issue_pending()` should be called to make sure the driver starts pending transactions if there is no active transaction which would cause an interrupt.
- The number of transactions in the completed transactions list is limited to `XILINX_DMA_TX_HISTORY` (32), after which oldest transaction descriptors are removed and freed.
- When a transaction is submitted using `dmaengine_submit()` a cookie (integer value) is returned and can be used to query the status of the transaction using `dmaengine_tx_status()`.
- The `dmaengine_tx_status(..., &state)` call populates a `dma_tx_state` structure which has a `residue` field.
- In this driver, the `dmaegine_tx_status` can be called with the cookie of a completed transaction and will return this `residue` field, or `-1` if the transaction was not found (driver only stores the last 32 completed transactions).
- The `residue` field is the number of bytes requested minus the number of bytes actually received.


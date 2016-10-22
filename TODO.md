# Progress notes

- xilinx_dma_prep_slave_sg   

- add xilinx_dma_is_running   and is_idle  from the current Xilinx DMA driver
- potentially split all the hw accesses into their own file, like  xilinx_dma_hw.c
- add in Xilinx Pull requests for DMA
- look at [xilinx_dma_start() in interrupt calling sleep](https://forums.xilinx.com/t5/Embedded-Linux/xilinx-dma-c-sleeping-function-called-from-invalid-context/m-p/702448/highlight/true#M16252)
- do more comparisons with the current Xilinx DMA driver, especially in relation to how it operates generally, ie does it also run xilinx_dma_start() in the IRQ handler?
- consider removing all of the multi-channel DMA code for now, this will clean up the code and determine how to insert it at a later date
- check for either wrong device tree configuration or incompatible ones, a lot of people have problems with the device tree so lets make it easier to detect the problems by having the driver be more restrictive when it comes to parsing the device tree
- we could also run a set of tests against the hardware DMA to make sure it matches the device tree, suggested tests would be:
   1. Check if SG mode is really available in the hardware.
   2. Check if both the S2MM and MM2S devices are available in the hardware.
   3. Check if the DMA max length is actually 23 bits, and/or detect it and use that as the max.
   4. 


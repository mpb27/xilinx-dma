## AXI4-Stream Reader character device driver for Xilinx DMA driver.  ![License](https://img.shields.io/badge/license-GPL-blue.svg)
This driver creates a character device (/dev/axisreader0) that can be used to read complete AXI4-Stream packets.  It uses an S2MM (DMA_DEV_TO_MEM) channel provided by the **xilinx-dma-dr** DMA driver and creates a 4 packet circular buffer.  The maximum packet length is specified in bytes by the max_packet_length parameter.  The driver automatically finds the first available (not requested / taken by some other kernel module) S2MM channel and creates /dev/axisreader0.

#### Python Example

``` python

    import os
    
    # Open the character device.
    ar0 = os.open("/dev/axisreader0", os.O_RDONLY)
    
    # Read a single complete AXI4-Stream packet.
    data = os.read(ar0, 1024*1024)    # 1MB max
    
    if len(data) == 0:
        print("No AXI4-Stream packet available.")
    else:
        print("Got AXI4-Stream packet of length %d." % len(data))
    
    # Close the character device.
    os.close(ar0)
    
```

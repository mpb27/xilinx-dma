## AXI4-Stream Reader character device driver for Xilinx DMA driver.  ![License](https://img.shields.io/badge/license-GPL-blue.svg)
This driver creates a character device (/dev/axisreader0) that can be used to read complete AXI4-Stream packets.  It uses an S2MM (DMA_DEV_TO_MEM) channel provided by the **xilinx-dma-dr** DMA driver and creates a 4 packet circular buffer.  The maximum packet length is specified in bytes by the max_packet_length parameter.  The driver automatically finds the first available (not requested / taken by some other kernel module) S2MM channel and creates /dev/axisreader0.

#### Python Example (blocking)

``` python

    import os
    
    # Open the character device.
    ar0 = os.open("/dev/axisreader0", os.O_RDONLY)
    
    # Read a single complete AXI4-Stream packet.
    data = os.read(ar0, 1024*1024)    # 1MB max
        
    print("Got AXI4-Stream packet of length %d." % len(data))
    
    # Close the character device.
    os.close(ar0)
    
```

#### Python Example (non-blocking)

``` python

    import os
    import errno
    
    # Open the character device in non-blocking mode.
    ar0 = os.open("/dev/axisreader0", os.O_RDONLY | os.O_NONBLOCK)
    
    # Try to read one complete AXI4-Stream packet.
    try:
        data = os.read(ar0, 1024*1024)    # 1MB max
    except OSError as err:
        if err.errno == errno.EAGAIN:
            data = None
        else:
            raise                         # something else happened, re-raise
    
    if data is None:
        print("Nothing received.")
    else:
        print("Got AXI4-Stream packet of length %d." % len(data))

    # Close the character device.
    os.close(ar0)
    
```

# DMA Local Copy

This sample illustrates how to locally copy memory with DMA from one buffer to another on the DPU.

This sample should be run on the DPU.

### The sample logic includes:

1. Locates the DOCA device.
2. Initializes the necessary DOCA core structures.
3. Populates the DOCA memory map with two relevant buffers.
4. Allocates an element in the DOCA buffer inventory for each buffer.
5. Initializes the DOCA DMA memory copy task object.
6. Submits the DMA task.
7. Handles task completion once it is done.
8. Checks the task result.
9. Destroys all DMA and DOCA core structures.

## References
- `dma_local_copy_sample.c`
- `dma_local_copy_main.c`
- `meson.build`

# DMA Copy Host

**Note**: This sample should be run first. It is user responsibility to transfer the two configuration files (descriptor and buffer) to the DPU and provide their path to the DMA Copy DPU sample.

This sample illustrates how to allow memory copy with DMA from the x86 host into the DPU. 

This sample should be run on the host.

### The sample logic includes

1. Locates the DOCA device.
2. Initializes the necessary DOCA core structures.
3. Populates the DOCA memory map with the source buffer.
4. Exports the memory map.
5. Saves the export descriptor and local DMA buffer information into files. These files should be transferred to the DPU before running the DPU sample.
6. Waits until the DPU DMA sample has finished.
7. Destroys all DMA and DOCA core structures.

## References

- `dma_copy_host_sample.c`
- `dma_copy_host_main.c`
- `meson.build`

# DMA Copy DPU

**Note**: This sample should be run only after the DMA Copy Host sample has been run and the required configuration files (descriptor and buffer) have been copied to the DPU.

This sample illustrates how to copy memory (containing user-defined text) with DMA from the x86 host into the DPU. 

This sample should be executed on the DPU.

### The sample logic includes

1. Locates the DOCA device.
2. Initializes the necessary DOCA core structures.
3. Reads configuration files and saves their content into local buffers.
4. Allocates the local destination buffer in which the host text will be saved.
5. Populates the DOCA memory map with the destination buffer.
6. Creates the remote memory map using the export descriptor file.
7. Creates a memory map to the remote buffer.
8. Allocates an element in the DOCA buffer inventory for each buffer.
9. Initializes the DOCA DMA memory copy task object.
10. Submits the DMA task.
11. Handles task completion once it is done.
12. Checks the DMA task result.
13. If the DMA task ends successfully, prints the text that has been copied to the log.
14. Prints a log message indicating that the host-side sample can be closed.
15. Destroys all DMA and DOCA core structures.

## References

- `dma_copy_dpu_sample.c`
- `dma_copy_dpu_main.c`
- `meson.build`

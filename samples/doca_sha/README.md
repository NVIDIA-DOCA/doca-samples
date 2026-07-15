# DOCA SHA Samples

## SHA Create
This sample illustrates how to perform SHA calculation with DOCA SHA.

### Sample Logic:
1. Locating DOCA device.
2. Initializing required DOCA Core structures.
3. Setting the task pool configuration for `doca_sha_task_hash`.
4. Populating DOCA memory map with two relevant buffers.
5. Allocating element in DOCA buffer inventory for each buffer.
6. Allocating and initializing a `doca_sha_task_hash`.
7. Submitting the task.
8. Retrieving task result once it is done.

### References:
- `sha_create/sha_create_sample.c`
- `sha_create/sha_create_main.c`
- `sha_create/meson.build`

## SHA-Partial Create
This sample illustrates how to perform partial-SHA calculation for a collection of data segments with DOCA SHA.

### Sample Logic:
1. Locating DOCA device.
2. Initializing the required DOCA Core structures.
3. Setting the task pool configuration for `doca_sha_task_partial_hash`.
4. Chopping the source data into a collection of data segments according to the selected SHA algorithm's block size.
5. Populating DOCA memory map with needed buffers for all source data segments and destination buffer.
6. Allocating element in DOCA buffer inventory for the first source buffer and destination buffer.
7. Allocating and initializing a `doca_sha_task_partial_hash` with the first source buffer and the destination buffer.
8. Iteratively repeating the following sub-steps until all data segments are consumed:
   - Submitting the `doca_sha_task_partial_hash`.
   - Waiting for the submitted task to finish.
   - Allocating a `doca_buf` for the next source segment and using `doca_sha_task_partial_hash_set_src` to set it as the source buffer of the above allocated task.
   - If it is the final segment, use `doca_sha_task_partial_hash_set_is_final_buf` to mark it in the allocated task.
9. Retrieving the result of the final iteration in the destination buffer as the full partial-SHA calculation result.
10. Destroying all SHA and DOCA Core structures.

### References:
- `sha_partial_create/sha_partial_create_sample.c`
- `sha_partial_create/sha_partial_create_main.c`
- `sha_partial_create/meson.build`

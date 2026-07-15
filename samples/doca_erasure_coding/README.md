# DOCA Erasure Coding Recover Sample

This sample illustrates how to use the DOCA Erasure Coding (EC) library to encode and decode a file block (and entire file).

## Sample Logic
The sample follows a three-step process:
1. **Encoding** – Create redundancy.
2. **Deleting** – Simulate disaster.
3. **Decoding** – Recover data.

### Encoding Logic:
1. Locating a DOCA device.
2. Initializing the required DOCA Core structures, such as the progress engine (PE), memory maps, and buffer inventory.
3. Reading the source original data file and splitting it into a specified number of blocks (`<data block count>`) to the output directory.
4. Populating two DOCA memory maps with a memory range—one for the source data and one for the result.
5. Allocating buffers from the DOCA buffer inventory for each memory range.
6. Creating an EC object.
7. Connecting the EC context to the PE.
8. Setting a state change callback function for the PE:
   - Printing a log with every state change.
   - Indicating that the user may stop the progress of the PE once it is back in idle state.
9. Setting the configuration to the EC create task, including the callback functions:
   - **Successful completion callback:**
     - Writing the resulting redundancy blocks to the output directory (specified by `<redundancy block count>`).
     - Freeing the task.
     - Saving the result of the task and the callback. If there was an error, saving the relevant error value.
     - Stopping the context.
   - **Failed completion callback:**
     - Saving the result of the task and the callback.
     - Freeing the task.
     - Stopping the context.
10. Creating the EC encoding matrix by the matrix type specified in the sample.
11. Allocating and submitting the EC create task.
12. Progressing the PE until the context returns to idle state, either from a successful run where all tasks are completed or due to a fatal error.
13. Destroying all EC and DOCA Core structures.

### Deleting Logic:
1. Deleting the block files specified by `<indices of data blocks to delete>`.

### Decoding Logic:
1. Locating a DOCA device.
2. Initializing the required DOCA Core structures, such as the PE, memory maps, and buffer inventory.
3. Reading the output directory (source remaining data) and determining the block size and which blocks are missing (needing recovery).
4. Populating two DOCA memory maps with a memory range—one for the source data and one for the result.
5. Allocating buffers from the DOCA buffer inventory for each memory range.
6. Creating an EC object.
7. Connecting the EC context to the PE.
8. Setting a state change callback function for the PE:
   - Printing a log with every state change.
   - Indicating that the user may stop the progress of the PE once it is back in idle state.
9. Setting the configuration to the EC recover task, including the callback functions:
   - **Successful completion callback:**
     - Writing the resulting recovered blocks to the output directory.
     - Writing the recovered file to the output path.
     - Freeing the task.
     - Saving the result of the task and the callback. If there was an error, saving the relevant error value.
     - Stopping the context.
   - **Failed completion callback:**
     - Saving the result of the task and the callback.
     - Freeing the task.
     - Stopping the context.
10. Creating the EC encoding matrix by the matrix type specified in the sample.
11. Creating the EC decoding matrix using `doca_ec_matrix_create_recover()`, based on the encoding matrix.
12. Allocating and submitting the EC recover task.
13. Progressing the PE until the context returns to idle state, either from a successful run where all tasks are completed or due to a fatal error.
14. Destroying all DOCA EC and DOCA Core structures.

### References:
- `erasure_coding_recover_sample.c`
- `erasure_coding_recover_main.c`
- `meson.build`

# DOCA RDMA verbs Samples

## Message chaining send
This sample illustrates how to use the wait_cq_pi work request to chain messages and force order of execution.

### Sample Logic:
1. Locating DOCA device.
2. Initializing required DOCA Core structures.
3. Create two pairs of QPs, each pair connected between them. We will call them QP1, QP2, QP3 and QP4. 
4. QP1 is connected to QP2, QP3 is connected to QP4.
4. Create three buffers and populate the first with data.
5. The send flows: QP1 will send data from the first buffer to QP2, which will receive the data in the second buffer.
   QP3 will then send the data from the second buffer to QP4, which will receive the data in the third buffer. 
   In order for the data to arrive properly in the third buffer, the send operation on QP3 must be done only after the send operation on QP1 finishes successfully.
5. Post recv WRs on QP2 and QP4 to prepare for receiving the data.
6. Post a "wait_cq_pi" WR on QP3 to assure we wait with the send execution until QP1 finishes sending it's data.
7. Post the send WR on QP3 which will only be executed after the "wait_cq_pi" WR will finish waiting.
8. Post the send WR on QP1 to start the data transfer. 
9. Wait for all WRs to complete. 
10. Assure the data on the third buffer is identical to the data on the first, indicating the WRs were executed in the correct order.


### References:
- `wait_cq_wr/wait_cq_wr_sample.c`
- `wait_cq_wr/wait_cq_wr_sample.h`
- `wait_cq_wr/wait_cq_wr_main.c`
- `wait_cq_wr/meson.build`


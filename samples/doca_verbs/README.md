# DOCA Verbs Samples

## Verbs Server Client
This sample illustrates how to perform RDMA Verbs operations for server client mechanism with DOCA RDMA verbs.

### Sample Logic: 
1. Resource Setup - Verbs Context, PD, CQ, AH, QP and allocate and register memory buffers
2. Connection Establishment between server and client using TCP socket
3. RDMA Parameters Exchange - server and client exchange local buffer addresses, MKEYs, QP numbers and GID addresses
4. QP Connection - set QP attributes, Modify QP states
5. Data Path Execution - Send/Receive, Write or Read operations
6. Verify completions or received messages
7. Cleanup - Destroy QP, AH, CQ, PD, Verbs Context, close device, TCP connection and free memory buffers

### References:
- `verbs_server_client/verbs_server_client_sample.c`
- `verbs_server_client/verbs_server_client_main.c`
- `verbs_server_client/meson.build`

## Wait CQ WR
This sample illustrates how to use the wait_cq_pi work request to chain messages and force order of execution.

### Sample Logic:
1. Locating DOCA device.
2. Initializing required DOCA Core structures.
3. Create two pairs of QPs, each pair connected between them. We will call them QP1, QP2, QP3 and QP4.
4. QP1 is connected to QP2, QP3 is connected to QP4.
5. Create three buffers and populate the first with data.
6. The send flows: QP1 will send data from the first buffer to QP2, which will receive the data in the second buffer.
   QP3 will then send the data from the second buffer to QP4, which will receive the data in the third buffer.
   In order for the data to arrive properly in the third buffer, the send operation on QP3 must be done only after the send operation on QP1 finishes successfully.
7. Post recv WRs on QP2 and QP4 to prepare for receiving the data.
8. Post a "wait_cq_pi" WR on QP3 to assure we wait with the send execution until QP1 finishes sending it's data.
9. Post the send WR on QP3 which will only be executed after the "wait_cq_pi" WR will finish waiting.
10. Post the send WR on QP1 to start the data transfer.
11. Wait for all WRs to complete.
12. Assure the data on the third buffer is identical to the data on the first, indicating the WRs were executed in the correct order.

### References:
- `verbs_wait_cq_wr/verbs_wait_cq_wr_sample.c`
- `verbs_wait_cq_wr/verbs_wait_cq_wr_sample.h`
- `verbs_wait_cq_wr/verbs_wait_cq_wr_main.c`
- `verbs_wait_cq_wr/meson.build`
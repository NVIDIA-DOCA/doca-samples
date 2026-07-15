# DOCA Comch Data Path Server

**Note**: `doca_comch_data_path_high_speed_server` should be run on the BlueField Arm cores and should be started before `doca_comch_data_path_high_speed_client` is started on the host.

This sample sets up a client-server connection between the host and BlueField Arm.

The connection is used to create a producer and consumer on both sides and pass a message across the two fastpath connections.

The sample logic includes:

1. Locates the DOCA device.
2. Initializes the core DOCA structures.
3. Initializes and configures client/server contexts.
4. Initializes and configures producer/consumer contexts on top of an established connection.
5. Submits post-receive tasks for population by producers.
6. Submits send tasks from producers to write to consumers.
7. Stops and destroys producer/consumer objects.
8. Stops and destroys client/server objects.

## References

- `doca_comch/comch_data_path_high_speedserver/comch_data_path_high_speed_server_main.c`
- `doca_comch/comch_data_path_high_speedserver/comch_data_path_high_speed_server_sample.c`

# DOCA Comch Data Path Client

This sample sets up a client-server connection between the host and BlueField Arm.

The connection is used to create a producer and consumer on both sides and pass a message across the two fastpath connections.

The sample logic includes:

1. Locates the DOCA device.
2. Initializes the core DOCA structures.
3. Initializes and configures client/server contexts.
4. Initializes and configures producer/consumer contexts on top of an established connection.
5. Submits post-receive tasks for population by producers.
6. Submits send tasks from producers to write to consumers.
7. Stops and destroys producer/consumer objects.
8. Stops and destroys client/server objects.

## References

- `doca_comch/comch_data_path_high_speed_client/comch_data_path_high_speed_client_main.c`
- `doca_comch/comch_data_path_high_speedclient/comch_data_path_high_speed_client_sample.c`

# DOCA Comch Control Path Server

**Note**: `doca_comch_ctrl_path_server` must be run on the BlueField Arm side and started before `doca_comch_ctrl_path_client` is started on the host.

This sample sets up a client-server connection between the host and BlueField Arm cores.

The connection is used to pass two messages: the first sent by the client when the connection is established, and the second sent by the server upon receipt of the client's message.

The sample logic includes:

1. Locates the DOCA device.
2. Initializes the core DOCA structures.
3. Initializes and configures client/server contexts.
4. Registers tasks and events for sending/receiving messages and tracking connection changes.
5. Allocates and submits tasks for sending control path messages.
6. Handles event completions for receiving messages.
7. Stops and destroys client/server objects.

## References

- `comch_ctrl_path_server_main.c`
- `comch_ctrl_path_server_sample.c`

# DOCA Comch Control Path Client

This sample sets up a client-server connection between the host and BlueField Arm cores.

The connection is used to pass two messages: the first sent by the client when the connection is established, and the second sent by the server upon receipt of the client's message.

The sample logic includes:

1. Locates the DOCA device.
2. Initializes the core DOCA structures.
3. Initializes and configures client/server contexts.
4. Registers tasks and events for sending/receiving messages and tracking connection changes.
5. Allocates and submits tasks for sending control path messages.
6. Handles event completions for receiving messages.
7. Stops and destroys client/server objects.

## References

- `comch_ctrl_path_client_main.c`
- `comch_ctrl_path_client_sample.c`

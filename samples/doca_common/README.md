# DOCA Core Samples

## Info
All the DOCA samples described in this section are governed under the BSD-3 software license agreement.

## Progress Engine Samples
All progress engine (PE) samples use DOCA DMA because of its simplicity. PE samples should be used to understand the PE, not DOCA DMA.

### pe_common
`pe_common.c` and `pe_common.h` contain code that is used in most or all PE samples.

Users can find core code (e.g., create MMAP) and common code that uses PE (e.g., poll_for_completion).

Struct `pe_sample_state_base` (defined in `pe_common.h`) is the base state for all PE samples, containing common members that are used by most or all PE samples.

### pe_polling
The polling sample is the most basic sample for using PE. Start with this sample to learn how to use DOCA PE.

**Info**: You can diff between `pe_polling_sample.c` and any other `pe_x_sample.c` to see the unique features that the other sample demonstrates.

The sample demonstrates the following functions:
- How to create a PE
- How to connect a context to the PE
- How to allocate tasks
- How to submit tasks
- How to run the PE
- How to cleanup (e.g., destroy context, destroy PE)

**Note**: Pay attention to the order of destruction (e.g., all contexts must be destroyed before the PE).

The sample performs the following:
- Uses one DMA context.
- Allocates and submits 16 DMA tasks.

**Info**: Task completion callback checks that the copied content is valid.

- Polls until all tasks are completed.

### pe_async_stop
A context can be stopped while it still processes tasks. This stop is asynchronous because the context must complete/abort all tasks.

The sample demonstrates the following functions:
- How to asynchronously stop a context
- How to implement a context state changed callback (with regards to context moving from stopping to idle)
- How to implement task error callback (check if this is a real error or if the task is flushed)

The sample performs the following:
- Submits 16 tasks and stops the context after half of the tasks are completed.
- Polls until all tasks are complete (half are completed successfully, half are flushed).

The difference between `pe_polling_sample.c` and `pe_async_stop_sample.c` is to learn how to use PE APIs for event-driven mode.

### pe_event
Event-driven mode reduces CPU utilization (wait for event until a task is complete) but may increase latency or reduce performance.

The sample demonstrates the following functions:
- How to run the PE in event-driven mode

The sample performs the following:
- Runs 16 DMA tasks.
- Waits for event.

The difference between `pe_polling_sample.c` and `pe_event_sample.c` is to learn how to use PE APIs for event-driven mode.

### pe_multi_context
A PE can host more than one instance of a specific context. This facilitates running a single PE with multiple BlueField devices.

The sample demonstrates the following functions:
- How to run a single PE with multiple instances of a specific context

The sample performs the following:
- Connects 4 instances of DOCA DMA context to the PE.
- Allocates and submits 4 tasks to every context instance.
- Polls until all tasks are complete.

The difference between `pe_polling_sample.c` and `pe_multi_context_sample.c` is to learn how to use PE with multiple instances of a context.

### pe_reactive
PE and contexts can be maintained in callbacks (task completion and state changed).

The sample demonstrates the following functions:
- How to maintain the context and PE in the callbacks instead of the program's main function

The user must make sure to:
- Review the task completion callback and the state changed callbacks
- Review the difference between poll_to_completion and the polling loop in main

The sample performs the following:
- Runs 16 DMA tasks.
- Stops the DMA context in the completion callback after all tasks are complete.

The difference between `pe_polling_sample.c` and `pe_reactive_sample.c` is to learn how to use PE in reactive model.

### pe_single_task_cb
A DOCA task can invoke a success or error callback. Both callbacks share the same structure (same input parameters).

DOCA recommends using 2 callbacks:
- Success callback – does not need to check the task status, thereby improving performance
- Error callback – may need to run a different flow than success callback

The sample demonstrates the following functions:
- How to use a single callback instead of two callbacks

The sample performs the following:
- Runs 16 DMA tasks.
- Handles completion with a single callback.

The difference between `pe_polling_sample.c` and `pe_single_task_comp_cb_sample.c` is to learn how to use PE with a single completion callback.

### pe_task_error
Task execution may fail causing the associated context (e.g., DMA) to move to stopping state due to this fatal error.

The sample demonstrates the following functions:
- How to mitigate a task error during runtime

The user must make sure to:
- Review the state changed callback and the error callback to see how the sample mitigates context error

The sample performs the following:
- Submits 255 tasks.
- Allocates the second task with invalid parameters that cause hardware to fail.
- Mitigates the failure and polls until all submitted tasks are flushed.

The difference between `pe_polling_sample.c` and `pe_task_error_sample.c` is to learn how to mitigate context error.

### pe_task_resubmit
A task can be freed or reused after it is completed:

- Task resubmit can improve performance because the program does not free and allocate the task.
- Task resubmit can reduce memory usage (using a smaller task pool).
- Task members (e.g., source or destination buffer) can be set, so resubmission can be used if the source or destination are changed every iteration.

The sample demonstrates the following functions:
- How to re-submit a task in the completion callback
- How to replace buffers in a DMA task (similar to other task types)

The sample performs the following:
- Allocates a set of 4 tasks and 16 buffer pairs.
- Uses the tasks to copy all sources to destinations by resubmitting the tasks.

The difference between `pe_polling_sample.c` and `pe_task_resubmit_sample.c` is to learn how to use task resubmission.

### pe_task_try_submit
`doca_task_submit` does not validate task inputs (to increase performance). Developers can use `doca_task_try_submit` to validate the tasks during development.

**Note**: Task validation impacts performance and should not be used in production.

The sample demonstrates the following functions:
- How to use `doca_task_try_submit` instead of `doca_task_submit`

The sample performs the following:
- Allocates and tries to submit tasks using `doca_task_try_submit`.

The difference between `pe_polling_sample.c` and `pe_task_try_submit_sample.c` is to learn how to use `doca_task_try_submit`.

### Graph Sample
The graph sample demonstrates how to use DOCA graph with PE. The sample can be used to learn how to build and use DOCA graph.

The sample uses two nodes of DOCA DMA and one user node.

The graph runs both DMA nodes (copying a source buffer to two destinations). Once both nodes are complete, the graph runs the user node that compares the buffers.

The sample runs 10 instances of the graph in parallel.

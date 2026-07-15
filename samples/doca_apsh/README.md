# DOCA App Shield Samples

### 1. **Apsh Libs Get**
This sample demonstrates how to initialize DOCA App Shield and use its API to get the list of loadable libraries of a specific process.

**Sample Logic:**
- Opening DOCA device with DMA ability.
- Creating DOCA Apsh context.
- Setting and starting the Apsh context.
- Opening DOCA remote PCI device via given VUID.
- Creating DOCA Apsh system handler.
- Getting the list of system processes using Apsh API and searching for a specific process by PID.
- Getting the list of loadable libraries using `doca_apsh_libs_get`.
- Querying libraries for selected fields using `doca_apsh_lib_info_get`.
- Printing libraries' attributes.
- Cleaning up.

**References:**
- `apsh_libs_get/apsh_libs_get_sample.c`
- `apsh_libs_get/apsh_libs_get_main.c`
- `apsh_libs_get/meson.build`
- `apsh_common.c; apsh_common.h`

---

### 2. **Apsh Modules Get**
This sample shows how to use DOCA App Shield API to get the list of installed modules on a monitored system.

**Sample Logic:**
- Opening DOCA device with DMA ability.
- Creating DOCA Apsh context.
- Setting and starting the Apsh context.
- Opening DOCA remote PCI device via given VUID.
- Creating DOCA Apsh system handler.
- Getting system-installed modules using `doca_apsh_modules_get`.
- Querying the names of modules using `doca_apsh_module_info_get`.
- Printing module attributes.
- Cleaning up.

**References:**
- `apsh_modules_get/apsh_modules_get_sample.c`
- `apsh_modules_get/apsh_modules_get_main.c`
- `apsh_modules_get/meson.build`
- `apsh_common.c; apsh_common.h`

---

### 3. **Apsh Pslist**
This sample demonstrates how to initialize DOCA App Shield and use its API to get the list of running processes on a monitored system.

**Sample Logic:**
- Opening DOCA device with DMA ability.
- Creating DOCA Apsh context.
- Setting and starting the Apsh context.
- Opening DOCA remote PCI device via given VUID.
- Creating DOCA Apsh system handler.
- Getting the list of running processes using `doca_apsh_processes_get`.
- Querying processes for selected attributes using `doca_apsh_proc_info_get`.
- Printing process attributes.
- Cleaning up.

**References:**
- `apsh_pslist/apsh_pslist_sample.c`
- `apsh_pslist/apsh_pslist_main.c`
- `apsh_pslist/meson.build`
- `apsh_common.c; apsh_common.h`

---

### 4. **Apsh Threads Get**
This sample illustrates how to initialize DOCA App Shield and use its API to get the list of threads of a specific process.

**Sample Logic:**
- Opening DOCA device with DMA ability.
- Creating DOCA Apsh context.
- Setting and starting the Apsh context.
- Opening DOCA remote PCI device via given VUID.
- Creating DOCA Apsh system handler.
- Getting the list of processes using Apsh API and searching for a specific process with the given PID.
- Getting the list of process threads using `doca_apsh_threads_get`.
- Querying threads for selected fields using `doca_apsh_thread_info_get`.
- Printing thread attributes.
- Cleaning up.

**References:**
- `apsh_threads_get/apsh_threads_get_sample.c`
- `apsh_threads_get/apsh_threads_get_main.c`
- `apsh_threads_get/meson.build`
- `apsh_common.c; apsh_common.h`

---

### 5. **Apsh Vads Get**
This sample demonstrates how to use DOCA App Shield API to get the list of virtual address descriptors (VADs) of a specific process.

**Sample Logic:**
- Opening DOCA device with DMA ability.
- Creating DOCA Apsh context.
- Setting and starting the Apsh context.
- Opening DOCA remote PCI device via given VUID.
- Creating DOCA Apsh system handler.
- Getting the list of processes using Apsh API and searching for a specific process with the given PID.
- Getting the list of VADs using `doca_apsh_vads_get`.
- Querying VADs for selected fields using `doca_apsh_vad_info_get`.
- Printing VAD attributes.
- Cleaning up.

**References:**
- `apsh_vads_get/apsh_vads_get_sample.c`
- `apsh_vads_get/apsh_vads_get_main.c`
- `apsh_vads_get/meson.build`
- `apsh_common.c; apsh_common.h`

---

### 6. **Apsh Envars Get**
This sample illustrates how to use DOCA App Shield API to get the list of environment variables of a specific process. **Note:** This sample works only on target systems with Windows OS.

**Sample Logic:**
- Opening DOCA device with DMA ability.
- Creating DOCA Apsh context.
- Setting and starting the Apsh context.
- Opening DOCA remote PCI device via given VUID.
- Creating DOCA Apsh system handler.
- Getting the list of system processes using Apsh API and searching for a specific process with the given PID.
- Getting the list of process environment variables using `doca_apsh_envars_get`.
- Querying environment variables for selected fields using `doca_apsh_envar_info_get`.
- Printing environment variable attributes.
- Cleaning up.

**References:**
- `apsh_envars_get/apsh_envars_get_sample.c`
- `apsh_envars_get/apsh_envars_get_main.c`
- `apsh_envars_get/meson.build`
- `apsh_common.c; apsh_common.h`

---

### 7. **Apsh Privileges Get**
This sample demonstrates how to use DOCA App Shield API to get the list of privileges of a specific process. **Note:** This sample works only on target systems with Windows OS.

**Sample Logic:**
- Opening DOCA device with DMA ability.
- Creating DOCA Apsh context.
- Setting and starting the Apsh context.
- Opening DOCA remote PCI device via given VUID.
- Creating DOCA Apsh system handler.
- Getting the list of system processes using Apsh API and searching for a specific process with the given PID.
- Getting the list of process privileges using `doca_apsh_privileges_get`.
- Querying privileges for selected fields using `doca_apsh_privilege_info_get`.
- Printing privilege attributes.
- Cleaning up.

**References:**
- `apsh_privileges_get/apsh_privileges_get_sample.c`
- `apsh_privileges_get/apsh_privileges_get_main.c`
- `apsh_privileges_get/meson.build`
- `apsh_common.c; apsh_common.h`

---

### 8. **Apsh Containers Get**
This sample illustrates how to use DOCA App Shield API to get the list of running containers on a monitored system and processes within each container. **Note:** This sample works only on Linux systems.

**Sample Logic:**
- Opening DOCA device with DMA ability.
- Creating DOCA Apsh context.
- Setting and starting the Apsh context.
- Opening DOCA remote PCI device via given VUID.
- Creating DOCA Apsh system handler.
- Getting the list of containers using `doca_apsh_containers_get`.
- Querying containers for container ID using `doca_apsh_container_info_get`.
- Getting list of processes for each container using `doca_apsh_container_processes_get`.
- Printing process attributes.
- Cleaning up.

**References:**
- `apsh_containers_get/apsh_containers_get_sample.c`
- `apsh_containers_get/apsh_containers_get_main.c`
- `apsh_containers_get/meson.build`
- `apsh_common.c; apsh_common.h`

---

### 9. **Apsh Netscan Get**
This sample illustrates how to use DOCA App Shield API to get the list of open network connections on a monitored system. **Note:** This sample works only on Linux and specific Windows OS builds.

**Sample Logic:**
- Opening DOCA device with DMA ability.
- Creating DOCA Apsh context.
- Setting and starting the Apsh context.
- Opening DOCA remote PCI device via given VUID.
- Creating DOCA Apsh system handler.
- Getting the list of open connections using `doca_apsh_netscan_get`.
- Querying connection attributes using `doca_apsh_netscan_info_get`.
- Printing connection attributes.
- Cleaning up.

**References:**
- `apsh_netscan_get/apsh_netscan_get_sample.c`
- `apsh_netscan_get/apsh_netscan_get_main.c`
- `apsh_netscan_get/meson.build`
- `apsh_common.c; apsh_common.h`

---

### 10. **Apsh Process Netscan Get**
This sample shows how to get the list of open network connections for a specific process.

**Sample Logic:**
- Opening DOCA device with DMA ability.
- Creating DOCA Apsh context.
- Setting and starting the Apsh context.
- Opening DOCA remote PCI device via given VUID.
- Creating DOCA Apsh system handler.
- Getting the list of processes and searching for a specific process by PID.
- Getting process connections using `doca_apsh_process_netscan_get`.
- Querying connection attributes using `doca_apsh_netscan_info_get`.
- Printing connection attributes.
- Cleaning up.

**References:**
- `apsh_process_netscan_get/apsh_process_netscan_get_sample.c`
- `apsh_process_netscan_get/apsh_process_netscan_get_main.c`
- `apsh_process_netscan_get/meson.build`
- `apsh_common.c; apsh_common.h`

--- 

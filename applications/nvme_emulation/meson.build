#
# Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are permitted
# provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright notice, this list of
#       conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright notice, this list of
#       conditions and the following disclaimer in the documentation and/or other materials
#       provided with the distribution.
#     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
#       to endorse or promote products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
# FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

# Check for UUID library
dependency_uuid = dependency('uuid', required: false)
if not dependency_uuid.found()
	warning('Skipping compilation of DOCA Application - @0@ - Missing UUID support.'.format(APP_NAME))
	subdir_done()
endif
app_dependencies += dependency_uuid

###################
# SPDK Dependency #
###################

# Define list of SPDK & DPDK libs
spdk_libs = [
	'spdk_event',
	'spdk_event_accel',
	'spdk_event_bdev',
	'spdk_bdev',
	'spdk_bdev_aio',
	'spdk_bdev_delay',
	'spdk_bdev_error',
	'spdk_bdev_ftl',
	'spdk_bdev_gpt',
	'spdk_lvol',
	'spdk_bdev_lvol',
	'spdk_bdev_malloc',
	'spdk_bdev_null',
	'spdk_nvme',
	'spdk_bdev_nvme',
	'spdk_bdev_passthru',
	'spdk_bdev_raid',
	'spdk_bdev_split',
	'spdk_bdev_uring',
	'spdk_bdev_virtio',
	'spdk_bdev_zone_block',
	'spdk_blob_bdev',
	'spdk_blobfs_bdev',
	'spdk_rdma',
	'spdk_json',
	'spdk_rpc',
	'spdk_jsonrpc',
	'spdk_log',
	'spdk_thread',
	'spdk_util',
	'spdk_init',
	'spdk_conf',
	'spdk_trace',
	'spdk_notify',
	'spdk_sock',
	'spdk_env_dpdk',
	'spdk_dma',
	'spdk_sock_posix',
	'spdk_accel',
	'spdk_accel_mlx5',
	'spdk_bdev_crypto',
	'spdk_nvmf',
	'spdk_event_nvmf',

	# DPDK libraries used by SPDK
	'rte_eal',
	'rte_ring',
	'rte_pci',
	'rte_bus_pci',
	'rte_mempool',
	'rte_mempool_ring',
	'rte_kvargs',
	'rte_ethdev',
	'rte_cmdline',
	'rte_telemetry',
	'rte_net',
	'rte_mbuf',
	'rte_meter',
	'rte_telemetry',
	'rte_net',
	'rte_mbuf',
]

spdk_lib_path = get_option('spdk_prefix') / 'lib'
spdk_incl_path = get_option('spdk_prefix') / 'include'
spdk_dpdk_lib_path = get_option('spdk_dpdk_prefix') / 'lib'
spdk_isal_prefix = get_option('spdk_isal_prefix')

# Prepare common SPDK link arguments
spdk_link_args  = ['-Wl,-no-as-needed']
spdk_link_args += [
	'-Wl,-rpath=' + spdk_lib_path,
	'-Wl,-rpath-link=' + spdk_lib_path,
	'-Wl,-rpath=' + spdk_dpdk_lib_path,
	'-Wl,-rpath-link=' + spdk_dpdk_lib_path,
	'-L' + spdk_lib_path,
	'-L' + spdk_dpdk_lib_path,
]

# Check that all SPDK/DPDK libraries exist
foreach spdk_lib : spdk_libs
	if c_compiler.find_library(spdk_lib, dirs : [spdk_lib_path, spdk_dpdk_lib_path], disabler : true, required : false).found()
		spdk_link_args += ['-l' + spdk_lib]
	else
		warning('Skipping compilation of DOCA Application - @0@ - Failed to find @1@ required by SPDK.'.format(APP_NAME, spdk_lib))
		subdir_done()
	endif
endforeach

# Check that SPDK ISA-L library exists
if run_command('[', '-f', spdk_isal_prefix + '/libisal.a', ']').returncode() == 0
	spdk_link_args += [
		'-Wl,-u,needed_symbol',
		spdk_isal_prefix + '/libisal.a',
		'-Wl,--no-whole-archive',
	]
else
	warning('Skipping compilation of DOCA Application - @0@ - Failed to find SPDK ISA-L required by SPDK.'.format(APP_NAME))
	subdir_done()
endif

# Prepare SPDK include and compiler flags
spdk_inc_dirs = include_directories(spdk_incl_path)
spdk_args = []
if c_compiler.has_header_symbol('spdk/jsonrpc.h', 'spdk_jsonrpc_set_log_file', include_directories : [spdk_inc_dirs])
	spdk_args += ['-DHAVE_SPDK_JSONRPC_SET_LOG_FILE=1']
endif

# Declare SPDK dependency
spdk_dependency = declare_dependency(link_args: spdk_link_args, include_directories: spdk_inc_dirs, compile_args: spdk_args)

if is_host
	warning('@0@ is only supported on the DPU, and is not supported on the Host'.format(APP_NAME))
endif

vanilla_app_srcs = [
	APP_NAME + '.c',
]

app_srcs += [
	# SPDK External NVMf Trasnport Sources
	'host/doca_transport.c',
	'host/nvmf_doca_io.c',
	# SPDK External RPC Sources
	'host/nvmf_rpc.c',
	# PCI common
	'host/nvme_pci_common.c'
]

app_inc_dirs += include_directories('common')

#########################
# build DPA device code #
#########################

dependency_dpa = dependency('doca-dpa')

# Find DPA build script
device_build_script_path = meson.current_source_dir() / 'build_device_code.sh'
device_build_script = find_program(device_build_script_path)

device_binary_name = 'nvmf_doca_transport_device'
dpa_app_name = 'nvmf_doca_transport_app'

app_device_srcs = files([
	'device/doca_transport_dev.c',
])

app_attributes_file = files(['device/doca_transport_attributes.yaml'])

# Prepare include flags
dpa_include_dir = dependency_dpa.get_variable(pkgconfig : 'includedir')
dpa_device_include_flags = [
	'-I' + dpa_include_dir,
	'-I' + meson.current_source_dir() / 'common',
]

# Prepare link flags
dpa_lib_dir = dependency_dpa.get_variable(pkgconfig : 'libdir')
dpa_device_link_flags = '-L' + dpa_lib_dir
dpa_dev_device_lib_names = ['dpa_dev', 'dpa_dev_comm']
foreach dpa_dev_device_lib_name : dpa_dev_device_lib_names
	dpa_device_link_flags += ' -ldoca_' + dpa_dev_device_lib_name
endforeach

if not get_option('dpacc_mcpu').contains('nv-dpa-bf3') 
	warning('Skipping compilation of DOCA Application - @0@ - only BF3 compilation is supported.'.format(APP_NAME))
	subdir_done()
endif

if get_option('dpacc_mcpu') != 'nv-dpa-bf3'
	warning('DOCA Application @0@ supports compilation only for BF3 - ignoring other architectures.'.format(APP_NAME))
endif

nvme_emu_dpacc_mcpu_flag = 'nv-dpa-bf3'

# Invoke device compilation script
dpa_code = custom_target(
	device_binary_name,
	input: app_device_srcs,
	command: [
		device_build_script,
		' '.join(dpa_device_include_flags),
		'@OUTDIR@',
		nvme_emu_dpacc_mcpu_flag,
		dpa_device_link_flags,
		dpa_app_name,
		app_attributes_file,
		'@OUTPUT@',
		'@INPUT@',
	],
	depfile: '@BASENAME@.d',
	output: [device_binary_name + '.a']
)

executable(DOCA_PREFIX + APP_NAME,
	   vanilla_app_srcs + app_srcs + dpa_code,
	   c_args : base_c_args,
	   dependencies : app_dependencies + spdk_dependency,
	   include_directories : app_inc_dirs,
	   install: install_apps)

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

# Install public version of DPA build script
device_code_build_script_name = 'build_device_code.sh'
device_code_build_script = meson.current_source_dir() / device_code_build_script_name
app_install_excludes += device_code_build_script_name + public_file_suffix
install_data(sources : device_code_build_script + public_file_suffix, rename : [device_code_build_script_name],
		install_dir : app_install_dir)
app_install_started = true

# Check if we should skip the compilation of the applications
if flag_disable_applications_compilation
	subdir_done()
endif

# Check for UUID library
dependency_uuid = dependency('uuid', required: false)
if not dependency_uuid.found()
	set_variable('app_' + APP_NAME + '_disable_reason', 'Missing UUID support')
	subdir_done()
endif
app_dependencies += dependency_uuid

# Create SPDK dependency
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

nvme_spdk_lib_path = get_option('nvme_emulation_spdk_prefix') / 'lib'
nvme_spdk_incl_path = get_option('nvme_emulation_spdk_prefix') / 'include'
nvme_dpdk_lib_path = get_option('nvme_emulation_dpdk_prefix') / 'lib'
nvme_isal_prefix = get_option('nvme_emulation_isal_prefix')

nvme_spdk_link_args  = ['-Wl,-no-as-needed']
nvme_spdk_link_args += [
	'-Wl,-rpath=' + nvme_spdk_lib_path,
	'-Wl,-rpath-link=' + nvme_spdk_lib_path,
	'-Wl,-rpath=' + nvme_dpdk_lib_path,
	'-Wl,-rpath-link=' + nvme_dpdk_lib_path,
	'-L' + nvme_spdk_lib_path,
	'-L' + nvme_dpdk_lib_path,
]
foreach spdk_lib : spdk_libs
	if c_compiler.find_library(spdk_lib, dirs : [nvme_spdk_lib_path, nvme_dpdk_lib_path], disabler : true, required : false).found()
		nvme_spdk_link_args += ['-l' + spdk_lib]
	else
		set_variable('app_' + APP_NAME + '_disable_reason', 'Missing SPDK library ' + spdk_lib)
		subdir_done()
	endif
endforeach

spdk_args = []

if c_compiler.has_header_symbol('spdk/jsonrpc.h', 'spdk_jsonrpc_set_log_file', include_directories : [include_directories(nvme_spdk_incl_path)])
	spdk_args += ['-DHAVE_SPDK_JSONRPC_SET_LOG_FILE=1']
endif

if run_command('[', '-f', nvme_isal_prefix + '/libisal.a', ']').returncode() == 0
	nvme_spdk_link_args += [
		'-Wl,-u,needed_symbol',
		nvme_isal_prefix + '/libisal.a',
		'-Wl,--no-whole-archive',
	]
else
	set_variable('app_' + APP_NAME + '_disable_reason', 'Missing SPDK ISA-L library')
	subdir_done()
endif

spdk_inc_dirs = include_directories(nvme_spdk_incl_path)
spdk_depends = declare_dependency(link_args : nvme_spdk_link_args, include_directories : spdk_inc_dirs, compile_args: spdk_args)

app_srcs = files([
	APP_NAME + '.c',
	# SPDK External NVMf Transport Sources
	'host/doca_transport.c',
	'host/nvmf_doca_io.c',
	# SPDK External RPC Sources
	'host/nvmf_rpc.c',
	# PCI common
	'host/nvme_pci_common.c'
])

# Build DPA device code
# Add necessary flags
dpa_device_cc_extra_flags = ''
dpa_host_cc_extra_flags = ''

if get_option('buildtype') == 'debug'
	dpa_device_cc_extra_flags += ' -DDOCA_DEBUG -g'
endif

# Prepare common DPA compiler arguments
dpa_app_name = 'nvmf_doca_transport_app'
dpa_binary_name = 'nvmf_doca_transport_device'
dpa_device_link_flags = ''
dpa_device_include_flags = [
	'-I' + doca_root_dir / 'applications/nvme_emulation/common',
]
app_dpa_device_depends = []
dpa_dev_device_lib_names = ['dpa_dev', 'dpa_dev_comm']
if flag_amalgamation_build
	app_dpa_device_depends += doca_dpa_runtime_lib
	foreach dpa_dev_device_lib_name : dpa_dev_device_lib_names
		dpa_device_lib_info = get_variable('doca_' + dpa_dev_device_lib_name + '_dev_lib_info')
		dpa_device_link_flags += ' ' + dpa_device_lib_info.get_variable('dev_link_args')
		dpa_device_include_flags += dpa_device_lib_info.get_variable('dev_include_args')
	endforeach
	dpa_device_include_flags += [
		'-I' + doca_root_dir / 'libs/doca_common/version',
		'-I' + doca_root_dir / 'libs/doca_common/core/include/public',
	]
else
	dpa_device_link_flags = '-L/opt/mellanox/doca' / get_option('libdir')
	foreach dpa_dev_device_lib_name : dpa_dev_device_lib_names
		dpa_device_link_flags += ' -ldoca_' + dpa_dev_device_lib_name
	endforeach
	dpa_device_include_flags += [
		'-I/opt/mellanox/doca/include',
	]
endif

# Application device source code files to be compiled by the DPACC
app_device_srcs = files([
	'device/doca_transport_dev.c',
])

# Attributes file to be used
app_device_attributes_arg = meson.current_source_dir() + '/device/doca_transport_attributes.yaml'

# Run DPA compiler
app_srcs += custom_target(
	dpa_binary_name,
	input: app_device_srcs,
	command: [
		dpa_build_script,
		' '.join(dpa_device_include_flags),
		dpa_host_cc_extra_flags,
		dpa_device_cc_extra_flags,
		'@SOURCE_ROOT@',
		'@OUTDIR@',
		'application-attributes',
		bf3_dpacc_mcpu_flag,
		dpa_device_link_flags,
		dpa_app_name,
		app_device_attributes_arg,
		'@OUTPUT@',
		'@INPUT@',
	],
	depends: app_dpa_device_depends,
	depfile: '@BASENAME@.d',
	output: [dpa_binary_name + '.a']
)

app_inc_dirs += include_directories('common')

executable(DOCA_PREFIX + APP_NAME, app_srcs,
	c_args : [apps_common_c_args],
	dependencies : [app_dependencies, spdk_depends],
	include_directories: app_inc_dirs)

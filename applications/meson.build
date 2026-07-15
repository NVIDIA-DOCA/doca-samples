#
# Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

project('DOCA_APPLICATIONS', 'C', 'CPP',
	# Get version number from file.
	version: run_command(find_program('cat'), ('../VERSION'), check: true).stdout().strip(),
	license: 'BSD-3',
	default_options: [
		'buildtype=debug',
		'cpp_std=c++11'
	],
	meson_version: '>= 0.61.2'
)

DOCA_PREFIX = 'doca_'
GPU_SUFFIX = '_gpu'

#################################
# Basic Compilation Definitions #
#################################

languages = ['c', 'cpp']

base_c_args = [
	'-Wno-missing-braces',
	'-Wno-missing-field-initializers',
]

base_cpp_args = [
	'-Wno-missing-field-initializers',
]

install_apps = false
app_install_dir = ''

######################
# CUDA / GPU Support #
######################

# Start optimistic, and check what we have available
flag_enable_gpu_support = true

# Check #1 - Start by searching for "nvlink" to avoid adding the language for nothing (see note below)
nvlink = find_program('nvlink', required : false)
if not nvlink.found()
	warning('CUDA was not found (nvlink) - Disabling GPU support!')
	flag_enable_gpu_support = false
endif

# Check #2 - Only really activate it if CUDA is present
if flag_enable_gpu_support
	add_languages('CUDA')
	import('unstable-cuda')
	# CUDA can only be found by meson (if exists) after we import it...
	cuda_dep = dependency('cuda', modules: ['cudart', 'cuda'], required: false)
	if not cuda_dep.found()
		warning('CUDA was not found (dependency) - Disabling GPU support!')
		flag_enable_gpu_support = false
	else
		cuda_version = cuda_dep.version()
		if not cuda_version.version_compare('>= 12-2')
			warning('CUDA version ' + cuda_version + ' is lower than the expected version of 12.2 - Disabling GPU support!')
			flag_enable_gpu_support = false
		endif
	endif
endif

# CUDA is available, now activate it
if flag_enable_gpu_support
	# Enables ifdef for GPU code
	gpu_compile_flags = ['-DGPU_SUPPORT']

	languages += ['cuda']
	cuda = import('unstable-cuda')
	nvcc = meson.get_compiler('cuda')

	nvcc_flags = []
	nvcc_flags += ['-gencode', 'arch=compute_80,code=sm_80']
	nvcc_flags += ['-gencode', 'arch=compute_90,code=sm_90']
	if cuda_version.version_compare('>= 12-8')
		nvcc_flags += ['-gencode', 'arch=compute_100,code=sm_100']
	endif
	add_project_arguments(nvcc_flags, language: 'cuda')

	# Refer to https://mesonbuild.com/Cuda-module.html
	add_project_arguments('-forward-unknown-to-host-compiler', language: 'cuda')
	if get_option('enable_cross_compilation_to_dpu')
		add_project_link_arguments(['-forward-unknown-to-host-compiler', '--allow-shlib-undefined'], language : 'cuda')
	endif
	add_project_arguments('-rdc=true', language: 'cuda')

	gpu_dependencies = [cuda_dep]

	nvcc_flags_link = []
	nvcc_flags_link += ['-gencode=arch=compute_80,code=sm_80']
	nvcc_flags_link += ['-gencode=arch=compute_90,code=sm_90']
	if cuda_version.version_compare('>= 12-8')
		nvcc_flags_link += ['-gencode=arch=compute_100,code=sm_100']
	endif

	add_project_link_arguments(nvcc_flags_link, language: 'cuda')

	# Update the compilation flags
	gpu_c_args = base_c_args + gpu_compile_flags
	gpu_cpp_args = base_cpp_args + gpu_compile_flags
	gpu_cuda_args = gpu_compile_flags
endif

################
# gRPC Support #
################

# Start optimistic, and check what we have available
flag_enable_grpc_support = true

# Check for protoc
protoc = find_program('protoc', required: false)
if not protoc.found()
	warning('Failed to find an up-to-date gRPC compiler (protoc) - Disabling gRPC support!')
	flag_enable_grpc_support = false
endif

# Check for C++ plugin
if flag_enable_grpc_support
	grpc_cpp_plugin = find_program('grpc_cpp_plugin', required: false)
	if not grpc_cpp_plugin.found()
		warning('Failed to find a gRPC C++ plugin - Disabling gRPC support!')
		flag_enable_grpc_support = false
	endif
endif

# Check for Python plugin
if flag_enable_grpc_support
	grpc_py_plugin = find_program('grpc_python_plugin', required: false)
	if not grpc_py_plugin.found()
		warning('Failed to find a gRPC Python plugin (grpc_python_plugin) - Disabling gRPC support!')
		flag_enable_grpc_support = false
	endif
endif

# Check for protobuf
if flag_enable_grpc_support
	dependency_protobuf = dependency('protobuf', required: false)
	if not dependency_protobuf.found()
		warning('Failed to find protobuf (gRPC) - Disabling gRPC support!')
		flag_enable_grpc_support = false
	endif
endif

# Check for gRPC itself
if flag_enable_grpc_support
	# In later versions/homebrew versions, the .pc files only work out-of-the-box when using static linking
	if get_option('upstream_grpc')
		dependency_grpc = dependency('grpc++', required: false)
	else
		dependency_grpc = dependency('grpc++', static: true, required: false)
	endif
	if not dependency_grpc.found()
		warning('Failed to find gRPC - Disabling gRPC support!')
		flag_enable_grpc_support = false
	endif
endif

# gRPC is available, now activate it
if flag_enable_grpc_support
	grpc_dependencies = []
	grpc_dependencies += dependency_protobuf
	grpc_dependencies += dependency_grpc

	# We get FP warnings about gRPC's own code / generated code
	grpc_warnings_disabling = [
		'-Wno-deprecated-declarations',
		'-Wno-missing-declarations',
		'-Wno-redundant-decls',
		'-Wno-null-dereference',
		'-Wno-array-bounds',
	]

	grpc_cpp_warnings_disabling = grpc_warnings_disabling + [
		'-fno-permissive',
	]

	grpc_c_args = base_c_args + grpc_warnings_disabling
	grpc_cpp_args = base_cpp_args + grpc_cpp_warnings_disabling + ['-std=c++17']
	grpc_link_args = []

	# Workaround build issue in older versions
	if dependency_grpc.version().version_compare('< 1.28.0')
		grpc_link_args += ['-lgrpc']
	endif

	# Hack to force the generation of non-executable generated files
	python_generator = find_program('cp')
endif

################
# DOCA Drivers #
################

doca_name_to_pkgconfig_name = {
	'dpdk': 'libdpdk',
	'flexio': 'libflexio',
	'ucx': 'ucx',
}

# DPACC Support
flag_enable_dpa_support = true
dpacc_mcpu_flag = get_option('dpacc_mcpu')
dpacc_dependencies = ['dpa', 'pcc', 'flexio']
dpacc = find_program('dpacc', dirs: ['/opt/mellanox/doca/tools'], required: false)
if not dpacc.found()
	warning('Missing DPACC tool needed by DOCA DPA, DOCA PCC and FlexIO')
	flag_enable_dpa_support = false
endif
if get_option('enable_cross_compilation_to_dpu')
	warning('DPACC tool is not support during cross compilation')
	flag_enable_dpa_support = false
endif

##############################
# Advanced Compilation Flags #
##############################

# Comment this line to restore warnings of experimental DOCA features
add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: languages)

if get_option('enable_deprecated_features')
	# Comment this line to restore warnings of deprecated DOCA features
	add_project_arguments('-D DOCA_ALLOW_DEPRECATED_API', language: languages)
endif

if get_option('enable_trace_log')
	add_project_arguments('-D DOCA_LOGGING_ALLOW_TRACE', language: languages)
endif

if get_option('enable_cross_compilation_to_dpu')
	# Please update this folder if the base cross-compilation folder is located elsewhere
	add_project_arguments('-I/root/doca-cross/usr/include', language: languages)
endif

# Resolve irrelevant compiler warnings
add_project_arguments('-Wno-format-zero-length', language: languages)

base_app_dependencies = []
base_app_dependencies += dependency('threads')

# In old distributions it was in libbsd, in newer ones it is part of libc
c_compiler = meson.get_compiler('c')
if c_compiler.has_function('strlcpy', prefix: '#include <string.h>', args: ['-D_GNU_SOURCE'])
	add_project_arguments('-D DOCA_USE_LIBC_STRING_L_FUNC', language: languages)
else
	dependency_libbsd = dependency('libbsd', required: false)
	if dependency_libbsd.found()
		base_app_dependencies += dependency_libbsd
		add_project_arguments('-D DOCA_USE_LIBBSD', language: languages)
		# Ensure mlnx-dpdk will manage to find our libbsd if exists
		add_project_arguments('-D RTE_USE_LIBBSD', language: languages)
	endif
endif

# Learn from our own config file if we are installed on the DPU or the Host
dependency_doca = dependency('doca-common')
if c_compiler.has_header_symbol('doca_build_config.h', 'DOCA_ARCH_DPU', dependencies: dependency_doca)
        is_dpu = true
else
	is_dpu = false
endif
is_host = not is_dpu

common_path = 'common'
common_dir_path = '../' + common_path
base_app_inc_dirs = [
	include_directories(common_path),
	include_directories('..'),
]
samples_dir_path = '../../samples'

app_list = [
	    'app_shield_agent',
	    'bifurcated_driver_model',
	    'dma_copy',
	    'dpa_all_to_all',
	    'dpu_gpu_remote_offload',
	    'eth_l2_fwd',
	    'file_compression',
	    'file_integrity',
	    'gpu_packet_processing',
	    'ip_frag',
	    'ipsec_security_gw',
	    'nvme_emulation',
	    'pcc',
	    'psp_gateway',
	    'secure_channel',
	    'simple_fwd_vnf',
	    'sta_offload',
	    'storage',
	    'stream_receive_perf',
	    'switch',
	    'upf_accel',
	    'urom_rdmo',
	    'virtiofs',
	    'yara_inspection',
]

fs = import('fs')
foreach APP_NAME : app_list
	# Some apps are only present on a subset of installations
	if not fs.is_dir(APP_NAME)
		continue
	endif
	if (not get_option('enable_' + APP_NAME) and not get_option('enable_all_applications'))
		warning('Skipping compilation of DOCA Application - @0@.'.format(APP_NAME))
		continue
	endif

	# Basic app definitions
	app_dependencies = base_app_dependencies
	app_doca_dependencies = []
	app_driver_dependencies = []
	app_doca_depends = []
	app_driver_depends = []
	app_inc_dirs = base_app_inc_dirs
	app_srcs = []

	######################################
	# Check dependencies before starting #
	######################################

	if fs.is_dir(APP_NAME + '/' + 'dependencies')
		subdir(APP_NAME + '/' + 'dependencies')
	endif

	# Everything should use common (no need for all apps to add it manually)
	app_doca_depends += 'common'

	# DOCA Libraries
	foreach doca_dep : app_doca_depends
		cur_dep = dependency('doca-' + doca_dep.replace('_', '-'), required: false)
		if not cur_dep.found()
			warning('Skipping compilation of DOCA Application - @0@ - Missing DOCA library @1@'.format(APP_NAME, DOCA_PREFIX + doca_dep))
			break
		endif
		# DPA Check
		if not flag_enable_dpa_support and dpacc_dependencies.contains(doca_dep)
			warning('Skipping compilation of DOCA Application - @0@ - Missing DPACC support'.format(APP_NAME))
			break
		endif
		app_doca_dependencies += cur_dep
	endforeach

	# Skip if something is missing
	if app_doca_dependencies.length() != app_doca_depends.length()
		continue
	endif

	# DOCA Drivers
	foreach driver_dep : app_driver_depends
		cur_dep = dependency(doca_name_to_pkgconfig_name[driver_dep], required: false)
		if not cur_dep.found()
			warning('Skipping compilation of DOCA Application - @0@ - Missing DOCA driver @1@'.format(APP_NAME, driver_dep))
			break
		endif
		# DPA Check
		if not flag_enable_dpa_support and dpacc_dependencies.contains(driver_dep)
			warning('Skipping compilation of DOCA Application - @0@ - Missing DPACC support'.format(APP_NAME))
			break
		endif
		app_driver_dependencies += cur_dep
	endforeach

	# Skip if something is missing
	if app_driver_dependencies.length() != app_driver_depends.length()
		continue
	endif

	# Pass on the new information
	app_dependencies += app_doca_dependencies
	app_dependencies += app_driver_dependencies

	#########################
	# Build the Application #
	#########################

	# Enter the application's directory
	subdir(APP_NAME)

endforeach

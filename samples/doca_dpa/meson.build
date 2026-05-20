#
# Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

samples = [
	'dpa_kernel_launch',
	'dpa_basic_initiator_target',
	'dpa_initiator_target',
	'dpa_ping_pong',
	'dpa_nvqual',
	'dpa_verbs_initiator_target'
]

sample_lib_srcs = files([
	'dpa_common.c',
])

sample_lib_hdrs = files([
	'dpa_common.h',
])

# We need to install the public version of this script on our own
dpacc_build_script_name = 'build_dpacc_samples.sh'
install_data(sources : dpacc_build_script_name + public_file_suffix, rename : [dpacc_build_script_name],
		install_dir : sample_lib_install_dir)

# No need to set compilation flags if we are not going to compile anything
if flag_disable_samples_compilation
	subdir_done()
endif

sample_lib_includes += include_directories('.')

# Add necessary flags
dpa_device_cc_extra_flags = ''
dpa_host_cc_extra_flags = ''

if build_debug_mode
	dpa_device_cc_extra_flags += ' -DDOCA_DEBUG -g'
endif

# Prepare common DPA compiler arguments
sample_dpa_app_name = 'dpa_sample_app'
dpa_device_link_flags = ''
dpa_device_include_flags = []
sample_dpa_device_depends = []
dpa_dev_device_lib_names = ['dpa_dev', 'dpa_dev_comm', 'dpa_dev_verbs']
if flag_amalgamation_build
	sample_dpa_device_depends += doca_dpa_runtime_lib
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

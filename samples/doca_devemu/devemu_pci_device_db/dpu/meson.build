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

sample_srcs = files([
	'/'.join(['host', SAMPLE_NAME + '_dpu_sample.c']),
])

# Needed because we use common DOCA-Wide utilities in the main
common_dir_path = '/'.join([meson.project_source_root(), 'common', 'public'])

sample_main_srcs = files([
	SAMPLE_NAME + '_dpu_main.c',
])

sample_program_name = SAMPLE_NAME + '_program'

if skip_sample_compilation
	subdir_done()
endif

# Sample device source code absolute path to be compiled by the DPACC
sample_device_srcs_arg = files(['device' / SAMPLE_NAME + '_dpu_kernels_dev.c'])

# Attributes file to be used
sample_device_attributes_arg = meson.current_source_dir() + '/device/' + SAMPLE_NAME + '_attributes.yaml'

# Run DPA compiler
sample_srcs += custom_target(
	sample_program_name,
	input: sample_device_srcs_arg,
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
		sample_dpa_app_name,
		sample_device_attributes_arg,
		'@OUTPUT@',
		'@INPUT@',
	],
	depends: sample_dpa_device_depends,
	depfile: '@BASENAME@.d',
	output: [sample_program_name + '.a']
)

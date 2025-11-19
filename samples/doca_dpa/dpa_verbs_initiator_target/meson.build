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

project('DOCA_SAMPLE', 'C',
	# Get version number from file.
	version: run_command(find_program('cat'),
		files('../../../VERSION'), check: true).stdout().strip(),
	license: 'BSD-3',
	default_options: ['buildtype=debug'],
	meson_version: '>= 0.61.2'
)

SAMPLE_NAME = 'dpa_verbs_initiator_target'
sample_program_name = SAMPLE_NAME + '_program'

# Comment this line to restore warnings of experimental DOCA features
add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: ['c'])

sample_dependencies = []
# Required for all DOCA programs
sample_dependencies += dependency('doca-common')
# The DOCA library of the sample itself
sample_dependencies += dependency('doca-dpa')
# Additional DOCA library that is relevant for this sample
sample_dependencies += dependency('doca-rdma')
# Additional DOCA library that is relevant for this sample
sample_dependencies += dependency('doca-verbs')
# Utility DOCA library for executables
sample_dependencies += dependency('doca-argp')
# FlexIO definitions - Required by DPACC
sample_dependencies += dependency('libflexio')
# libibverbs
sample_dependencies += dependency('libibverbs')
sample_dependencies += dependency('libmlx5')

sample_srcs = [
	# The sample itself
	'host/' + SAMPLE_NAME + '_sample.c',
	# Main function for the sample's executable
	SAMPLE_NAME + '_main.c',
	# Common code for the DOCA library samples
	'../dpa_common.c',
	# Common code for the DOCA samples
	'../../common.c',
]

sample_inc_dirs  = []
# Common DOCA library logic
sample_inc_dirs += include_directories('..')
sample_inc_dirs += include_directories('../..')

# DPA compiler build script
dpacc_build_script_name = 'build_dpacc_samples.sh'
dpacc_build_script = meson.current_source_dir() + '/../' + dpacc_build_script_name
# Sample device source code absolute path to be compiled by the DPACC
sample_device_src = meson.project_source_root() + '/device/' +  SAMPLE_NAME + '_kernels_dev.c'
sample_manifest = meson.project_source_root() + '/device/' +  SAMPLE_NAME + '_attributes.yaml'
doca_dep = dependency('doca-dpa')
doca_lib_dir = doca_dep.get_variable(pkgconfig : 'libdir')
dpacc_mcpu_flag = 'nv-dpa-bf3,nv-dpa-cx7,nv-dpa-cx8'

# Run DPA compiler
run_command(dpacc_build_script, meson.project_build_root(), sample_device_src, SAMPLE_NAME,
	    sample_program_name, dpacc_mcpu_flag, doca_lib_dir, sample_manifest, check: true)

# Dependencies
sample_device_build_dir = meson.project_build_root() + '/' + SAMPLE_NAME + '/device/build_dpacc'
dpa_program_lib = meson.get_compiler('c').find_library(sample_program_name, dirs : sample_device_build_dir,
							static: true)

sample_dependencies += dpa_program_lib

executable('doca_' + SAMPLE_NAME, sample_srcs,
	dependencies : sample_dependencies,
	include_directories: sample_inc_dirs,
	install: false)

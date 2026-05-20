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

project('DOCA_SAMPLE', 'C', 'CPP',
	# Get version number from file.
	version: run_command(find_program('cat'),
		files('../../../../VERSION'), check: true).stdout().strip(),
	license: 'BSD-3',
	default_options: ['buildtype=debug'],
	meson_version: '>= 0.61.2'
)

SAMPLE_NAME = 'devemu_pci_device_db'
sample_program_name = SAMPLE_NAME + '_program'

# Comment this line to restore warnings of experimental DOCA features
add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: ['c', 'cpp'])

sample_dependencies = []
# Required for all DOCA programs
sample_dependencies += dependency('doca-common')
# The DOCA library of the sample itself
sample_dependencies += dependency('doca-devemu')
# Utility DOCA library for executables
sample_dependencies += dependency('doca-argp')
# FlexIO definitions - Required by DPACC
sample_dependencies += dependency('libflexio')
# Required for running DPA
dependency_dpa = dependency('doca-dpa')
sample_dependencies += dependency_dpa

sample_srcs = [
	# The sample itself
	'host/' + SAMPLE_NAME + '_dpu_sample.c',
	# Main function for the sample's executable
	SAMPLE_NAME + '_dpu_main.c',
	# Common code for the DOCA library samples
	'../../devemu_pci_common.c',
	# Common code for all DOCA samples
	'../../../common.c',
]

sample_inc_dirs  = []
# Common DOCA library logic
sample_inc_dirs += include_directories('../..')
# Common DOCA logic (samples)
sample_inc_dirs += include_directories('../../..')
# Common DOCA logic
sample_inc_dirs += include_directories('../../../..')
# Common DOCA logic (applications)
sample_inc_dirs += include_directories('../../../../applications/common/')

# DPA compiler build script
dpacc_build_script_name = 'build_dpacc_samples.sh'
dpacc_build_script = meson.current_source_dir() + '/../../' + dpacc_build_script_name
# Sample device source code absolute path to be compiled by the DPACC
sample_device_src = meson.project_source_root() + '/device/' +  SAMPLE_NAME + '_dpu_kernels_dev.c'
sample_attributes = meson.project_source_root() + '/device/' +  SAMPLE_NAME + '_attributes.yaml'

dpa_lib_dir = dependency_dpa.get_variable(pkgconfig : 'libdir')
dpa_include_dir = dependency_dpa.get_variable(pkgconfig : 'includedir')

dpacc_mcpu_flag = 'nv-dpa-bf3'

# Run DPA compiler
run_command(dpacc_build_script, meson.project_build_root(), dpa_lib_dir, dpa_include_dir, sample_device_src, SAMPLE_NAME,
	    sample_program_name, dpacc_mcpu_flag, sample_attributes, check: true)

# Dependencies
sample_device_build_dir = meson.project_build_root() + '/' + SAMPLE_NAME + '/dpu/device/build_dpacc'
dpa_program_lib = meson.get_compiler('c').find_library(sample_program_name, dirs : sample_device_build_dir,
							static: true)

sample_dependencies += dpa_program_lib

executable('doca_' + SAMPLE_NAME + '_dpu', sample_srcs,
	c_args : '-Wno-missing-braces',
	dependencies : sample_dependencies,
	include_directories: sample_inc_dirs,
	install: false)

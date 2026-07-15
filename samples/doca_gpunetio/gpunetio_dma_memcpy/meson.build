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

project('DOCA_SAMPLE', 'C', 'CPP', 'CUDA',
	# Get version number from file.
	version: run_command(find_program('cat'),
		files('../../../VERSION'), check: true).stdout().strip(),
	license: 'BSD-3',
	default_options: ['buildtype=release'],
	meson_version: '>= 0.61.2'
)

SAMPLE_NAME = 'gpunetio_dma_memcpy'

# Comment this line to restore warnings of experimental DOCA features
add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: ['c', 'cpp', 'cuda'])

sample_dependencies = []
# Required for all DOCA programs
sample_dependencies += dependency('doca-common')
# The DOCA library of the sample itself
doca_gpu_dep = dependency('doca-gpunetio')
sample_dependencies += doca_gpu_dep
# Utility DOCA library for executables
sample_dependencies += dependency('doca-argp')
# Additional DOCA library that is relevant for this sample
sample_dependencies += dependency('doca-dma')

cuda = import('unstable-cuda')
nvcc = meson.get_compiler('cuda')

cuda_dep = dependency('cuda', version: '>= 12-1', modules: ['cudart', 'cuda'])
cuda_version = cuda_dep.version()

nvcc_flags = []
if cuda_version.version_compare('< 13-0')
	nvcc_flags += ['-gencode', 'arch=compute_70,code=sm_70']
endif
nvcc_flags += ['-gencode', 'arch=compute_75,code=sm_75']
nvcc_flags += ['-gencode', 'arch=compute_80,code=sm_80']
nvcc_flags += ['-gencode', 'arch=compute_90,code=sm_90']
if cuda_version.version_compare('>= 12-8')
	nvcc_flags += ['-gencode', 'arch=compute_100,code=sm_100']
endif
add_project_arguments(nvcc_flags, language: 'cuda')

# Refer to https://mesonbuild.com/Cuda-module.html
add_project_arguments('-forward-unknown-to-host-compiler', language: 'cuda')
add_project_arguments('-rdc=true', language: 'cuda')

gpu_dependencies = []
gpu_dependencies += cuda_dep

nvcc_flags_link = []
if cuda_version.version_compare('< 13-0')
	nvcc_flags_link += ['-gencode=arch=compute_70,code=sm_70']
endif
nvcc_flags_link += ['-gencode=arch=compute_75,code=sm_75']
nvcc_flags_link += ['-gencode=arch=compute_80,code=sm_80']
nvcc_flags_link += ['-gencode=arch=compute_90,code=sm_90']
if cuda_version.version_compare('>= 12-8')
	nvcc_flags_link += ['-gencode=arch=compute_100,code=sm_100']
endif
add_project_link_arguments(nvcc_flags_link, language: 'cuda')

sample_dependencies += gpu_dependencies

gpunetio_device_path = doca_gpu_dep.get_variable(pkgconfig : 'libdir')
dependency_gpunetio_device = declare_dependency(compile_args : '-Wl,--whole-archive',
                                                link_args : ['-L' + gpunetio_device_path , '-ldoca_gpunetio_device'],)
sample_dependencies += dependency_gpunetio_device

sample_srcs = [
	# The sample itself
	'device/' + SAMPLE_NAME + '_kernel.cu',
	'host/' + SAMPLE_NAME + '_sample.c',
	# Main function for the sample's executable
	SAMPLE_NAME + '_main.c',
	# Common code for all DOCA samples
	'../../common.c',
]

sample_inc_dirs  = []
# Common DOCA library logic
sample_inc_dirs += include_directories('..')
# Common DOCA logic (samples)
sample_inc_dirs += include_directories('../..')
# Common DOCA logic
sample_inc_dirs += include_directories('../../..')
# Common DOCA logic (applications)
sample_inc_dirs += include_directories('../../../applications/common')

executable('doca_' + SAMPLE_NAME, sample_srcs,
	c_args : '-Wno-missing-braces',
	dependencies : sample_dependencies,
	include_directories: sample_inc_dirs,
	install: false)

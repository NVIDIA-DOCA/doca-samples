#
# Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
	version: run_command(find_program('cat'), files('../../../VERSION'), check: true).stdout().strip(),
	license: 'BSD-3',
	default_options: ['buildtype=debug'],
	meson_version: '>= 0.61.2'
)

SAMPLE_NAME = 'flow_definition_mode'

sample_project_arguments = [
	'-DDOCA_ALLOW_EXPERIMENTAL_API',
	'-DDOCA_ALLOW_DEPRECATED_API',
]
add_project_arguments(sample_project_arguments, language: ['c', 'cpp'])

sample_dependencies = [
	dependency('doca-common'),
	dependency('doca-flow'),
	dependency('doca-dpdk-bridge'),
	dependency('doca-argp'),
	dependency('libdpdk'),
]

c_compiler = meson.get_compiler('c')
if c_compiler.has_header_symbol('rte_pmd_mlx5.h',
				'rte_pmd_mlx5_driver_disable_steering',
				dependencies: dependency('libdpdk'),
				prefix: '#include <rte_pmd_mlx5.h>')
	add_project_arguments('-DDOCA_BUNDLE_DPDK_FOUND', language: ['c', 'cpp'])
	message('Found rte_pmd_mlx5_driver_disable_steering(), using DOCA-Bundle DPDK')
elif c_compiler.has_header_symbol('rte_pmd_mlx5.h',
				  'rte_pmd_mlx5_disable_steering',
				  dependencies: dependency('libdpdk'),
				  prefix: '#include <rte_pmd_mlx5.h>')
	add_project_arguments('-DUPSTREAM_DPDK_FOUND', language: ['c', 'cpp'])
	message('Found rte_pmd_mlx5_disable_steering(), using Upstream DPDK')
else
	warning('Unable to find rte_pmd_mlx5_driver_disable_steering() or rte_pmd_mlx5_disable_steering()')
endif

sample_srcs = [
	SAMPLE_NAME + '_sample.c',
	SAMPLE_NAME + '_main.c',
	'../../common.c',
	'../flow_common.c',
	'../flow_tune_common.c',
	'../../../applications/common/dpdk_utils.c',
]

sample_inc_dirs = [
	include_directories('..'),
	include_directories('../..'),
	include_directories('../../../applications/common/'),
]

executable('doca_' + SAMPLE_NAME, sample_srcs,
	c_args: '-Wno-missing-braces',
	dependencies: sample_dependencies,
	include_directories: sample_inc_dirs,
	install: false)

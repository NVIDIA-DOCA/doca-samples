#
# Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
	SAMPLE_NAME + '_sample.c',
	'../flow_switch_common.c',
	'../flow_eth_common.c',
])

# Define FLOW_NO_DPDK at compile time
sample_cargs += ['-DFLOW_NO_DPDK']

sample_main_srcs = files([
	SAMPLE_NAME + '_main.c',
])

# install specific common files per sample
install_data(sources : '../flow_switch_common.c', install_dir : '/'.join([doca_samples_dir, 'doca_flow']))
install_data(sources : '../flow_eth_common.c', install_dir : '/'.join([doca_samples_dir, 'doca_flow']))

# Exclude internal directory from installation
sample_dir_excludes += 'internal'

if available_libs.contains('eth')
	sample_depends += get_variable('doca_eth_dependency')
else
	warning('Skipping compilation of DOCA Sample @0@ that needs library - @1@.'.format(SAMPLE_NAME, 'doca_eth'))
	skip_sample_compilation = true
	subdir_done()
endif

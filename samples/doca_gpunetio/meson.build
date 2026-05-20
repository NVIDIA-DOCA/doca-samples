#
# Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
	'gpunetio_send_wait_time',
	'gpunetio_simple_receive',
	'gpunetio_simple_send',
	'gpunetio_rdma_client_server_write',
	'gpunetio_verbs_twosided_bw',
	'gpunetio_verbs_get_bw',
	'gpunetio_verbs_put_bw',
	'gpunetio_verbs_put_signal_bw',
	'gpunetio_verbs_put_signal_lat',
	'gpunetio_verbs_put_counter_lat',
	'gpunetio_verbs_put_counter_bw',
	'gpunetio_verbs_write_bw',
	'gpunetio_verbs_write_lat',
	'gpunetio_dma_memcpy',
]

sample_lib_srcs = files([
	'rdma_common.c',
	'verbs_common.c',
	'verbs_high_level.c',
])

sample_lib_hdrs = files([
	'rdma_common.h',
	'verbs_common.h',
])

# GPUNetIO-based samples & apps are only supported in amalgamation build
if not flag_amalgamation_build
	skip_all_samples_compilation = true
	subdir_done()
endif

# No need to set compilation flags if we are not going to compile anything
if flag_disable_samples_compilation
	subdir_done()
endif

sample_lib_includes += [include_directories('.')]
# Needed for utils.c (DOCA Core)
sample_lib_includes += apps_common_base_include
sample_lib_cargs += gpu_compile_flags
sample_lib_depends += gpu_dependencies
# The below is only accessible in amalgamation builds (both dep and lib)
sample_lib_depends += gpunetio_device_dep
# RM#3857532: Required for maintaining build order
sample_lib_link_with += gpunetio_device_lib

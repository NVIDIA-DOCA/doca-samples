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

# Check if we should skip the compilation of the applications
if flag_disable_applications_compilation
    subdir_done()
endif

remote_offload_common_src = [
    'remote_offload_common/control_message.cpp',
    'remote_offload_common/doca_utils.cpp',
]

dpu_gpu_remote_offload_include_dirs = app_inc_dirs + include_directories('.')
dpu_gpu_remote_offload_dependencies = app_dependencies

if is_linux
    remote_offload_common_src += [
        'remote_offload_common/posix/os_utils.cpp',
        'remote_offload_common/posix/tcp_socket.cpp',
    ]
elif is_windows
    message('DPU GPU remote_offload does not currently support windows')
    subdir_done()
else
    message('DPU GPU remote_offload does not currently support this unknown operating system')
    subdir_done()
endif

# GPUNetIO-based samples & apps are only supported in amalgamation build
if not flag_amalgamation_build
    set_variable('app_' + APP_NAME + '_disable_reason', 'Internal compilation isn\'t supported for this App outside of amalgamation mode')
    subdir_done()
endif

executable(DOCA_PREFIX + APP_NAME + '_client',
           [
               'client/main.cpp',
               'client/application.cpp',
               'client/thread.cpp',
           ] + remote_offload_common_src,
           override_options : ['cpp_std=c++17'],
           c_args : apps_common_c_args,
           cpp_args : apps_common_cpp_args,
           dependencies : dpu_gpu_remote_offload_dependencies,
           include_directories : dpu_gpu_remote_offload_include_dirs,
)
gpunetio_dev_dep = dependency('doca-gpunetio', required : false)
if gpunetio_dev_dep.found()

    executable(DOCA_PREFIX + APP_NAME + '_orchestrator',
               [
                   'orchestrator/application.cpp',
                   'orchestrator/comch_control_channel.cpp',
                   'orchestrator/comch_datapath.cpp',
                   'orchestrator/main.cpp',
                   'orchestrator/gpu_processing.cu'
               ] + remote_offload_common_src,
               override_options : ['cpp_std=c++17'],
               c_args : apps_common_c_args,
               cpp_args : apps_common_cpp_args,
               dependencies : [gpunetio_device_dep, dpu_gpu_remote_offload_dependencies],
               include_directories : dpu_gpu_remote_offload_include_dirs,
    )
else
    message('Skipping compilation of DOCA Application - ' + DOCA_PREFIX + APP_NAME + '_orchestrator' + ' - Missing library doca-gpunetio')
endif

executable(DOCA_PREFIX + APP_NAME + '_server',
           [
               'server/application.cpp',
               'server/comch_control_channel.cpp',
               'server/main.cpp',
               'server/thread.cpp',
           ] + remote_offload_common_src,
           override_options : ['cpp_std=c++17'],
           c_args : apps_common_c_args,
           cpp_args : apps_common_cpp_args,
           dependencies : dpu_gpu_remote_offload_dependencies,
           include_directories : dpu_gpu_remote_offload_include_dirs,
)

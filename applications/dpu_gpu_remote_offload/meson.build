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


dpu_gpu_remote_offload_common_src = [
    'remote_offload_common/control_message.cpp',
    'remote_offload_common/doca_utils.cpp',
]

if host_machine.system() == 'linux'
    dpu_gpu_remote_offload_common_src += [
        'remote_offload_common/posix/os_utils.cpp',
        'remote_offload_common/posix/tcp_socket.cpp',
    ]
elif host_machine.system() == 'windows'
    message('DPU GPU remote_offload does not currently support windows')
    subdir_done()
else
    message('DPU GPU remote_offload does not currently support this unknown operating system')
    subdir_done()
endif

executable(DOCA_PREFIX + APP_NAME + '_client',
           [
               'client/main.cpp',
               'client/application.cpp',
               'client/thread.cpp',
           ] + dpu_gpu_remote_offload_common_src,
           override_options : ['cpp_std=c++17'],
           c_args : base_c_args,
           cpp_args : base_cpp_args,
           dependencies : app_dependencies,
           include_directories : app_inc_dirs + include_directories('.'),
           install : install_apps,
)

if is_host
    gpunetio_dev_dep = dependency('doca-gpunetio', required : false)
    if gpunetio_dev_dep.found()

        gpunetio_device_path = gpunetio_dev_dep.get_variable(pkgconfig : 'libdir')
        dependency_gpunetio_device = declare_dependency(compile_args : '-Wl,--whole-archive',
                                                        link_args : ['-L' + gpunetio_device_path , '-ldoca_gpunetio_device'],)

       executable(DOCA_PREFIX + APP_NAME + '_orchestrator',
                   [
                       'orchestrator/application.cpp',
                       'orchestrator/comch_control_channel.cpp',
                       'orchestrator/comch_datapath.cpp',
                       'orchestrator/main.cpp',
                       'orchestrator/gpu_processing.cu'
                   ] + dpu_gpu_remote_offload_common_src,
                   override_options : ['cpp_std=c++17'],
                   c_args : base_c_args,
                   cpp_args : base_cpp_args,
                   dependencies : [dependency_gpunetio_device, app_dependencies],
                   include_directories : app_inc_dirs + include_directories('.'),
                   install : install_apps,
        )
    else
        message('Skipping compilation of DOCA Application - ' + DOCA_PREFIX + APP_NAME + '_orchestrator' + ' - Missing library doca-gpunetio')
    endif
endif

if is_dpu
    executable(DOCA_PREFIX + APP_NAME + '_server',
               [
                   'server/application.cpp',
                   'server/comch_control_channel.cpp',
                   'server/main.cpp',
                   'server/thread.cpp',
               ] + dpu_gpu_remote_offload_common_src,
               override_options : ['cpp_std=c++17'],
               c_args : base_c_args,
               cpp_args : base_cpp_args,
               dependencies : app_dependencies,
               include_directories : app_inc_dirs + include_directories('.'),
               install : install_apps,
    )
endif
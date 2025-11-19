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


storage_common_src = [
    'storage_common/binary_content.cpp',
    'storage_common/buffer_utils.cpp',
    'storage_common/control_channel.cpp',
    'storage_common/control_message.cpp',
    'storage_common/doca_utils.cpp',
    'storage_common/file_utils.cpp',
    'storage_common/io_message.cpp',
    'storage_common/ip_address.cpp',
]

if host_machine.system() == 'linux'
    storage_common_src += [
        'storage_common/posix/os_utils.cpp',
        'storage_common/posix/tcp_socket.cpp',
    ]
elif host_machine.system() == 'windows'
    message('Storage application does not currently support windows')
    subdir_done()
else
    message('Storage application does not currently support this unknown operating system')
    subdir_done()
endif

lz4_dev_dep = dependency('liblz4', required : false)
if lz4_dev_dep.found()
    base_c_args += ['-DSTORAGE_APP_LZ4_SW_LIB_AVAILABLE=1']
    base_cpp_args += ['-DSTORAGE_APP_LZ4_SW_LIB_AVAILABLE=1']
    app_dependencies += [lz4_dev_dep]
    storage_common_src += [
        'storage_common/lz4_sw_context.cpp',
    ]

    executable(DOCA_PREFIX + APP_NAME + '_gga_offload_sbc_generator',
               [
                   'gga_offload_sbc_generator.cpp',
               ] + storage_common_src,
               override_options : ['cpp_std=c++17'],
               c_args : base_c_args,
               cpp_args : base_cpp_args,
               dependencies : app_dependencies,
               include_directories : app_inc_dirs + include_directories('.'),
               install : install_apps,
    )
else
    message('Skipping compilation of DOCA Application - ' + DOCA_PREFIX + APP_NAME + '_gga_offload_sbc_generator' + ' - Missing library liblz4')
    base_c_args += ['-DSTORAGE_APP_LZ4_SW_LIB_AVAILABLE=0']
    base_cpp_args += ['-DSTORAGE_APP_LZ4_SW_LIB_AVAILABLE=0']
endif

if is_host
    executable(DOCA_PREFIX + APP_NAME + '_initiator_comch',
               [
                   'initiator_comch.cpp',
               ] + storage_common_src,
               override_options : ['cpp_std=c++17'],
               c_args : base_c_args,
               cpp_args : base_cpp_args,
               dependencies : app_dependencies,
               include_directories : app_inc_dirs + include_directories('.'),
               install : install_apps,
    )

endif

if is_dpu
    executable(DOCA_PREFIX + APP_NAME + '_comch_to_rdma_zero_copy',
               [
                   'comch_to_rdma_zero_copy.cpp',
               ] + storage_common_src,
               override_options : ['cpp_std=c++17'],
               c_args : base_c_args,
               cpp_args : base_cpp_args,
               dependencies : app_dependencies,
               include_directories : app_inc_dirs + include_directories('.'),
               install : install_apps,
    )

    executable(DOCA_PREFIX + APP_NAME + '_comch_to_rdma_gga_offload',
               [
                   'comch_to_rdma_gga_offload.cpp',
               ] + storage_common_src,
               override_options : ['cpp_std=c++17'],
               c_args : base_c_args,
               cpp_args : base_cpp_args,
               dependencies : app_dependencies,
               include_directories : app_inc_dirs + include_directories('.'),
               install : install_apps,
    )
endif

executable(DOCA_PREFIX + APP_NAME + '_target_rdma',
           [
               'target_rdma.cpp',
           ] + storage_common_src,
           override_options : ['cpp_std=c++17'],
           c_args : base_c_args,
           cpp_args : base_cpp_args,
           dependencies : app_dependencies,
           include_directories : app_inc_dirs + include_directories('.'),
           install : install_apps,
)



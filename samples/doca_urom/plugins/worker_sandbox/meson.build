#
# Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

PLUGIN_NAME = 'worker_sandbox'

plugin_src = []
plugin_hrd = []
plugin_common_src = []
plugin_common_hdr = []
plugin_common_includes = []

subdir('common')

if is_dpu
        subdir('dpu')
else
        subdir('host')
endif

plugins_install_dir = '/'.join([sample_lib_install_dir, 'plugins'])
plugin_dir = plugins_install_dir + '/' + PLUGIN_NAME

meson_public_path = meson.current_source_dir() + '/' + 'meson.build.public'

install_data(sources : plugin_src, install_dir : plugin_dir)
install_data(sources : plugin_hdr, install_dir : plugin_dir)
install_data(sources : plugin_common_src, install_dir : plugin_dir)
install_data(sources : plugin_common_hdr, install_dir : plugin_dir)
install_data(sources : meson_public_path, rename: ['meson.build'], install_dir : plugin_dir)

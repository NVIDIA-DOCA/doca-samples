#
# Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

if flag_disable_applications_compilation
	subdir_done()
endif

samples_dir_path_aux = '../../samples'
common_dir_path_aux = '../common'

app_srcs += files([
	APP_NAME + '.c',
	APP_NAME + '_core.c',
	APP_NAME + 'ice.c',
	APP_NAME + '_lu.c',
	common_dir_path_aux + '/utils.c',
	samples_dir_path_aux + '/common.c',
])

if is_variable('lib_devemu_vnet_disabled')
	set_variable('app_' + APP_NAME + '_disable_reason', 'Missing devemu vnet support')
	subdir_done()
endif

vnet_doca_deps = ['devemu']
foreach doca_dep : vnet_doca_deps
	if not available_libs.contains(doca_dep)
		set_variable('app_' + APP_NAME + '_disable_reason', 'Missing dependency: ' + doca_dep)
		subdir_done()
	endif
	app_dependencies += get_variable(DOCA_PREFIX + doca_dep + '_dependency_internal')
endforeach

# Add libbsd for strlcpy
libbsd_dep = dependency('libbsd', required: false)
if libbsd_dep.found()
	app_dependencies += libbsd_dep
endif

# libibverbs
app_dependencies += dependency_libibverbs

app_inc_dirs += include_directories('.')
app_inc_dirs += include_directories(samples_dir_path_aux)

executable(DOCA_PREFIX + APP_NAME, app_srcs,
	c_args : base_c_args,
	dependencies : app_dependencies,
	include_directories: app_inc_dirs)

#
# Copyright (c) 2022-2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

# Check if mpicc exists
mpicc = find_program('mpicc', required: false)
if not mpicc.found()
	reason = 'missing mpicc'
	warning('Skipping compilation of @0@'.format(APP_NAME) + ' - ' + reason)
	subdir_done()
endif

host_srcs = files([
	'host/' + APP_NAME + '.c',
	'host/' + APP_NAME + '_core.c',
])

host_hdrs = files([
	'host/' + APP_NAME + '_core.h',
])

device_srcs = files([
	'device/' + APP_NAME + '_dev.c',
])

address_sanitizer = get_option('b_sanitize')

debug_mode = 'none'
if get_option('buildtype') == 'debug' or get_option('buildtype') == 'debugoptimized'
	debug_mode = 'true'
endif

build_script = meson.current_source_dir() / 'build_dpa_all_to_all.sh'
app_builds = find_program(build_script)
doca_dep = dependency('doca-dpa')
doca_lib_dir = doca_dep.get_variable(pkgconfig : 'libdir')
command = [app_builds, meson.current_source_dir(), meson.project_build_root(), address_sanitizer, debug_mode, dpacc_mcpu_flag, doca_lib_dir]

custom_target(DOCA_PREFIX + APP_NAME,
		command: command,
		output : DOCA_PREFIX + APP_NAME,
		install_dir : app_install_dir,
		install : install_apps,
		build_by_default: true)


#
# Copyright (c) 2022-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

# Check if openMPI is enabled
if not flag_enable_openmpi_support and skip_app_build
	set_variable('app_' + APP_NAME + '_disable_reason', 'Missing mpicc')
	subdir_done()
endif

host_srcs = files([
	'host/' + APP_NAME + '.c',
	'host/' + APP_NAME + '_core.c',
])

device_srcs = files([
	'device/' + APP_NAME + '_dev.c',
])

build_script_name = 'build_' + APP_NAME + '.sh'
build_script = meson.current_source_dir() / build_script_name
device_code_build_script_name = 'build_device_code.sh'
device_code_build_script = meson.current_source_dir() / device_code_build_script_name
if flag_amalgamation_build
	dpa_script_amalgamation = 'true'
else
	dpa_script_amalgamation = 'false'
endif

# We need to install the public version of the build scripts on our own
app_install_excludes += build_script_name
app_install_excludes += build_script_name + public_file_suffix
app_install_excludes += device_code_build_script_name
app_install_excludes += device_code_build_script_name + public_file_suffix

install_data(sources : build_script + public_file_suffix, rename : [build_script_name],
		install_dir : app_install_dir)
install_data(sources : device_code_build_script + public_file_suffix, rename : [device_code_build_script_name],
		install_dir : app_install_dir)

# Check if we should skip the compilation of the applications
if flag_disable_applications_compilation
	subdir_done()
endif

# Custom-target samples & apps are only supported in amalgamation build
if not flag_amalgamation_build
	set_variable('app_' + APP_NAME + '_disable_reason', 'Internal compilation isn\'t supported for this App outside of amalgamation mode')
	subdir_done()
endif

# Can now safely access the runtime variable
app_lib_dependencies = []
app_lib_dependencies += doca_dpa_runtime_lib

address_sanitizer = get_option('b_sanitize')

debug_mode = 'none'
if build_debug_mode
	debug_mode = 'true'
endif

# Command to compile the application
app_builds = find_program(build_script)
command = [app_builds, meson.project_source_root(), meson.project_build_root(), doca_version, address_sanitizer, debug_mode, dpa_script_amalgamation, default_dpacc_mcpu_flag]

custom_target(DOCA_PREFIX + APP_NAME,
		command: command,
		output : DOCA_PREFIX + APP_NAME,
		depends: app_lib_dependencies,
		build_by_default: true)

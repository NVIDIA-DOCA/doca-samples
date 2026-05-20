#
# Copyright (c) 2023-2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
])

sample_main_srcs = files([
	SAMPLE_NAME + '_main.c',
])

sample_excludes += [
	'build_urom_ping_pong.sh',
	'build_urom_ping_pong.sh.public',
]

# install compilation scripts file
build_script_name = 'build_' + SAMPLE_NAME + '.sh'
build_script = meson.current_source_dir() / build_script_name
install_build_script_name = 'build_' + SAMPLE_NAME + '.sh.public'
install_data(sources : meson.current_source_dir() / install_build_script_name, rename:[build_script_name], install_dir : src_install_dir)

if flag_disable_samples_compilation
	subdir_done()
endif

# Check if openMPI is enabled
if not flag_enable_openmpi_support
	skip_sample_compilation = true
	subdir_done()
endif

# Custom target based samples & apps are only supported in amalgamation build
if not flag_amalgamation_build
	skip_all_samples_compilation = true
	subdir_done()
endif

# Can now safely access the runtime variable
sample_lib_dependencies = []
sample_lib_dependencies += doca_urom_runtime_lib

sample_plugin_depends = []
sample_plugin_depends += sandbox_plugin

address_sanitizer = get_option('b_sanitize')

debug_mode = 'none'
if build_debug_mode
	debug_mode = 'true'
endif
sample_builds = find_program(build_script)

command = [sample_builds, meson.project_source_root(), meson.project_build_root(), doca_version, address_sanitizer, debug_mode]

# Compilation happens by MPI, so here we just tell meson what we need so it won't build it too-early when things are missing
custom_target(DOCA_PREFIX + SAMPLE_NAME,
	depends: [sample_lib_dependencies] + [sample_plugin_depends],
	command: command,
	output : DOCA_PREFIX + SAMPLE_NAME,
	build_by_default: true)

# No need to to compile again from the main meson
skip_sample_compilation = true

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

host_srcs = files([
	'host/' + APP_NAME + '.c',
	'host/' + APP_NAME + '_core.c',
])

device_code_build_script_name = 'build_device_code.sh'
device_code_build_script = meson.current_source_dir() / device_code_build_script_name

app_device_build_dir = meson.project_build_root() + '/' + APP_NAME + '/device/build_dpacc'

# Command to compile the application
app_builds = find_program(device_code_build_script)

doca_dep = dependency('doca-common')
doca_lib_dir = doca_dep.get_variable(pkgconfig : 'libdir')
pcc_app_dev_src_dir = meson.project_source_root() + '/' + APP_NAME + '/device/'

# check user option to enable TX bytes counter sampling
if get_option('enable_pcc_application_tx_counter_sampling')
	enable_tx_counter_sampling = 'true'
else
	enable_tx_counter_sampling = 'false'
endif

# Compile DOCA PCC Reaction Point RTT Template DPA program
pcc_app_name = 'pcc_rp_rtt_template_app'
pcc_dev_stubs_keep_dir_path = app_device_build_dir + '/' + pcc_app_name
run_command(app_builds, pcc_app_dev_src_dir, app_device_build_dir, doca_lib_dir, pcc_app_name,
	pcc_dev_stubs_keep_dir_path, enable_tx_counter_sampling, dpacc_mcpu_flag, check: true)
pcc_rp_rtt_template_app = meson.get_compiler('c').find_library(pcc_app_name, dirs : pcc_dev_stubs_keep_dir_path, static: true)

# Compile DOCA PCC Reaction Point Switch Telemetry DPA program
pcc_app_name = 'pcc_rp_switch_telemetry_app'
pcc_dev_stubs_keep_dir_path = app_device_build_dir + '/' + pcc_app_name
run_command(app_builds, pcc_app_dev_src_dir, app_device_build_dir, doca_lib_dir, pcc_app_name,
	pcc_dev_stubs_keep_dir_path, enable_tx_counter_sampling, dpacc_mcpu_flag, check: true)
pcc_rp_switch_telemetry_app = meson.get_compiler('c').find_library(pcc_app_name, dirs : pcc_dev_stubs_keep_dir_path, static: true)

# Compile DOCA PCC Notification Point Switch Telemetry DPA program
pcc_app_name = 'pcc_np_switch_telemetry_app'
pcc_dev_stubs_keep_dir_path = app_device_build_dir + '/' + pcc_app_name
run_command(app_builds, pcc_app_dev_src_dir, app_device_build_dir, doca_lib_dir, pcc_app_name,
	pcc_dev_stubs_keep_dir_path, enable_tx_counter_sampling, dpacc_mcpu_flag, check: true)
pcc_np_switch_telemetry_app = meson.get_compiler('c').find_library(pcc_app_name, dirs : pcc_dev_stubs_keep_dir_path, static: true)

# Build executable
doca_pcc_test = executable(DOCA_PREFIX + APP_NAME,
	host_srcs,
	c_args : base_c_args,
	include_directories: [app_inc_dirs, './host'],
	install_dir : app_install_dir,
	dependencies : [app_dependencies, pcc_rp_rtt_template_app, pcc_rp_switch_telemetry_app, pcc_np_switch_telemetry_app],
	install: install_apps
)

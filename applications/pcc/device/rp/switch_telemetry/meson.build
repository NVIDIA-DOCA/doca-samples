#
# Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#### Building DOCA PCC RP Switch Telemetry program ####

app_program_name = APP_NAME + '_rp_switch_telemetry_program'
app_dev_name = 'pcc_rp_switch_telemetry_app'

app_device_sources = files([
	'rp_switch_telemetry_dev_main.c',
        'algo/telem_template.c',
])

app_rp_switch_telemetry_device_include_flags = app_common_device_include_flags + app_rp_device_include_flags
app_rp_switch_telemetry_device_include_flags += ['-I' + doca_root_dir / 'applications/pcc/device/rp/switch_telemetry',]
app_rp_switch_telemetry_device_include_flags += ['-I' + doca_root_dir / 'applications/pcc/device/rp/switch_telemetry/algo',]

# Attributes file to be used
app_device_attributes_arg = meson.current_source_dir() + '/../../dpa_app_attributes.yaml'

app_srcs += custom_target(
		app_program_name,
		input: app_device_sources,
		command : [
			dpa_build_script,
			' '.join(app_rp_switch_telemetry_device_include_flags),
			app_host_cc_extra_flags,
			app_device_cc_extra_flags,
			'@SOURCE_ROOT@',
			'@OUTDIR@',
			'application-attributes',
			default_dpacc_mcpu_flag,
			pcc_device_link_flags,
			app_dev_name,
			app_device_attributes_arg,
			'@OUTPUT@',
			'@INPUT@',
			],
		depends: app_device_depends,
		depfile: '@BASENAME@.d',
		output : [app_program_name + '.a'])

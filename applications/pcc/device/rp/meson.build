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

# App flags
DOCA_PCC_SAMPLE_TX_BYTES = ''
# DOCA_PCC_SAMPLE_TX_BYTES='-DDOCA_PCC_SAMPLE_TX_BYTES ' ## to be enabled only on development

app_device_cc_extra_flags = DOCA_PCC_SAMPLE_TX_BYTES
app_host_cc_extra_flags = ''
pcc_device_link_flags = ''
app_rp_device_include_flags = []
app_device_depends = []
pcc_device_lib_name = 'pcc_dev'
if flag_amalgamation_build
	app_device_depends += doca_pcc_runtime_lib
	pcc_device_lib_info = get_variable('doca_' + pcc_device_lib_name + '_dev_lib_info')
	pcc_device_link_flags += ' ' + pcc_device_lib_info.get_variable('dev_link_args')
	app_rp_device_include_flags += pcc_device_lib_info.get_variable('dev_include_args')
	app_rp_device_include_flags += [
		'-I' + doca_root_dir / 'libs/doca_common/version',
		'-I' + doca_root_dir / 'libs/doca_common/core/include/public',
	]
else
	pcc_device_link_flags += '-L/opt/mellanox/doca' / get_option('libdir')
	pcc_device_link_flags += ' -ldoca_' + pcc_device_lib_name
	app_rp_device_include_flags += [
		'-I/opt/mellanox/doca/include',
	]
endif

subdir('rtt_template')
subdir('switch_telemetry')

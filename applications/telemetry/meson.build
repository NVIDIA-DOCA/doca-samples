#
# Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

# Compilation of Traceback application
if get_option('enable_telemetry_traceback')
	json_c_dep = dependency('json-c', required : false)

	if json_c_dep.found()
		executable(DOCA_PREFIX + APP_NAME + '_traceback',
				   [
					   'traceback/traceback.cpp',
					   'traceback/traceback_core.cpp',
				   ],
				   override_options : ['cpp_std=c++17'],
				   c_args : base_c_args,
				   cpp_args : base_cpp_args,
				   dependencies : app_dependencies + json_c_dep,
				   include_directories : app_inc_dirs + include_directories('traceback'),
				   install : install_apps,
		)
	else
		message('Skipping compilation of DOCA Application - ' + DOCA_PREFIX + APP_NAME + '_traceback' + ' - Missing library json-c')
	endif
else
	message('Skipping compilation of DOCA Application - ' + DOCA_PREFIX + APP_NAME + '_traceback')
endif

# Compilation of OpenTelemetry application
if get_option('enable_telemetry_opentelemetry')
	otel_dep = dependency(
		'opentelemetry-cpp',
		method : 'cmake',
		cmake_module_path : ['/usr/lib/cmake/opentelemetry-cpp'],
		modules : [
			'opentelemetry-cpp::api',
			'opentelemetry-cpp::otlp_http_metric_exporter',
			'opentelemetry-cpp::http_client_curl',
		],
		required : false
	)

	curl_dep = dependency('libcurl', required: false)

	if not otel_dep.found()
		message('Skipping compilation of DOCA Application - ' + DOCA_PREFIX + APP_NAME + '_opentelemetry' + ' - Missing OpenTelemetry-cpp SDK')
	elif not curl_dep.found()
		message('Skipping compilation of DOCA Application - ' + DOCA_PREFIX + APP_NAME + '_opentelemetry' + ' - Missing Curl support')
	else
		executable(DOCA_PREFIX + APP_NAME + '_opentelemetry',
				[
					'opentelemetry/opentelemetry.cpp',
					'opentelemetry/opentelemetry_core.cpp',
				],
				override_options : ['cpp_std=c++17'],
				c_args : base_c_args,
				dependencies : app_dependencies + otel_dep + curl_dep,
				include_directories: app_inc_dirs + include_directories('opentelemetry'),
				install : install_apps,
		)
	endif
else
	message('Skipping compilation of DOCA Application - ' + DOCA_PREFIX + APP_NAME + '_opentelemetry')
endif

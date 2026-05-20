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

common_dependencies = ['comch', 'dma'] # common files dependencies

# explicitly declare any dependency per sample
raw_samples = {
	'graph': ['dma'],
	'sync_event_local_pci': ['comch'],
	'sync_event_remote_pci': ['comch'],
	'pe_polling': ['dma'],
	'pe_event': ['dma'],
	'pe_multi_context': ['dma'],
	'pe_task_resubmit': ['dma'],
	'pe_async_stop': ['dma'],
	'pe_single_task_comp_cb': ['dma'],
	'pe_reactive': ['dma'],
	'pe_task_try_submit': ['dma'],
	'pe_task_error': ['dma'],
	'cache_invalidate': ['dma'],
	'clock_cross_timestamp': ['argp'],
}

samples = []

foreach SAMPLE_NAME, sample_dependencies : raw_samples
	skip_sample_compilation = false
	# add common files dependencies as a dependency in each doca_common sample
	foreach common_dependency : common_dependencies
		if common_dependency not in sample_dependencies
			sample_dependencies += common_dependency
		endif
	endforeach
	foreach LIB_DEP_BASE_NAME : sample_dependencies
		LIB_DEP_NAME = DOCA_PREFIX + LIB_DEP_BASE_NAME
		if not available_libs.contains(LIB_DEP_BASE_NAME)
			warning('Skipping compilation of DOCA Sample @0@ that needs library - @1@.'.format(SAMPLE_NAME, LIB_DEP_NAME))
			skip_sample_compilation = true
			break
		endif
		# Add the dependency so we could use it
		sample_lib_depends += get_variable(LIB_DEP_NAME + '_dependency')
	endforeach
	if skip_sample_compilation and not sample_continue_even_if_missing
		continue
	endif
	samples += SAMPLE_NAME
endforeach

if samples.length() == 0 # no doca_common sample to compile
	skip_all_samples_compilation = true
endif

sample_lib_includes += [include_directories('.')]
# Needed for utils.c (DOCA Core)
sample_lib_includes += apps_common_base_include

sample_lib_srcs = files([
	'common_common.c',
	'pe_common.c',
])

sample_lib_hdrs = files([
	'common_common.h',
	'pe_common.h',
])

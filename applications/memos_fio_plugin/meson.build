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

memos_fio_lib_srcs = files([
	'memos_fio_plugin_dev.c',
	'memos_fio_plugin_dev_io.c',
	'memos_fio_plugin_thread.c',
	'memos_fio_utils.c',
])

xxhash_dependency = dependency('libxxhash', required: false)
if not xxhash_dependency.found()
	warning('Skipping compilation of DOCA Application - @0@ - Missing libxxhash support.'.format(APP_NAME))
	subdir_done()
endif

app_dependencies += xxhash_dependency

app_inc_dirs += include_directories('.')

# Standalone sample application demonstrating direct use of the library modules.
# Unlike the ioengine plugin below, it does NOT depend on fio headers.
executable(DOCA_PREFIX + 'memos_fio',
	files(['memos_fio.c']) + memos_fio_lib_srcs,
	c_args : [base_c_args, '-Wno-redundant-decls'],
	include_directories: app_inc_dirs,
	dependencies : app_dependencies,
	install: install_apps,
)

fio_prefix = get_option('fio_prefix')

if fio_prefix == ''
	set_variable('app_' + APP_NAME + '_disable_reason', 'fio_prefix not specified')
	subdir_done()
endif

# include_directories() hard-fails configuration for the whole tree on a missing path,
# so a stale fio_prefix must skip the ioengine rather than break every other target.
if not (fs.is_dir(fio_prefix) and fs.exists(fio_prefix / 'fio.h') and fs.exists(fio_prefix / 'config-host.h'))
	set_variable('app_' + APP_NAME + '_disable_reason',
		'fio_prefix is not a configured fio source tree: ' + fio_prefix)
	subdir_done()
endif

app_srcs += files([APP_NAME + '.c'])
app_srcs += memos_fio_lib_srcs

shared_library(DOCA_PREFIX + APP_NAME,
	app_srcs,
	c_args : [base_c_args, '-Wno-redundant-decls'],
	cpp_args : base_cpp_args,
	include_directories: [
		app_inc_dirs,
		include_directories(fio_prefix),
	],
	dependencies : app_dependencies,
	link_args: '-rdynamic',
	override_options: ['b_lundef=false'],
	install: install_apps,
)

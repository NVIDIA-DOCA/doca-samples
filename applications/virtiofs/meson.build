#
# Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

# Check for libnfs library
dependency_libnfs = dependency('libnfs', required: false)
if not dependency_libnfs.found()
	set_variable('app_' + APP_NAME + '_disable_reason', 'Missing libnfs package')
	subdir_done()
endif

# Check for glib library
dependency_glib = dependency('glib-2.0', required: false)
if not dependency_glib.found()
	set_variable('app_' + APP_NAME + '_disable_reason', 'Missing glib-2.0 package')
	subdir_done()
endif

app_dependencies += dependency_libnfs
app_dependencies += dependency_glib

app_srcs += files([
	'virtiofs.c',
	'virtiofs_core.c',
	'virtiofs_thread.c',
	'virtiofs_mpool.c',
	'virtiofs_manager.c',
	'virtiofs_device.c',
	'virtiofs_request.c',

	# nfs_fsdev
	'nfs_fsdev/nfs_fsdev.c',
	'nfs_fsdev/nfs_fsdev_pipe.cpp',
	'nfs_fsdev/nfs_fsdev_io_device.cpp',
	'nfs_fsdev/nfs_fsdev_recovery.c',
])

app_inc_dirs += include_directories('nfs_fsdev')
app_inc_dirs += include_directories('.')

executable(DOCA_PREFIX + APP_NAME, app_srcs,
	override_options: ['cpp_std=c++17'],
	c_args : base_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: install_apps)
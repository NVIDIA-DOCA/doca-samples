#
# Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

app_srcs += files([
	'vblk_pci.c',
	'pci_spec_tlp.c',
	'vblk_ctrl.c',
	'vblk_io_ctx.c',
	'vblk_tlp_ctx.c',
	'vblk_pci_dev.c',
	'vblk_pci_dev_core.c',
	'vblk_mpool.c',
	common_dir_path + '/utils.c',
	samples_dir_path + '/common.c',
])

# Add libbsd for strlcpy
libbsd_dep = dependency('libbsd', required: false)
if libbsd_dep.found()
	app_dependencies += libbsd_dep
endif

app_inc_dirs += include_directories('.')
app_inc_dirs += include_directories(samples_dir_path)

executable(DOCA_PREFIX + APP_NAME, app_srcs,
	c_args : base_c_args,
	dependencies : app_dependencies,
	include_directories: app_inc_dirs,
	install: install_apps)

# Common LU sources shared by TLP LU and EMU binaries
common_lu_srcs = files([
	'vblk_pci_lu.c',
	'pci_spec_tlp_lu.c',
	'vblk_ctrl_lu.c',
	'vblk_mpool_lu.c',
	'vblk_ipc.c',
	'vblk_handover.c',
	common_dir_path + '/utils.c',
	samples_dir_path + '/common.c',
])

# TLP LU binary (user-facing, spawns EMU child)
tlp_lu_srcs = common_lu_srcs + files([
	'vblk_pci_dev_lu.c',
	'vblk_pci_dev_tlp.c',
	'vblk_tlp_ctx_lu.c',
])

executable(DOCA_PREFIX + APP_NAME + '_lu', tlp_lu_srcs,
	c_args : base_c_args,
	dependencies : app_dependencies,
	include_directories: app_inc_dirs,
	install: install_apps)

# EMU child binary (spawned by TLP LU app)
emu_srcs = common_lu_srcs + files([
	'vblk_pci_dev_emu.c',
	'vblk_pci_dev_core_lu.c',
	'vblk_io_ctx_lu.c',
])

executable(DOCA_PREFIX + APP_NAME + '_emu', emu_srcs,
	c_args : base_c_args,
	dependencies : app_dependencies,
	include_directories: app_inc_dirs,
	install: install_apps)

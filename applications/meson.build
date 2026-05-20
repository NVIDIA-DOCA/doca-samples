#
# Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

if flag_disable_all_applications
	warning('Skipping compilation of DOCA Applications.')
	subdir_done()
endif

if flag_disable_all_samples
	flag_disable_all_applications = true
	warning('Skipping compilation of DOCA Applications due to missing dependency, please enable all samples first.')
	subdir_done()
endif

apps_common_dir_include = []
# Common files are not needed on Windows for now
if is_linux
	subdir('common')
endif

install_apps = false

base_app_dependencies  = []
if not flag_disable_applications_compilation
	base_app_dependencies += dependency_threads
	if dependency_libbsd.found()
		base_app_dependencies += dependency_libbsd
	endif
endif

base_app_inc_dirs = doca_base_include + apps_common_dir_include

path_to_root = '../'
common_dir_path = '../common'
samples_dir_path = '../../samples'
meson_options_file = files(['meson_options.txt'])
public_meson_file  = files([public_meson_file_name])

# Install the main DOCA VERSION file as the version of the DOCA Reference Applications
install_data(sources : [path_to_root + 'VERSION'], install_dir : doca_applications_dir)
install_data(sources : [path_to_root + 'VERSION'], install_dir : doca_install_dir)
install_data(sources : meson_options_file, rename: ['meson_options.txt'], install_dir : doca_applications_dir)
install_data(sources : [public_meson_file], rename : [regular_meson_file_name], install_dir : doca_applications_dir)

# List of all DOCA applications - Alphabetical order
app_list = [
	'app_shield_agent',
	'bifurcated_driver_model',
	'dma_copy',
	'dpa_all_to_all',
	'dpu_gpu_remote_offload',
	'east_west_overlay_encryption',
	'eth_l2_fwd',
	'file_compression',
	'file_integrity',
	'gpu_packet_processing',
	'ip_frag',
	'ipsec_security_gw',
	'nvme_emulation',
	'pcc',
	'psp_gateway',
	'secure_channel',
	'simple_fwd_vnf',
	'sta_offload',
	'storage',
	'stream_receive_perf',
	'switch',
	'time_sync',
	'upf_accel',
	'urom_rdmo',
	'urom_ucc',
	'virtiofs',
	'yara_inspection',
]

enabled_apps = []
skipped_apps  = []

foreach APP_NAME : app_list
	# Apps could be missing if publicly exported a subset of our code
	if not fs.is_dir(APP_NAME)
		continue
	endif

	# Clear the list between each application
	app_install_excludes = ['OWNERS']
	app_install_excludes += []
	app_has_sources = true
	is_internal_app = false

	app_install_dir = '/'.join([doca_applications_dir, APP_NAME])

	# Basic app definitions
	app_dependencies = base_app_dependencies
	# List of DOCA libraries & drivers (names) we depend upon
	app_doca_depends = []
	app_driver_depends = []
	# List of DOCA libraries & drivers (real dependencies)
	app_doca_dependencies = []
	app_driver_dependencies = []
	app_srcs = []
	app_inc_dirs = base_app_inc_dirs

	# Create absolute path
	absolute_path = '/'.join([meson.current_source_dir(), APP_NAME])

	# Should we skip the application directory? Or should we continue and install it?
	# * If we are told to compile it, we must skip if something is wrong
	# * This is true unless we started to install content
	skip_app_build = not flag_disable_applications_compilation
	app_install_started = false

	######################################
	# Check dependencies before starting #
	######################################

	if fs.is_dir(APP_NAME + '/' + 'dependencies')
		subdir(APP_NAME + '/' + 'dependencies')
	endif

	# Skip internal apps in official build
	if is_internal_app and flag_official_doca_build
		warning('Skipping processing of internal DOCA Application - @0@.'.format(APP_NAME))
		set_variable('app_' + APP_NAME + '_disable_reason', 'Internal application')
		continue
	endif

	# Everything should use common (no need for them to add it manually)
	app_doca_depends += 'common'
	# Once we get rid of "script apps" we can add here "argp" as well

	# DOCA Libraries
	foreach doca_dep : app_doca_depends
		if not available_libs.contains(doca_dep)
			set_variable('app_' + APP_NAME + '_disable_reason', 'Missing DOCA library ' + doca_dep)
			break
		endif
		# We use the internal dependency if fault injection is enabled
		if flag_enable_fault_injection
			app_doca_dependencies += get_variable(DOCA_PREFIX + doca_dep + '_dependency_internal')
		else
			app_doca_dependencies += get_variable(DOCA_PREFIX + doca_dep + '_dependency')
		endif
	endforeach
	# DOCA Drivers
	foreach doca_driver : app_driver_depends
		if not enabled_drivers.contains(doca_driver)
			set_variable('app_' + APP_NAME + '_disable_reason', 'Missing DOCA driver ' + doca_driver)
			break
		endif
		app_driver_dependencies += get_variable('dependency_' + doca_driver)
	endforeach

	# Was it disabled? (By itself or due to dependencies)
	if is_variable('app_' + APP_NAME + '_disable_reason') and skip_app_build
		warning('Skipping compilation of DOCA Application - @0@ - @1@'.format(APP_NAME, get_variable('app_' + APP_NAME + '_disable_reason')))
		skipped_apps += APP_NAME
		continue
	endif

	# Pass on the new information
	app_dependencies += app_doca_dependencies
	app_dependencies += app_driver_dependencies

	#########################################
	# Invoke the application itself - Start #
	#########################################

	# Enter the application's directory
	subdir(APP_NAME)

	#######################################
	# Invoke the application itself - End #
	#######################################

	# Was it disabled? (By itself or due to dependencies)
	if is_variable('app_' + APP_NAME + '_disable_reason') and skip_app_build
		warning('Skipping compilation of DOCA Application - @0@ - @1@'.format(APP_NAME, get_variable('app_' + APP_NAME + '_disable_reason')))
		skipped_apps += APP_NAME
		# If we started the installation (internally) we should continue
		if not app_install_started
			continue
		endif
	endif

	enabled_apps += APP_NAME

	# We will take care of the meson ourselves
	app_install_excludes += regular_meson_file_name
	if app_has_sources
		app_install_excludes += public_meson_file_name
	endif

	# Install the entire application directory (with some excluded files)
	install_subdir(APP_NAME, install_dir : doca_applications_dir, exclude_files : app_install_excludes)

	# Stop if there is nothing more to do
	if not app_has_sources
		continue
	endif

	# Install public meson
	public_meson_path = '/'.join([absolute_path, public_meson_file_name])
	install_data(sources : public_meson_path, rename : [regular_meson_file_name], install_dir : app_install_dir)

	# Meson variable for the application's include files (internal use)
	set_variable(APP_NAME + '_dir_include', include_directories(APP_NAME))

endforeach

# Unset application-specific variables that might accidentally be used later on by mistake
unset_variable('app_install_excludes')
unset_variable('app_install_dir')
unset_variable('app_doca_depends')
unset_variable('app_driver_depends')
unset_variable('app_doca_dependencies')
unset_variable('app_driver_dependencies')
unset_variable('app_dependencies')
unset_variable('app_inc_dirs')

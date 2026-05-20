#
# Copyright (c) 2021-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

if flag_disable_all_samples
	warning('Skipping processing of DOCA Samples.')
	subdir_done()
endif

# We always install ALL samples, which means we should continue processing the samples even
# if something is missing, unless we are required to compile them (which also means this isn't
# an official build which is sources only).
# In ALL other cases (compilation required) we should skip modules if their dependencies are missing.
sample_continue_even_if_missing = flag_disable_samples_compilation

# 1. We must have Argp for compiling the samples
# 2. We don't compile samples in release, so no need to continue to install part
if not available_libs.contains('argp') and not flag_disable_samples_compilation
	flag_disable_all_samples = true
	warning('Skipping compilation of DOCA Samples - Missing DOCA Library Argp.')
	subdir_done()
endif

base_sample_includes = [doca_root_dir_include] + doca_base_include

# Common definitions before we enter the main loop
public_file_suffix = '.public'
regular_meson_file_name = 'meson.build'
public_meson_file_name = regular_meson_file_name + public_file_suffix

samples_dir_include = include_directories('.')
base_sample_includes += [samples_dir_include]
samples_common_srcs = files(['common.c'])
samples_common_install = files(['common.c', 'common.h', 'common_utils.h'])
install_data(sources : samples_common_install, install_dir : '/'.join([doca_samples_dir]))

# Place here the list of subdirs per DOCA library - Alphabetical order
sample_libs = [
	'aes_gcm',
	'apsh',
	'comch',
	'compress',
	'common',
	'devemu',
	'dma',
	'dpa',
	'erasure_coding',
	'eth',
	'flow',
	'gpunetio',
	'mgmt',
	'rdma',
	'verbs',
	'rmax',
	'sha',
	'telemetry',
	'telemetry_exporter',
	'urom',
]

# Build the unified list of libraries (allows for developer-friendly printing)
sample_libraries = []
foreach LIB_BASE_NAME : sample_libs
	if not sample_continue_even_if_missing and not available_libs.contains(LIB_BASE_NAME)
		warning('Skipping compilation of DOCA Samples for library - @0@.'.format(DOCA_PREFIX + LIB_BASE_NAME))
		continue
	endif
	sample_libraries += [LIB_BASE_NAME]
endforeach

# Can only happen if we should skip samples with missing dependencies
if sample_libraries.length() == 0
	warning('No libraries to process samples for, skipping')
	subdir_done()
endif

# This is added temporarily to build doca_data_direct sample and will be removed after July 2025 release
sample_libraries += ['data_direct']

# Go over all the modules and their samples
foreach LIB_BASE_NAME : sample_libraries
	LIB_NAME = DOCA_PREFIX + LIB_BASE_NAME
	skip_all_samples_compilation = false
	# Dependency management (common to all samples for this lib)
	sample_lib_depends = []
	# List of DOCA libraries & drivers (names) we depend upon
	sample_lib_doca_depends = []
	sample_lib_driver_depends = []
	# List of DOCA libraries & drivers (real dependencies)
	sample_lib_doca_dependencies = []
	sample_lib_driver_dependencies = []
	# Common sources from the samples module
	sample_lib_srcs = []
	sample_lib_hdrs = []
	sample_lib_includes = base_sample_includes
	sample_lib_link_with = []
	# sample's main declare locally the sample function declaration
	sample_lib_cargs = [
		'-Wno-missing-declarations',
		'-Wno-missing-prototypes',
	]
	sample_lib_override_options = []
	sample_commons = samples_common_srcs
	# Additional files to be installed on top of the plain common sources
	sample_lib_installs = []

	sample_lib_install_dir = '/'.join([doca_samples_dir, LIB_NAME])

	######################################
	# Check dependencies before starting #
	######################################

	if fs.is_dir(LIB_NAME + '/' + 'dependencies')
		subdir(LIB_NAME + '/' + 'dependencies')
	endif

	# Everything should use the following (no need for them to add it manually)
	sample_lib_doca_depends += 'common'
	sample_lib_doca_depends += 'argp'
	sample_lib_doca_depends += LIB_BASE_NAME

	# DOCA Libraries
	foreach doca_dep : sample_lib_doca_depends
		# No need for dependencies if we aren't going to compile anything
		if sample_continue_even_if_missing
			continue
		endif
		if not available_libs.contains(doca_dep)
			set_variable('sample_lib_' + LIB_NAME + '_disable_reason', 'Missing DOCA library ' + doca_dep)
			break
		endif
		# We use the internal dependency if fault injection is enabled
		if flag_enable_fault_injection
			sample_lib_doca_dependencies += get_variable(DOCA_PREFIX + doca_dep + '_dependency_internal')
		else
			sample_lib_doca_dependencies += get_variable(DOCA_PREFIX + doca_dep + '_dependency')
		endif
	endforeach
	# DOCA Drivers
	foreach doca_driver : sample_lib_driver_depends
		# No need for dependencies if we aren't going to compile anything
		if sample_continue_even_if_missing
			continue
		endif
		if not enabled_drivers.contains(doca_driver)
			set_variable('sample_lib_' + LIB_NAME + '_disable_reason', 'Missing DOCA driver ' + doca_driver)
			break
		endif
		sample_lib_driver_dependencies += get_variable('dependency_' + doca_driver)
	endforeach

	# Was it disabled? (By itself or due to dependencies)
	if not sample_continue_even_if_missing and is_variable('sample_lib_' + LIB_NAME + '_disable_reason')
		# This is added temporarily to build doca_data_direct sample and will be removed after July 2025 release
		if (LIB_NAME != 'doca_data_direct')
			warning('Skipping compilation of DOCA Samples for library - @0@ - @1@.'.format(LIB_NAME, get_variable('sample_lib_' + LIB_NAME + '_disable_reason')))
			continue
		endif
	endif

	# Pass on the new information
	sample_lib_depends += sample_lib_doca_dependencies
	sample_lib_depends += sample_lib_driver_dependencies

	#########################################
	# Learn about the module itself - Start #
	#########################################

	# Get the list of sample for this module
	subdir(LIB_NAME)

	#######################################
	# Learn about the module itself - End #
	#######################################

	# Install the module common files
	sample_lib_installs += sample_lib_srcs
	sample_lib_installs += sample_lib_hdrs
	install_data(sources : sample_lib_installs, install_dir : sample_lib_install_dir)
	install_data(sources : LIB_NAME + '/README.md', install_dir : sample_lib_install_dir)

	# If the library has any headers, let others access the lib headers more easily
	if sample_lib_hdrs.length() > 0
		# Meson variable for the application's include files (internal use)
		set_variable('samples_' + LIB_NAME + '_dir_include', sample_lib_includes)
	endif

	foreach SAMPLE_NAME : samples
		sample_dir = LIB_NAME + '/' + SAMPLE_NAME
		sample_subsystems = []

		# # Should we have a sub-systems?
		if fs.is_dir('/'.join(sample_dir, 'subsystems'))
			subdir('/'.join(sample_dir, 'subsystems'))
		# Nope, it is a simple case
		else
			# Single module per sample, no subsystems
			sample_subsystems += ['']
		endif

		foreach SUBSYSTEM_NAME : sample_subsystems
			sample_subsystem_dir = '/'.join(sample_dir, SUBSYSTEM_NAME)
			# Sources used by the sample directly
			sample_srcs = []
			sample_excludes = [] # Could be files used for internal DPA compilation for instance
			sample_dir_excludes = [UNIT_TEST_DIR] # All testing files should be under the unit test directory
			# Vars used to compile the sample's main
			sample_main_srcs = []
			sample_main_includes = []
			sample_depends = sample_lib_depends
			sample_link_with = sample_lib_link_with
			sample_cargs = base_c_args + sample_lib_cargs
			sample_cppargs = base_cpp_args
			if flag_enable_fault_injection
				sample_cargs += ['-include', 'priv_extern_doca_malloc.h']
				sample_cppargs += ['-include', 'priv_extern_doca_malloc.h']
			endif

			# Define paths and names, handle case where no subsystems exist
			executable_name = DOCA_PREFIX + SAMPLE_NAME
			src_install_dir = '/'.join([sample_lib_install_dir, SAMPLE_NAME])
			sample_subsystem_install_dir = sample_lib_install_dir
			if SUBSYSTEM_NAME != ''
				executable_name = '_'.join(executable_name, SUBSYSTEM_NAME)
				src_install_dir = '/'.join(src_install_dir, SUBSYSTEM_NAME)
				sample_subsystem_install_dir = '/'.join(sample_subsystem_install_dir, SAMPLE_NAME)
			endif

			# Create absolute path
			absolute_path = '/'.join([meson.current_source_dir(), sample_subsystem_dir])

			sample_excludes += regular_meson_file_name
			sample_excludes += public_meson_file_name
			sample_excludes += 'OWNERS'

			# Descend into the sample itself
			skip_sample_compilation = flag_disable_samples_compilation or skip_all_samples_compilation
			subdir(sample_subsystem_dir)

			# Install the entire sample directory (with some excluded files)
			install_subdir(sample_subsystem_dir,
				install_dir : sample_subsystem_install_dir,
				exclude_files : sample_excludes,
				exclude_directories : sample_dir_excludes)
			# Install public meson
			public_meson_path = '/'.join([sample_subsystem_dir, public_meson_file_name])
			install_data(sources : public_meson_path, rename : [regular_meson_file_name], install_dir : src_install_dir)

			# Check if we should skip the compilation of the sample
			if skip_sample_compilation
				continue
			endif

			# Compile the sample
			executable(executable_name, sample_main_srcs + sample_srcs + sample_lib_srcs + sample_commons,
					c_args : sample_cargs,
					cuda_args : gpu_compile_flags,
					dependencies : sample_depends,
					link_with : sample_link_with,
					include_directories: sample_main_includes + sample_lib_includes,
					override_options : sample_lib_override_options,
					install: false)
		endforeach
	endforeach
endforeach

# Unset sample-specific variables that might accidentally be used later on by mistake
unset_variable('sample_lib_srcs')
unset_variable('sample_lib_hdrs')
unset_variable('sample_lib_includes')
unset_variable('sample_lib_depends')
unset_variable('sample_lib_link_with')
unset_variable('sample_lib_cargs')
unset_variable('sample_lib_override_options')
unset_variable('sample_commons')
unset_variable('sample_lib_installs')
unset_variable('sample_lib_install_dir')

if not is_variable('sample_srcs')
	warning('Could NOT find any samples for any of the libraries...')
	subdir_done()
endif
unset_variable('sample_srcs')
unset_variable('sample_excludes')
unset_variable('sample_main_srcs')
unset_variable('sample_main_includes')
unset_variable('sample_depends')
unset_variable('sample_link_with')
unset_variable('sample_cargs')
unset_variable('sample_cppargs')
unset_variable('src_install_dir')
unset_variable('sample_dir')

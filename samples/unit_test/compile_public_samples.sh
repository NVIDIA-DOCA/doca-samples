#!/usr/bin/env bash

#
# Copyright (c) 2022-2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#
# Test the compilation of the publicly installed DOCA SDK samples
#
# Usage:
#     ./compile_public_samples.sh ([list_of_libraries.txt])
#
# An optional parameter can specify a desired SUBSET of DOCA SDK libraries to be
# tested. An example for such a file that focuses on a random subset of DOCA SDK
# libraries is the following:
# "
# doca_aes_gcm
# doca_compress
# doca_dma
# "
#
# Note:
# If a list of libraries isn't provided, ALL samples will be built.
# If environment variable SAMPLES_FOLDER_PATH is set, will use this folder to derive all applications list
# Examples:
#  1. Test the build of ALL samples:
#         ./compile_public_samples.sh
#  2. Test the build of only a selected set of DOCA SDK libraries:
#         ./compile_public_samples.sh doca_networking_libraries.txt
#

##################
# Configurations #
##################

shopt -s dotglob
shopt -s nullglob

# Color variables
red='\033[0;31m'
green='\033[0;32m'
# Clear the color after that
clear='\033[0m'

skip_lib_dir_array=(
	# Skip the unit test directory (obviously)
	'unit_test'
)

skip_sample_dir_array=()
skip_host_sample_dir_array=(
	'doca_urom'
)

# Number of jobs for parallel execution
NUM_JOBS=7

##################
## Script Start ##
##################

# Binary target directory prefix
target_dir_prefix=/tmp/build

# Return value variable
returncode=0

# Derive a list of all known sample libraries (skipped will be processed later on)

SAMPLES_FOLDER="${SAMPLES_FOLDER_PATH:-/opt/mellanox/doca/samples}"
pushd "$SAMPLES_FOLDER"
ALL_SAMPLE_LIBS=(*/)
popd

# Generic function checks binary files for samples programs and plugins
binary_check() {
	local name=$1
	local bin=$2
        local build_target_dir="${target_dir_prefix}${BASHPID}"
	# Use current PID in the directory to allow parallelism
	rm -rf ${build_target_dir}
	meson setup ${build_target_dir} -Dwarning_level=2 -Dwerror=true
	ninja -C ${build_target_dir}
	[ ! -f "${build_target_dir}/$bin" ] && echo -e "${red}${name} binary file is missing!${clear}" && touch "/tmp/samples_check_failed"
}

# The user passed a library list
if [ $# -eq 1 ]; then
    if [ -f "$1" ]; then
        readarray -t REQUESTED_LIBS < $1
    else
        echo "Failed to find the requested sample libs list file: $1."
        exit 1
    fi
    # Build a list of lib directories based on the user's request
    TESTED_SAMPLE_LIBS=()
    for i in "${ALL_SAMPLE_LIBS[@]}"; do
        for j in "${REQUESTED_LIBS[@]}"; do
            if [[ ${i::-1} == $j ]]; then
                TESTED_SAMPLE_LIBS+=("${i::-1}")
                break;
            fi
        done
    done
    echo "A sample lib list was provided, going to test only the samples of the following libraries:"
    printf '%s\n' "${TESTED_SAMPLE_LIBS[@]}"
else
    TESTED_SAMPLE_LIBS=()
    for i in "${ALL_SAMPLE_LIBS[@]}"; do
        TESTED_SAMPLE_LIBS+=("${i::-1}")
    done
    echo "Going to test ALL of DOCA's SDK samples"
fi

# Get environment settings (we need to disable this test on CentOS 7.6 Arm)
source ./devtools/scripts/query_env_info.sh --silent
if [[ "$?" != "0" ]]; then
    exit 1
fi

# Older GCC doesn't recognize the "cortex-a72" -mcpu flag we get from DPDK
if [[ "${ARC}" == *"aarch64"* && ("${OS}" == *"CentOS"* || "${OS}" == *"Red Hat"* || "${OS}" == *"Unknown"*) && "${OS_VERSION}" == *"7."* ]]; then
   echo "Aborting test - Not supported on CentOS 7.6 Arm"
   exit 0
fi

# Learn about the lib paths - need for .pc file check
if [ "${UBUNTU_PACKAGES}" = true ]; then
	DOCA_PC_FILE_DIR="/opt/mellanox/doca/lib/${ARCH_PATH}-linux-gnu/pkgconfig"
else
	DOCA_PC_FILE_DIR="/opt/mellanox/doca/lib64/pkgconfig"
fi

# Verification API mandate we start from DOCA's root directory
# Let's now move to the samples directory
pushd "$SAMPLES_FOLDER"

for lib_dir in "${TESTED_SAMPLE_LIBS[@]}"; do
	if [[ ${skip_lib_dir_array[@]} =~ "${lib_dir}" ]]; then
		continue
	fi
	# There are some libraries with samples for the host only
	if [ "${IS_DPU}" = true ]; then
		if [[ ${skip_host_sample_dir_array[@]} =~ "${lib_dir}" ]]; then
			continue
		fi
	fi
        # Check if the library is installed (might be missing dependencies)
	pc_filename="${lib_dir//_/-}.pc"
	full_pc_path="${DOCA_PC_FILE_DIR}/${pc_filename}"
	if [ ! -f ${full_pc_path} ]; then
		echo "Library $lib_dir is not installed, skipping these samples"
		continue
	fi
	cd ${lib_dir}
	samples_array=(*/)
	for sample_dir in "${samples_array[@]}"; do
		if [[ ${skip_sample_dir_array[@]} =~ "${sample_dir////}" ]]; then
			continue
		fi
		# Enter sample directory
		cd ${sample_dir}

		if [[ "${sample_dir///}" == "plugins" ]]; then
			plugins_array=(*/)
			for plugin_dir in "${plugins_array[@]}"; do
				cd ${plugin_dir}
				plugin_name=${plugin_dir%/*}
				plugin_bin=${plugin_name}.so
				binary_check $plugin_name $plugin_bin
				cd ..
			done
		elif ! [ -f "meson.build" ]; then
			# If meson not exist then loop over subdirectories
			subsystems_array=(*/)
			for subsystem_dir in "${subsystems_array[@]}"; do
				cd ${subsystem_dir}
				sample_name=doca_${sample_dir%/*}
				sample_subsystem_name=${sample_name}_${subsystem_dir%/*}
				sample_subsystem_bin=${sample_subsystem_name}
				binary_check $sample_subsystem_name $sample_subsystem_bin
				cd ..
			done
		else
			(
				sample_name=doca_${sample_dir%/*}
				sample_bin=${sample_name}
				binary_check $sample_name $sample_bin
			)&

			# allow to execute up to $N jobs in parallel
			if [[ $(jobs -r -p | wc -l) -ge $NUM_JOBS ]]; then
				# now there are $N jobs already running, so wait here for any job
				# to be finished so there is a place to start next one.
				wait -n
			fi
		fi
		# Exit sample directory
		cd ..
	done
	cd ..
done

# no more jobs to be started but wait for pending jobs
# (all need to be finished)
wait

if [ -f "/tmp/samples_check_failed" ]; then
	echo -e "${red}Finished with errors${clear}"
	returncode=1
else
	echo -e "${green}Finished successfully${clear}"
	returncode=0
fi

rm -rf ${target_dir_prefix}*
rm -rf "/tmp/samples_check_failed"
# Verification API mandate we finish at DOCA's root directory
# Let's now return back to it
popd
exit $returncode

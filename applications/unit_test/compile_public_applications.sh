#!/usr/bin/env bash

#
# Copyright (c) 2022-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
# Test the compilation of the publicly installed DOCA SDK applications
#
# Usage:
#     ./compile_public_applications.sh ([list_of_applications.txt])
#
# An optional parameter can specify a desired SUBSET of reference applications to
# be tested. An example for such a file that focuses on a networking related reference
# applications is the following:
# "
# ipsec_security_gw
# psp_gateway
# simple_fwd_vnf
# switch
# "
#
# Note:
# If a list of applications isn't provided, ALL applications will be tested.
# If environment variable APPLICATION_FOLDER_PATH is set, will use this folder to derive all applications list
# Examples:
#  1. Test the build of ALL applications:
#         ./compile_public_applications.sh
#  2. Tets the build of only a selected set of DOCA SDK reference apps:
#         ./compile_public_applications.sh doca_networking_apps.txt
#

##################
# Configurations #
##################

shopt -s dotglob
shopt -s nullglob

target_dir=/tmp/build
app_names_array=(*/)
skip_app_dir_array=("urom_rdmo" "urom_ucc")

# List of GPU-only applications
GPU_ONLY_APPS=(
    "gpu_packet_processing"
)

DPU_ONLY_APPS=(
    "nvme_emulation"
)

##################
## Script Start ##
##################

returncode=0

# Derive a list of all known reference applications (skipped will be processed later on)
# Check if a custom applications folder parameter is provided
APPS_FOLDER="${APPLICATION_FOLDER_PATH:-/opt/mellanox/doca/applications}"

pushd "$APPS_FOLDER"
ALL_APPLICATIONS=(*/)
popd

# The user passed an applications list
if [ $# -eq 1 ]; then
    if [ -f "$1" ]; then
        readarray -t REQUESTED_APPS < $1
    else
        echo "Failed to find the requested reference applications list file: $1."
        exit 1
    fi
    # Build a list of lib directories based on the user's request
    TESTED_APPS=()
    for i in "${ALL_APPLICATIONS[@]}"; do
        for j in "${REQUESTED_APPS[@]}"; do
            if [[ ${i::-1} == $j ]]; then
                TESTED_APPS+=("${i::-1}")
                break;
            fi
        done
    done
    echo "A reference apps list was provided, going to test only the following applications:"
    printf '%s\n' "${TESTED_APPS[@]}"
else
    TESTED_APPS=()
    for i in "${ALL_APPLICATIONS[@]}"; do
        TESTED_APPS+=("${i::-1}")
    done
    echo "Going to test ALL of DOCA's reference applications"
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

# Verification API mandate we start from DOCA's root directory
# Let's now move to the application directory

pushd "$APPS_FOLDER"
# Regular compilation
rm -rf ${target_dir}
meson setup ${target_dir} -Dwarning_level=2 -Dwerror=true
returncode=$?
if [[ "$returncode" != "0" ]]; then
    echo "Failed during meson build"
    rm -rf ${target_dir}
    # Verification API mandate we finish at DOCA's root directory
    # Let's now return back to it
    popd
    exit $returncode
fi
ninja -C ${target_dir}
returncode=$?
if [[ "$returncode" != "0" ]]; then
    echo "Failed during compilation"
    rm -rf ${target_dir}
    # Verification API mandate we finish at DOCA's root directory
    # Let's now return back to it
    popd
    exit $returncode
fi

for app_name in "${TESTED_APPS[@]}"; do
    if [[ ${skip_app_dir_array[@]} =~ "${app_name}" ]]; then
        echo "Skipping ${app_name} application"
        continue
    fi
    # We are only interested in apps that can be compiled from source
    if [[ ! -f ${app_name}/meson.build ]]; then
        continue
    fi

    # Not interested in GPU Apps - We are missing needed libcuda.so.1 in our environment
    gpu_app=0
    for gpu_app in ${GPU_ONLY_APPS[@]}; do
        if [[ "$gpu_app" == "$app_name" ]]; then
            gpu_app=1
            break
        fi
    done
    if [[ "$gpu_app" == "1" ]]; then
        continue
    fi

    # Not interested in DPU Only Apps - We need this script for both environments (Host & DPU)
    dpu_app=0
    for dpu_app in ${DPU_ONLY_APPS[@]}; do
        if [[ "$dpu_app" == "$app_name" ]]; then
            dpu_app=1
            break
        fi
    done
    if [[ "$dpu_app" == "1" ]]; then
        continue
    fi

    bin_dir=${target_dir}/${app_name}
    # We always have 1 for dependencies/meson.build, and we need more for a real compilation
    num_built_files=$(ls -1q ${bin_dir}* | wc -l)
    if [[ "${num_built_files}" == "1" ]]; then
        echo "${app_name} executable is missing, probably a missing build dependency"
        continue
    fi

    bin_path=${bin_dir}/doca_${app_name}
    bin_names=($(find ${bin_dir} -executable -type f -name "doca_${app_name}*"))
    if [[ ${#bin_names[@]} == 0 ]]; then
        echo "${app_name} executable is missing! (probably named incorrectly)"
        returncode=1
        continue
    fi

    for element in "${bin_names[@]}"
    do
        if ldd ${element} | grep "not found"; then
            echo "${app_name} executable has missing libraries!"
            returncode=1
        fi
    done
done

if [[ "$returncode" == "0" ]]; then
    echo "Finished successfully"
fi

rm -rf ${target_dir}
# Verification API mandate we finish at DOCA's root directory
# Let's now return back to it
popd
exit $returncode

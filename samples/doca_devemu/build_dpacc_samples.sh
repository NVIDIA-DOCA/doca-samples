#!/bin/bash

#
# Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

set -e

# This script uses the dpacc tool (located in /opt/mellanox/doca/tools/dpacc) to compile DPA kernels device code.
# This script takes 4 arguments:
# arg1: The project's build path (for the DPA Device build)
# arg2: Install directory for DOCA's device libraries
# arg3: Install directory for DOCA's include files
# arg4: Absolute paths of all DPA (kernel) device source code *files* (our code)
# arg5: The sample name
# arg6: The output DPACC sample program name

####################
## Configurations ##
####################

PROJECT_BUILD_DIR=$1
DOCA_LIB_DIR=$2
DOCA_INCLUDE_DIR=$3
DPA_KERNELS_DEVICE_SRC=$4
SAMPLE_NAME=$5
SAMPLE_PROGRAM_NAME=$6
DPACC_MCPU_FLAG=$7
SAMPLE_ATTRIBUTES=$8

# DOCA Configurations
DOCA_DPACC="/opt/mellanox/doca/tools/dpacc"
DOCA_APP_ATTRIBUTES2BLOB="/opt/mellanox/doca/tools/dpa-app-attributes2blob"

HOST_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra -DFLEXIO_ALLOW_EXPERIMENTAL_API"
DEVICE_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra -DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API"

# DOCA DPA APP Configuration
# This variable name passed to DPACC with --app-name parameter and it's token must be identical to the
# struct doca_dpa_app parameter passed to doca_dpa_set_app(), i.e.
# doca_error_t doca_dpa_set_app(..., struct doca_dpa_app *${DPA_APP_NAME});
DPA_APP_NAME="devemu_pci_sample_app"

##################
## Script Start ##
##################

# Build directory for the DPA device (kernel) code
SAMPLE_DEVICE_BUILD_DIR="${PROJECT_BUILD_DIR}/${SAMPLE_NAME}/dpu/device/build_dpacc"
SAMPLE_ATTRIBUTES_BLOB="${SAMPLE_DEVICE_BUILD_DIR}/${SAMPLE_NAME}_attributes.blob"

rm -rf ${SAMPLE_DEVICE_BUILD_DIR}
mkdir -p ${SAMPLE_DEVICE_BUILD_DIR}

# Generate blob from device attributes file
$DOCA_APP_ATTRIBUTES2BLOB ${SAMPLE_ATTRIBUTES} ${SAMPLE_ATTRIBUTES_BLOB}

# Compile the DPA (kernel) device source code using the DPACC
$DOCA_DPACC $DPA_KERNELS_DEVICE_SRC \
	-o ${SAMPLE_DEVICE_BUILD_DIR}/${SAMPLE_PROGRAM_NAME}.a \
	-mcpu=${DPACC_MCPU_FLAG} \
	-hostcc=gcc \
	-hostcc-options="${HOST_CC_FLAGS}" \
	--devicecc-options="${DEVICE_CC_FLAGS}" \
	--app-name="${DPA_APP_NAME}" \
	-device-libs="-L${DOCA_LIB_DIR} -ldoca_dpa_dev -ldoca_dpa_dev_comm" \
	-flto \
	-I${DOCA_INCLUDE_DIR} \
	--dpa-proc-attr="${SAMPLE_ATTRIBUTES_BLOB}"

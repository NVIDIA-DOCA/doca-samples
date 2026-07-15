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

######################
## Script Arguments ##
######################

# This script uses the dpacc tool (located in /opt/mellanox/doca/tools/dpacc) to compile DPA kernels device code.
# This script takes 6 arguments:
# arg1: The DPA compiler include flags
# arg2: The output directory where the DPA binary should be generated
# arg3: DPACC MCPU flag
# arg4: The DPA compiler link flags
# arg5: The DPA application name
# arg6: The output DPA compiler application binary program name
# arg7+: List of DPA source files

DPA_INCLUDE_FLAGS="$1"
OUTPUT_DIR=$2
DPACC_MCPU_FLAG=$3
DPA_LINK_FLAGS=$4
DPA_APP_NAME=$5
APPLICATION_DPA_ATTRIBUTES=$6
OUTPUT_ARCHIVE_NAME=$7
DPA_SOURCE_FILES="${@:8}"

####################
## Configurations ##
####################

DOCA_DPACC="/opt/mellanox/doca/tools/dpacc"
DPA_APP_ATTRIBUTES2BLOB="/opt/mellanox/doca/tools/dpa-app-attributes2blob"

HOST_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra -DFLEXIO_ALLOW_EXPERIMENTAL_API"
DEVICE_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra -DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API"
DEVICE_CC_FLAGS="-MMD -MT ${OUTPUT_ARCHIVE_NAME} ${DEVICE_CC_FLAGS}"
APPLICATION_DPA_ATTRIBUTES_BLOB=`pwd`"/${OUTPUT_DIR}/${DPA_APP_NAME}_attributes.blob"

# Generate blob from device attributes file
$DPA_APP_ATTRIBUTES2BLOB ${APPLICATION_DPA_ATTRIBUTES} ${APPLICATION_DPA_ATTRIBUTES_BLOB}

${DOCA_DPACC} \
	${DPA_SOURCE_FILES} \
	-o "${OUTPUT_ARCHIVE_NAME}" \
	-mcpu=${DPACC_MCPU_FLAG} \
	-hostcc=gcc \
	-hostcc-options="${HOST_CC_FLAGS}" \
	--devicecc-options="${DEVICE_CC_FLAGS}" \
	-device-libs="${DPA_LINK_FLAGS}" \
	--app-name=${DPA_APP_NAME} \
	${DPA_INCLUDE_FLAGS} \
	-flto \
	-disable-asm-checks \
	--keep-dir ${OUTPUT_DIR} \
	--dpa-proc-attr="${APPLICATION_DPA_ATTRIBUTES_BLOB}"

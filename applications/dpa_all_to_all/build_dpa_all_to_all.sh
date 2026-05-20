#!/bin/bash

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

set -e

# This script uses the mpicc (MPI C compiler) to compile the dpa_all_to_all application
# This script takes 5 arguments:
# arg1: The DOCA directory path
# arg2: The project's build path
# arg3: DOCA version
# arg4: Address sanitizer option
# arg5: Buildtype
# arg6: Is it an amalgamation build mode (true) or fine-grained build mode (false)

####################
## Configurations ##
####################

APP_NAME="dpa_all_to_all"
MPI_COMPILER="mpicc"

# DOCA Configurations
DOCA_DIR=$1
DOCA_BUILD_DIR=$2
DOCA_VERSION=$3
ADDRESS_SANITIZER_OPTION=$4
BUILD_TYPE=$5
AMALGAMATION_BUILD_MODE=$6
DPACC_MCPU_FLAG=$7
DOCA_INCLUDE="/opt/mellanox/doca/include"
ALL_TO_ALL_DIR="${DOCA_DIR}/applications/$APP_NAME"
DOCA_LIBS_DIR="${DOCA_DIR}/libs"
DPA_DIR="${DOCA_LIBS_DIR}/doca_dpa"
DPA_SRC_DIR="${DPA_DIR}/src"
DOCA_COMMON_DIR="${DOCA_LIBS_DIR}/doca_common/"
DOCA_RDMA_DIR="${DOCA_LIBS_DIR}/doca_rdma/include/public/"
DOCA_LOG_DIR="${DOCA_LIBS_DIR}/doca_common/log/"
DOCA_ARGP_DIR="${DOCA_LIBS_DIR}/doca_argp/include/public/"
DOCA_ERR_INC="${DOCA_LIBS_DIR}/doca_common/core/include/public/"
DOCA_VER_INC="${DOCA_LIBS_DIR}/doca_common/version"
DOCA_DPA_DEV_BUILD_DIR="${DOCA_BUILD_DIR}/libs/doca_dpa/src/device/build/"
ALL_TO_ALL_HOST_DIR="${ALL_TO_ALL_DIR}/host"
ALL_TO_ALL_HOST_SRC_FILES="${ALL_TO_ALL_HOST_DIR}/${APP_NAME}.c $ALL_TO_ALL_HOST_DIR/${APP_NAME}_core.c"
ALL_TO_ALL_DEVICE_SRC_DIR="${ALL_TO_ALL_DIR}/device"
ALL_TO_ALL_DEVICE_SRC_FILES="${ALL_TO_ALL_DEVICE_SRC_DIR}/${APP_NAME}_dev.c"
ALL_TO_ALL_DEVICE_ATTRIBUTES="${ALL_TO_ALL_DEVICE_SRC_DIR}/${APP_NAME}_attributes.yaml"
ALL_TO_ALL_APP_EXE="${DOCA_BUILD_DIR}/applications/${APP_NAME}/doca_${APP_NAME}"
DEVICE_CODE_BUILD_SCRIPT="${ALL_TO_ALL_DIR}/build_device_code.sh"
DEVICE_CODE_LIB="${DOCA_BUILD_DIR}/applications/${APP_NAME}/device/build_dpacc/${APP_NAME}_program.a"

# Finalize includes and flags
if [ ${AMALGAMATION_BUILD_MODE} = "true" ]; then
	DOCA_INC_LIST="-I$DOCA_COMMON_DIR -I$DOCA_RDMA_DIR -I${DPA_DIR}/include/public/ -I${DOCA_BUILD_DIR}/configs/ -I${DOCA_VER_INC} \
		-I$DOCA_ERR_INC -I$DOCA_LOG_DIR -I$DOCA_ARGP_DIR"
else
	DOCA_INC_LIST="-I${DOCA_INCLUDE}"
fi

CC_FLAGS="-Werror -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter"
LINK_FLAGS="-pthread -lm -lflexio -lstdc++ -libverbs -lmlx5 -lbsd"

# If address sanitizer option is not none then add it to the link flags
if [ "$ADDRESS_SANITIZER_OPTION" != "none" ]; then
	LINK_FLAGS="${LINK_FLAGS} -fsanitize=${ADDRESS_SANITIZER_OPTION}"
fi

# If compile in debug mode add -g flag
if [ "$BUILD_TYPE" != "none" ]; then
	LINK_FLAGS="${LINK_FLAGS} -g"
fi

# DOCA Link Flags
if [ ${AMALGAMATION_BUILD_MODE} = "true" ]; then
	DOCA_LINK_FLAGS="-Wl,-rpath,$DOCA_BUILD_DIR/libs -Wl,--as-needed -Wl,-rpath-link,${DOCA_BUILD_DIR}/libs \
		-Wl,--start-group ${DOCA_BUILD_DIR}/libs/libdoca_common.so.${DOCA_VERSION} \
		${DOCA_BUILD_DIR}/libs/libdoca_argp.so.${DOCA_VERSION} ${DOCA_BUILD_DIR}/libs/libdoca_dpa.so.${DOCA_VERSION} \
		${DOCA_BUILD_DIR}/libs/libdoca_rdma.so.${DOCA_VERSION} \
		-Wl,--end-group"
else
	DOCA_LINK_FLAGS=`pkg-config --libs doca-common doca-argp doca-dpa doca-rdma`
fi

DOCA_FLAGS="-DDOCA_ALLOW_EXPERIMENTAL_API"

# FlexIO Configurations
MLNX_INSTALL_PATH="/opt/mellanox/"
FLEXIO_LIBS_DIR="${MLNX_INSTALL_PATH}/flexio/lib/"

##################
## Script Start ##
##################

# Compile device code
/bin/bash $DEVICE_CODE_BUILD_SCRIPT $DOCA_DIR $DOCA_BUILD_DIR $ALL_TO_ALL_DEVICE_SRC_FILES $AMALGAMATION_BUILD_MODE $DPACC_MCPU_FLAG $ALL_TO_ALL_DEVICE_ATTRIBUTES

# Compile application using MPI compiler
$MPI_COMPILER $ALL_TO_ALL_HOST_SRC_FILES -o $ALL_TO_ALL_APP_EXE $DEVICE_CODE_LIB -I$ALL_TO_ALL_HOST_DIR \
	$DOCA_INC_LIST -L$FLEXIO_LIBS_DIR $CC_FLAGS $DOCA_FLAGS $DOCA_LINK_FLAGS $LINK_FLAGS

#!/bin/bash

#
# Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

# This script uses the mpicc (MPI C compiler) to compile the ping pong sample
# This script takes 4 arguments:
# arg1: The DOCA directory path
# arg2: The project's build path
# arg3: DOCA version
# arg4: Address sanitizer option
# arg5: Debug build option
####################
## Configurations ##
####################

APP_NAME="urom_ping_pong"
MPI_COMPILER="mpicc"

# DOCA Configurations
DOCA_DIR=$1
DOCA_BUILD_DIR=$2
DOCA_VERSION=$3
ADDRESS_SANITIZER_OPTION=$4
BUILD_TYPE=$5
UROM_PING_PONG_DIR="${DOCA_DIR}/samples/doca_urom/urom_ping_pong"
DOCA_LIBS_DIR="${DOCA_DIR}/libs"
UROM_DIR="${DOCA_LIBS_DIR}/doca_urom"
DOCA_COMMON_DIR="${DOCA_LIBS_DIR}/doca_common/"
DOCA_LOG_DIR="${DOCA_LIBS_DIR}/doca_common/log/"
DOCA_ARGP_DIR="${DOCA_LIBS_DIR}/doca_argp/include/public/"
DOCA_ERR_INC="${DOCA_LIBS_DIR}/doca_common/core/include/public/"
DOCA_VER_INC="${DOCA_LIBS_DIR}/doca_common/version"
DOCA_UROM_SAMPLES_COMMON="${DOCA_DIR}/samples/doca_urom/"
DOCA_SAMPLES_COMMON="${DOCA_DIR}/samples/"
UROM_PING_PONG_SRC_FILES="${UROM_PING_PONG_DIR}/${APP_NAME}_sample.c ${UROM_PING_PONG_DIR}/${APP_NAME}_main.c"
UROM_PING_PONG_COMMON_SRC_FILES="${DOCA_DIR}/samples/common.c ${DOCA_DIR}/samples/doca_urom/urom_common.c"
UROM_PING_PONG_SAMPLE_EXE="${DOCA_BUILD_DIR}/samples/doca_urom_ping_pong"
UROM_API_PUBLIC="${DOCA_LIBS_DIR}/doca_urom/core/include/public"
UROM_API_PRIVATE="${DOCA_LIBS_DIR}/doca_urom/core/include/private"
UROM_API_UTILS="${DOCA_LIBS_DIR}/doca_urom/core/include/utils"
SANDBOX_API="${DOCA_DIR}/samples/doca_urom/plugins/worker_sandbox/host/"
SANDBOX_COMMON_API="${DOCA_DIR}/samples/doca_urom/plugins/worker_sandbox/common/"
SANDBOX_SRC_FILES="${DOCA_DIR}/samples/doca_urom/plugins/worker_sandbox/host/worker_sandbox.c"

# Finalize includes and flags
DOCA_INC_LIST="-I${DOCA_VER_INC} -I$DOCA_ERR_INC -I${DOCA_BUILD_DIR}/configs/ -I$DOCA_LOG_DIR -I$DOCA_ARGP_DIR \
		-I$DOCA_SAMPLES_COMMON -I$DOCA_UROM_SAMPLES_COMMON -I$DOCA_COMMON_DIR -I$UROM_API_PUBLIC \
		-I$UROM_API_PRIVATE -I$UROM_API_UTILS -I$SANDBOX_API -I$SANDBOX_COMMON_API"
CC_FLAGS="-Werror -Wall -Wextra"
LINK_FLAGS="-pthread -lm -lstdc++ -libverbs -lmlx5 -lbsd -lucp -lucm -lucs -lc"

# If address sanitizer option is not none then add it to the link flags
if [ "$ADDRESS_SANITIZER_OPTION" != "none" ]; then
	LINK_FLAGS="${LINK_FLAGS} -fsanitize=${ADDRESS_SANITIZER_OPTION}"
fi

# If compile in debug mode add -g flag
if [ "$BUILD_TYPE" != "none" ]; then
	LINK_FLAGS="${LINK_FLAGS} -g"
fi

DOCA_LINK_FLAGS="-Wl,-rpath,$DOCA_BUILD_DIR/libs:$SANDBOX_BUILD \
		-Wl,--as-needed -Wl,-rpath-link,${DOCA_BUILD_DIR}/libs:$SANDBOX_BUILD \
		-Wl,--start-group ${DOCA_BUILD_DIR}/libs/libdoca_common.so.${DOCA_VERSION} \
		${DOCA_BUILD_DIR}/libs/libdoca_argp.so.${DOCA_VERSION} \
		${DOCA_BUILD_DIR}/libs/libdoca_urom.so.${DOCA_VERSION} -Wl,--end-group"
DOCA_FLAGS="-DDOCA_ALLOW_EXPERIMENTAL_API"

##################
## Script Start ##
##################

# Compile application using MPI compiler
$MPI_COMPILER $UROM_PING_PONG_SRC_FILES $UROM_PING_PONG_COMMON_SRC_FILES $SANDBOX_SRC_FILES \
	-o $UROM_PING_PONG_SAMPLE_EXE $DOCA_INC_LIST $CC_FLAGS $DOCA_FLAGS $DOCA_LINK_FLAGS $LINK_FLAGS

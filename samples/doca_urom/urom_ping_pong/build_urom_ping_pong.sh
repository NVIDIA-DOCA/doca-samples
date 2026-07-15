#!/bin/bash

#
# Copyright (c) 2023-2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
# arg1: The project's build path
# arg2: Address sanitizer option
# arg3: The installed DOCA lib dir
# arg4: Debug build option
####################
## Configurations ##
####################

APP_NAME="urom_ping_pong"
MPI_COMPILER="mpicc"

# DOCA Configurations
DOCA_DIR="/opt/mellanox/doca"
DOCA_BUILD_DIR=$1
ADDRESS_SANITIZER_OPTION=$2
DOCA_LIBS_DIR=$3
BUILD_TYPE=$4
DOCA_INCLUDE="${DOCA_DIR}/include"
DOCA_SAMPLES_INCLUDE="${DOCA_DIR}/samples"
DOCA_UROM_SAMPLS_INCLUDE="${DOCA_DIR}/samples/doca_urom"
UROM_PING_PONG_DIR="${DOCA_DIR}/samples/doca_urom/urom_ping_pong"
UROM_PING_PONG_SRC_FILES="${UROM_PING_PONG_DIR}/${APP_NAME}_sample.c ${UROM_PING_PONG_DIR}/${APP_NAME}_main.c"
UROM_PING_PONG_COMMON_SRC_FILES="${DOCA_DIR}/samples/common.c ${DOCA_DIR}/samples/doca_urom/urom_common.c"
UROM_PING_PONG_SAMPLE_EXE="${DOCA_BUILD_DIR}/doca_urom_ping_pong"
SANDBOX_WORKER_PLUGIN_DIR="${DOCA_DIR}/samples/doca_urom/plugins/worker_sandbox"
SANDBOX_WORKER_PLUGIN_SRC_FILES="${SANDBOX_WORKER_PLUGIN_DIR}/worker_sandbox.c "
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

DOCA_FLAGS="-DDOCA_ALLOW_EXPERIMENTAL_API"
DOCA_LINK_FLAGS=`pkg-config --libs doca-common doca-urom doca-argp`

##################
## Script Start ##
##################

# Compile application using MPI compiler
$MPI_COMPILER $UROM_PING_PONG_SRC_FILES $UROM_PING_PONG_COMMON_SRC_FILES $SANDBOX_WORKER_PLUGIN_SRC_FILES \
	-o $UROM_PING_PONG_SAMPLE_EXE -I$DOCA_INCLUDE -I$DOCA_UROM_SAMPLS_INCLUDE -I$SANDBOX_WORKER_PLUGIN_DIR \
	-I$DOCA_SAMPLES_INCLUDE $CC_FLAGS $DOCA_FLAGS $DOCA_LINK_FLAGS $LINK_FLAGS

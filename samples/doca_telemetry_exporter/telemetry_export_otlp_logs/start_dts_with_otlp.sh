#!/bin/bash

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

# Script to start DOCA Telemetry Service (DTS) with OTLP exporter configured
# DTS will receive telemetry via IPC and forward to OTLP collector

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck disable=SC1090,SC1091
source "${SCRIPT_DIR}/otlp_config.sh"

DEFAULT_DTS_ROOT="/opt/mellanox/doca/services/telemetry"
DEFAULT_CLX_API_LOG_LEVEL=7
DEFAULT_DTS_STARTUP_WAIT_SEC=3

echo "=========================================="
echo "  DOCA Telemetry Service (DTS) Setup"
echo "=========================================="

image=${1:-$IMAGE}
if [ -z "$image" ]; then
    echo "ERROR: DTS container image is required (use the official DOCA Telemetry Service image)."
    echo "Usage: IMAGE=<image> $0"
    echo "   or: $0 <image>"
    exit 1
fi
CMD=${CMD:-docker}
CLX_API_LOG_LEVEL=${CLX_API_LOG_LEVEL:-$DEFAULT_CLX_API_LOG_LEVEL}
DTS_STARTUP_WAIT_SEC=${DTS_STARTUP_WAIT_SEC:-$DEFAULT_DTS_STARTUP_WAIT_SEC}

CONTAINER_NAME="doca_telemetry_otlp_${USER}"
DTS_ROOT=${DTS_ROOT:-$DEFAULT_DTS_ROOT}
IPC_SOCKETS_DIR="${DTS_ROOT}/ipc_sockets"

echo "Creating DTS directories at: ${DTS_ROOT}"
mkdir -p "${IPC_SOCKETS_DIR}"
mkdir -p "${DTS_ROOT}/data"
mkdir -p "${DTS_ROOT}/config"
mkdir -p "${DTS_ROOT}/log"

container_args=""
container_args+="--net host "
container_args+="--privileged "
container_args+="-v ${IPC_SOCKETS_DIR}:/tmp/ipc_sockets "
container_args+="-v ${DTS_ROOT}/data:/data "
container_args+="-v ${DTS_ROOT}/config:/config "
container_args+="-v ${DTS_ROOT}/log:/var/log "

container_args+="-e CLX_OPEN_TELEMETRY_RECEIVER=http://0.0.0.0:${OTLP_LOGS_PORT}/v1/logs "
container_args+="-e CLX_OPEN_TELEMETRY_TRANSPORT=http "
container_args+="-e CLX_OPEN_TELEMETRY_FAST_SERIALIZER=true "
container_args+="-e CLX_OPEN_TELEMETRY_DUMP_PAYLOAD=true "
container_args+="-e CLX_API_LOG_LEVEL=${CLX_API_LOG_LEVEL} "

init_args="${container_args} --rm --name ${CONTAINER_NAME} "
daemon_args="${container_args} --name ${CONTAINER_NAME} "

echo "Cleaning up previous DTS container..."
$CMD stop "$CONTAINER_NAME" &> /dev/null || true
$CMD rm -f "$CONTAINER_NAME" &> /dev/null || true

echo "Initializing DTS..."
$CMD run ${init_args} "$image" /bin/bash -c "DTS_CONFIG_DIR=host /usr/bin/telemetry-init.sh"

echo "Starting DTS container with OTLP exporter..."
$CMD run -d ${daemon_args} "$image" /usr/bin/telemetry-run.sh

echo "Waiting for DTS to start..."
sleep "${DTS_STARTUP_WAIT_SEC}"

if ! $CMD ps | grep -q "$CONTAINER_NAME"; then
    echo "ERROR: DTS failed to start!"
    $CMD logs "$CONTAINER_NAME"
    exit 1
fi

echo ""
echo "=========================================="
echo "  ✓ DTS Running with OTLP Configured"
echo "=========================================="
echo "  Container: $CONTAINER_NAME"
echo "  Image: $image"
echo "  IPC sockets: ${IPC_SOCKETS_DIR}"
echo "  Data dir: ${DTS_ROOT}/data"
echo "  Log dir: ${DTS_ROOT}/log"
echo "  OTLP target: http://0.0.0.0:${OTLP_LOGS_PORT}/v1/logs"
echo "=========================================="
echo ""
echo "DTS is now ready to receive telemetry via IPC"
echo "and forward it to the OTLP collector."
echo ""
echo "To view DTS logs:"
echo "  $CMD logs -f $CONTAINER_NAME"
echo ""
echo "To stop DTS:"
echo "  $CMD stop $CONTAINER_NAME"
echo "  $CMD rm $CONTAINER_NAME"

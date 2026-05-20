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

# Script to run DOCA OTLP Logs sample
#
# This script supports TWO MODES:
#
# MODE 1 (Default): Direct OTLP HTTP Export
#   - Application sends OTLP logs directly to OpenTelemetry Collector via HTTP
#   - Usage: ./run_sample.sh
#   - Prerequisites: Start OpenTelemetry Collector first with ./start_otlp_collector.sh
#
# MODE 2: IPC to DTS (DOCA Telemetry Service)
#   - Application sends telemetry to DTS via IPC sockets
#   - DTS then forwards to OpenTelemetry Collector
#   - Usage: ENABLE_IPC=1 ./run_sample.sh
#   - Prerequisites:
#     1. Start OpenTelemetry Collector: ./start_otlp_collector.sh
#     2. Start DTS with OTLP config
#     3. Run sample with IPC: ENABLE_IPC=1 ./run_sample.sh

set -e

# Load shared configuration
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck disable=SC1090,SC1091
source "${SCRIPT_DIR}/otlp_config.sh"

# Check if IPC mode is enabled
IPC_MODE=${ENABLE_IPC:-0}

if [ "$IPC_MODE" = "1" ]; then
    echo "=========================================="
    echo "  DOCA OTLP Logs Sample (IPC Mode)"
    echo "=========================================="
else
    echo "=========================================="
    echo "  DOCA OTLP Logs Sample (Direct OTLP)"
    echo "=========================================="
fi

# Set library path for Collectx libraries
if [ -d "/opt/mellanox/collectx/lib" ]; then
    export LD_LIBRARY_PATH="/opt/mellanox/collectx/lib:${LD_LIBRARY_PATH}"
    echo "Library path set: /opt/mellanox/collectx/lib"
else
    echo "WARNING: Collectx library not found at /opt/mellanox/collectx/lib"
    echo "Please install collectx-clxapi package"
fi

if [ "$IPC_MODE" = "1" ]; then
    # IPC Mode: Send telemetry to DTS via IPC sockets
    # OTLP configuration is done in DTS container, not here
    export ENABLE_IPC=1

    echo "IPC mode enabled"
    echo "  IPC sockets: /opt/mellanox/doca/services/telemetry/ipc_sockets"
    echo "  → Data flows to DTS container"
    echo "  → DTS forwards to OTLP collector at http://0.0.0.0:${OTLP_LOGS_PORT}/v1/logs"
    echo ""
    echo "Note: Make sure DTS is running (./start_dts_with_otlp.sh)"
    echo ""
else
    # Direct OTLP Mode: Send directly to OpenTelemetry Collector via HTTP
    export CLX_OPEN_TELEMETRY_RECEIVER="http://0.0.0.0:${OTLP_LOGS_PORT}/v1/logs"
    export CLX_OPEN_TELEMETRY_TRANSPORT="http"
    export CLX_OPEN_TELEMETRY_FAST_SERIALIZER="true"
    export CLX_OPEN_TELEMETRY_DUMP_PAYLOAD="true"
    export CLX_API_LOG_LEVEL=7

    # Create debug directory for JSON files
    DEBUG_JSON_DIR="/tmp/doca_otlp_logs_debug_json"
    mkdir -p "${DEBUG_JSON_DIR}"
    rm -f "${DEBUG_JSON_DIR}"/*.json 2>/dev/null || true
    export CLX_OTLP_LOGS_DEBUG_JSON_PATH="${DEBUG_JSON_DIR}"

    echo "Direct OTLP exporter configured"
    echo "  Target: ${CLX_OPEN_TELEMETRY_RECEIVER}"
    echo "  Debug JSON: ${DEBUG_JSON_DIR}"
    echo ""
fi

# Find the binary
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DOCA_ROOT="$( cd "${SCRIPT_DIR}/../../.." && pwd )"
SAMPLE_BIN="${DOCA_ROOT}/build/samples/doca_telemetry_export_otlp_logs"

# Check if binary exists
if [ ! -f "${SAMPLE_BIN}" ]; then
    echo "ERROR: Sample binary not found at: ${SAMPLE_BIN}"
    echo "Please build the sample first:"
    echo "  cd ${DOCA_ROOT}"
    echo "  ninja -C build doca_telemetry_export_otlp_logs"
    exit 1
fi

echo "Running DOCA OTLP Logs sample..."
echo "Binary: ${SAMPLE_BIN}"
echo "=========================================="
echo ""

# Run the sample with all CLX environment variables at execution time
"${SAMPLE_BIN}"

RESULT=$?

echo ""
echo "=========================================="
if [ $RESULT -eq 0 ]; then
    echo " Sample completed successfully!"
else
    echo " Sample failed with exit code: $RESULT"
fi
echo "=========================================="
echo ""

if [ "$IPC_MODE" = "1" ]; then
    # IPC Mode - show DTS and collector logs
    echo "To view DTS logs:"
    echo "  docker logs doca_telemetry_otlp_${USER}"
    echo ""
    echo "To view OTLP collector logs:"
    echo "  docker logs ${USER}_otlp_collector"
    echo ""
else
    # Direct OTLP Mode - show debug JSON and collector logs
    echo "Debug JSON files: ${DEBUG_JSON_DIR}/*.json"
    echo ""
    echo "To view collector logs:"
    echo "  docker logs ${USER}_otlp_collector"
    echo ""
fi

echo "To view output file:"
echo "  cat /tmp/doca_otlp_logs_output.jsonl"
echo ""
echo "TIP: To run in IPC mode instead:"
echo "  ENABLE_IPC=1 ./run_sample.sh"

exit $RESULT

#!/bin/bash

#
# Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

# Script to start OpenTelemetry Collector
# This receiver will collect OTLP logs from the DOCA sample

set -e

# Load shared configuration
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck disable=SC1090,SC1091
source "${SCRIPT_DIR}/otlp_config.sh"

echo "=========================================="
echo "  OpenTelemetry Collector Setup"
echo "=========================================="

# Cleanup previous collector
echo "Cleaning up previous collector..."
docker stop "${USER}_otlp_collector" 2>/dev/null || true
docker rm -f "${USER}_otlp_collector" 2>/dev/null || true

# Create OpenTelemetry Collector configuration file
echo "Creating OpenTelemetry Collector configuration..."
cat <<EOL > /tmp/doca_otlp_logs_collector.yaml
receivers:
  otlp:
    protocols:
      http:
        endpoint: 0.0.0.0:${OTLP_LOGS_PORT}
      grpc:
        endpoint: 0.0.0.0:${OTLP_LOGS_GRPC_PORT}

processors:
  batch:
    timeout: 100ms
    send_batch_size: 1
    send_batch_max_size: 1

exporters:
  debug:
    verbosity: detailed
    sampling_initial: 5
    sampling_thereafter: 1

  file:
    path: /tmp/doca_otlp_logs_output.jsonl
    format: json

service:
  telemetry:
    logs:
      level: "debug"

  pipelines:
    logs:
      receivers: [otlp]
      processors: [batch]
      exporters: [debug, file]
EOL

# Run OpenTelemetry Collector container
echo "Starting OpenTelemetry Collector container..."
docker run -d \
  --name "${USER}_otlp_collector" \
  --network host \
  -v /tmp/doca_otlp_logs_collector.yaml:/etc/otel/config.yaml \
  -v /tmp:/tmp \
  otel/opentelemetry-collector-contrib:latest \
  --config /etc/otel/config.yaml

echo "Waiting for OpenTelemetry Collector to start..."
sleep 3

# Check if collector is running
if ! docker ps | grep -q "${USER}_otlp_collector"; then
    echo "ERROR: OpenTelemetry Collector failed to start!"
    docker logs "${USER}_otlp_collector"
    exit 1
fi

echo ""
echo "=========================================="
echo "  ✓ OpenTelemetry Collector Running"
echo "=========================================="
echo "  HTTP endpoint: http://localhost:${OTLP_LOGS_PORT}/v1/logs"
echo "  gRPC endpoint: localhost:${OTLP_LOGS_GRPC_PORT}"
echo "  Metrics: http://localhost:${OTLP_DEBUG_PORT}/metrics"
echo "  Output file: /tmp/doca_otlp_logs_output.jsonl"
echo "=========================================="
echo ""
echo "Now run the sender script inside your Docker container:"
echo "  sudo docker exec -it <container> /bin/bash"
echo "  cd /doca/samples/doca_telemetry_exporter/telemetry_export_otlp_logs"
echo "  ./run_sample.sh"
echo ""
echo "To view logs in real-time:"
echo "  docker logs -f ${USER}_otlp_collector"
echo ""
echo "To stop the collector:"
echo "  docker stop ${USER}_otlp_collector"

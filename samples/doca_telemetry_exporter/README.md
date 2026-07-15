# DOCA Telemetry Exporter Samples

These samples demonstrate the usage of the DOCA Telemetry Exporter API, including custom telemetry schema and NetFlow functionalities.

## Telemetry Export

This sample illustrates how to use the telemetry exporter API with a custom schema.

### Sample Logic:
1. Configuring schema attributes.
2. Initializing the schema.
3. Creating a telemetry exporter source.
4. Creating example events.
5. Reporting the example events via DOCA Telemetry Exporter.
6. Destroying the source and schema.

### References:
- `telemetry_export/telemetry_export_sample.c`
- `telemetry_export/telemetry_export_main.c`
- `telemetry_export/meson.build`

---

## Telemetry Export NetFlow

This sample demonstrates how to use the NetFlow functionality of the telemetry exporter API.

### Sample Logic:
1. Configuring NetFlow attributes.
2. Initializing NetFlow.
3. Creating a telemetry exporter source.
4. Starting NetFlow.
5. Creating example events.
6. Reporting the example events via DOCA Telemetry Exporter.
7. Destroying NetFlow.

### References:
- `telemetry_export_netflow/telemetry_export_netflow_sample.c`
- `telemetry_export_netflow/telemetry_export_netflow_main.c`
- `telemetry_export_netflowt/meson.build`

---

## Telemetry Export Metrics

The metrics sample showcases the DOCA Metrics API that is now part of the telemetry exporter. It
demonstrates how an application can emit labeled counters, gauges, and histograms while reusing the
same schema/source workflow as the classic event exporter.

### Capabilities
- **Metrics context lifecycle** – create, configure, flush, and destroy metrics state per source.
- **Label management** – mix constant labels (cluster/app identifiers) with per-series label sets.
- **Rich metric types** – emit absolute counters, incremental counters, double/uint64 gauges, and
  high-cardinality histograms with optional auto-flush policies.
- **Exporter fan-out** – write to binary files, stream over IPC to DTS, or expose a Prometheus
  endpoint (set `PROMETHEUS_ENDPOINT` before running the sample).
- **Rate control** – throttle noisy series via `doca_telemetry_exporter_metrics_set_min_sample_interval_us`
  and tune flush cadence with `doca_telemetry_exporter_metrics_set_flush_interval_ms`.

### Sample Flow
1. Initialize a telemetry schema (`doca_telemetry_exporter_schema_init`) and enable the desired
   exporters (file, IPC, Prometheus).
2. Create and start a source, giving it a unique ID/tag so metrics can be attributed per component.
3. Create a metrics context on the started source, add constant labels, and register dynamic label
   names that describe the monitored entities (interfaces in the sample).
4. For each monitoring iteration, acquire a timestamp with
   `doca_telemetry_exporter_get_timestamp`, then emit metric points:
   - Absolute counters via `doca_telemetry_exporter_metrics_add_counter`
   - Incremental counters via `doca_telemetry_exporter_metrics_add_counter_increment`
   - Gauges via `doca_telemetry_exporter_metrics_add_gauge` or `_add_gauge_uint64`
   - Histograms via `doca_telemetry_exporter_metrics_add_histogram` or base histogram helpers
5. Periodically flush buffered metrics (`doca_telemetry_exporter_metrics_flush`) or rely on the
   configured automatic interval.
6. Destroy the metrics context, then tear down the source and schema.

### Metrics Concepts

**Context & Flush Control**
- `doca_telemetry_exporter_metrics_create_context` / `_destroy_context` – bind metrics to a source.
- `doca_telemetry_exporter_metrics_set_flush_interval_ms` – align exporter flushes with monitoring
  cadence.
- `doca_telemetry_exporter_metrics_set_histogram_flush_interval_target` and
  `_histogram_flush` – keep bucketed data bounded in memory.

**Labels**
- Constant labels describe deployment-wide attributes once (e.g., hostname, service, pod).
- Label sets (registered through `doca_telemetry_exporter_metrics_add_label_names`) define the shape
  of dynamic dimensions; each metric point supplies an ordered list of label values that matches the
  set.

**Metric Types**

| Type        | API Entry Point                                             | Notes                                                  |
|-------------|-------------------------------------------------------------|--------------------------------------------------------|
| Counter     | `doca_telemetry_exporter_metrics_add_counter`               | Sends absolute totals (monotonic).                     |
| Counter Δ   | `doca_telemetry_exporter_metrics_add_counter_increment`     | Optimized when only the delta since last flush matters.|
| Gauge (f64) | `doca_telemetry_exporter_metrics_add_gauge`                 | Floating-point instantaneous values (bandwidth, temp). |
| Gauge (u64) | `doca_telemetry_exporter_metrics_add_gauge_uint64`          | Unsigned integers (queue depth, active flows).         |
| Histogram   | `doca_telemetry_exporter_metrics_add_histogram`             | One-off histogram with explicit buckets.               |
| Base Hist.  | `doca_telemetry_exporter_metrics_add_base_histogram`        | Template that can be instantiated per label set.       |

### Export Destinations

| Exporter     | How to enable                                                           | Typical usage                                           |
|--------------|-------------------------------------------------------------------------|---------------------------------------------------------|
| File         | `doca_telemetry_exporter_schema_set_file_write_enabled`                 | Local debugging or offline ingestion.                   |
| IPC          | `doca_telemetry_exporter_schema_set_ipc_enabled`                        | Streaming into DOCA Telemetry Service (DTS).            |
| Prometheus   | Set `PROMETHEUS_ENDPOINT=host:port` before running the application. | Direct scrape by Prometheus-compatible collectors.      |

The sample leaves IPC/Prometheus disabled by default so it can run on any system; enable only what
your deployment requires.

### Building & Running
1. Build the sample with the standard SDK flow:
   ```
   ninja -C /doca/build/collectx-development telemetry_export_metrics
   ```
2. Run the binary (no arguments are required because the sample generates synthetic metrics):
   ```
   sudo /doca/build/collectx-development/samples/doca_telemetry_exporter/telemetry_export_metrics/telemetry_export_metrics
   ```
   Toggle exporter flags or the Prometheus endpoint directly inside
   `telemetry_export_metrics_sample.c` before rebuilding if your environment requires it.

### References
- `telemetry_export_metrics/telemetry_export_metrics_sample.c`
- `telemetry_export_metrics/telemetry_export_metrics_main.c`
- `telemetry_export_metrics/meson.build`

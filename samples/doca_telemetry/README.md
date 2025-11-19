# DOCA Telemetry Diagnostics Sample

This README describes the telemetry diagnostics sample based on the `doca_telemetry_diag` library.

The sample demonstrates the use of DOCA telemetry diagnostics APIs to initialize and configure the `doca_telemetry_diag` context, as well as querying and parsing diagnostic counters.

## Sample Usage

```bash
Usage: doca_telemetry_diag [DOCA Flags] [Program Flags]
```

### DOCA Flags
- `-h`, `--help`                  : Print a help synopsis
- `-v`, `--version`               : Print program version information
- `-l`, `--log-level`             : Set the (numeric) log level for the program (`<10=DISABLE, 20=CRITICAL, 30=ERROR, 40=WARNING, 50=INFO, 60=DEBUG, 70=TRACE>`)
- `--sdk-log-level`               : Set the SDK (numeric) log level for the program (`<10=DISABLE, 20=CRITICAL, 30=ERROR, 40=WARNING, 50=INFO, 60=DEBUG, 70=TRACE>`)
- `-j`, `--json <path>`           : Parse command line flags from an input json file

### Program Flags
- `-p`, `--pci-addr`              : DOCA device PCI device address
- `-di`, `--data-ids`             : Path to data IDs JSON file
- `-o`, `--output`                : Output CSV file (default: `/tmp/out.csv`)
- `-rt`, `--sample-run-time`      : Total sample run time, in seconds
- `-sp`, `--sample-period`        : Sample period, in nanoseconds
- `-ns`, `--log-num-samples`      : Log max number of samples
- `-sr`, `--max-samples-per-read` : Max number of samples per read
- `-sm`, `--sample-mode`          : Sample mode (`0 - single, 1 - repetitive, 2 - on demand`)
- `-of`, `--output-format`        : Output format
- `-f`, `--force-ownership`       : Force ownership when creating context
- `-e`, `--example-json-path`     : Generate an example JSON file with the default data IDs to the given path and exit immediately. This file can be used as input later on. All other flags are ignored.

## Sample Logic

1. Locating a DOCA device.
2. Initializing and configuring the `doca_telemetry_diag` instance.
3. Applying a list of data IDs to sample (either from a source JSON file or the default data IDs).
4. Starting the `doca_telemetry_diag` instance.
5. Allocating a buffer according to the sample size and the number of desired samples.
6. Querying the actual sample time after starting.
7. Retrieving samples and writing the retrieved data to a `.csv` file (either once or periodically).
8. Stopping the data IDs sampling.
9. Releasing all resources and destroying the context.

The sample can use data IDs given by the user using a JSON file.

An example of the JSON file format can be created by using the "-e" flag on the sample, to export an example JSON file containing the default data IDs to a given path.


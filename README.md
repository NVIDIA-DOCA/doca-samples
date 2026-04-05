# NVIDIA DOCA Samples

![DOCA software Stack](doca-software.jpg "DOCA Software Stack")

##  Purpose

The DOCA samples repository is an educational resource provided as a guide on how to program on the NVIDIA BlueField networking platform using DOCA API.

The repository consist of 2 parts:

* [Samples](samples/):  simplistic code snippets that demonstrate the API usage 
* [Applications](applications/): Advanced samples that implements a logic that might cross different SDK libs.

For instructions regarding the development environment and installation, refer to the [NVIDIA DOCA Developer Quick Start Guide](https://docs.nvidia.com/doca/sdk/doca+developer+quick+start+guide/) and the [NVIDIA DOCA Installation Guide for Linux](https://docs.nvidia.com/doca/sdk/doca+installation+guide+for+linux/) respectively.

##  Prerequisites

Install DOCA Software Package:

A detailed step-by-step process for downloading and installing the required development software on both the host and BlueField can be found in the [NVIDIA DOCA Installation Guide for Linux](https://docs.nvidia.com/doca/sdk/doca+installation+guide+for+linux/).

> [!NOTE]
> Use doca-all profile, This profile is the super-set of components, which also includes the content of doca-ofed and doca-networking.

##  Installation

Clone the sample repository

    git clone https://github.com/NVIDIA-DOCA/doca-samples.git

## Compilation

To compile all the reference applications:

Move to the applications directory:

    cd doca-samples/applications
    meson /tmp/build
    ninja -C /tmp/build

> [!NOTE]
> The generated applications are located under the `/tmp/build/` directory, using the following path `/tmp/build/<application_name>/doca_<application_name>`.

> [!NOTE]
> Compilation against DOCA's SDK relies on environment variables which are automatically defined per user session upon login. For more information, please refer to section _"Meson Complains About Missing Dependencies"_ in the [NVIDIA BlueField Platform Software Troubleshooting Guide](https://docs.nvidia.com/networking/display/nvidia-bluefield-platform-software-troubleshooting-guide.pdf).

## Developer Configurations

When recompiling the reference applications, meson compiles them by default in "debug" mode. Therefore, the binaries would not be optimized for performance as they would include the debug symbol. For comparison, the programs binaries shipped as part of DOCA's installation are compiled in "release" mode. To compile the applications in something other than debug, please consult Meson's configuration guide.

The reference applications also offer developers the ability to use the DOCA log's TRACE level (`DOCA_LOG_TRC`) on top of the existing DOCA log levels. Enabling the TRACE log level during compilation activates various developer log messages left out of the release compilation. Activating the TRACE log level may be done through `enable_trace_log` in the `meson_options.txt` file, or directly from the command line:

Prepare the compilation definitions to use the trace log level:

    meson /tmp/build -Denable_trace_log=true

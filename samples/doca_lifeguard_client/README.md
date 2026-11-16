#
# This software product is a proprietary product of NVIDIA CORPORATION &
# AFFILIATES (the "Company") and all right, title, and interest in and to the
# software product, including all associated intellectual property rights, are
# and shall remain exclusively with the Company.
#
# This software product is governed by the End User License Agreement
# provided with the software product.
#

# DOCA Lifeguard Client Samples

## Lifeguard Deposit
This sample connects to the DOCA Lifeguard service, deposits a DOCA device, and imports that deposit on a later run.

The package installs the service disabled. Start it before the sample. The socket is root-only, so run the sample as root.

```
systemctl start doca_lifeguard.service
./doca_lifeguard_deposit -p p0
```

`systemctl enable --now doca_lifeguard.service` also starts it on later boots.

`-p` / `--device` is an interface name such as `p0`, or a PCI address. `-s` / `--socket` overrides the socket path. The default is `/run/doca_lifeguard.sock`.

To see an imported deposit, start the sample, stop it with `kill -9`, and start it again. Stop that run with Ctrl-C to release the deposit.

### Sample Logic
1. Connect to the lifeguard service.
2. Open a DOCA device.
3. Import a deposit from a previous run, and release it when one exists.
4. Deposit this process's device.
5. On SIGINT or SIGTERM, release the deposit and disconnect.

### References
- `lifeguard_deposit/lifeguard_deposit_sample.c`
- `lifeguard_deposit/lifeguard_deposit_main.c`
- `lifeguard_deposit/meson.build`

#
# Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#!/usr/bin/python3

import argparse
import re
import os
import subprocess

parser = argparse.ArgumentParser(description='Bind PCI device to VFIO Driver.')
parser.add_argument('pci_address', metavar='P', type=str, help='PCI address of device to bind. Format: XXXX:XX:XX.X')
parser.add_argument('--unbind', action='store_true', help='Unbind PCI device from VFIO Driver')

args = parser.parse_args()

pci_address = args.pci_address
unbind = args.unbind

pattern = re.compile('^([A-Fa-f0-9]){4}:([A-Fa-f0-9]){2}:([A-Fa-f0-9]){2}.([A-Fa-f0-9])')
if not pattern.match(pci_address):
	print("Bad PCI format, expected format: XXXX:XX:XX.X")
	quit()

pci_device_sys_path = os.path.join('/sys/bus/pci/devices/', pci_address)
if not os.path.isdir(pci_device_sys_path):
	print("PCI device does not exist")
	quit()

os.system('modprobe vfio-pci')

unbind_path = os.path.join(pci_device_sys_path, 'driver/unbind')
if os.path.isfile(unbind_path):
	with open(unbind_path, 'w') as unbind_file:
		unbind_file.write(pci_address)

if (unbind):
	print(f'Unbind PCI Address = {pci_address}')
	quit()


lspci = subprocess.run(['lspci', '-ns', pci_address], stdout=subprocess.PIPE, text=True)
lspci_out = str(lspci.stdout)
(vid, did) = lspci_out.replace('\n', '').split(' ')[2].split(':')

try:
	with open('/sys/bus/pci/drivers/vfio-pci/remove_id', 'w') as remove_id_file:
		remove_id_file.write(f'{vid} {did}')
except:
	# ID does not exist
	pass

with open('/sys/bus/pci/drivers/vfio-pci/new_id', 'w') as new_id_file:
	new_id_file.write(f'{vid} {did}')

iommu_group_link_path = os.path.join(pci_device_sys_path, 'iommu_group')
readlink = subprocess.run(['readlink', iommu_group_link_path], stdout=subprocess.PIPE, text=True)
readlink_out = str(readlink.stdout)
iommu_group_id = readlink_out.split('/')[-1].replace('\n', '')

print(f'PCI Address = {pci_address}')
print(f'VFIO Group ID = {iommu_group_id}')

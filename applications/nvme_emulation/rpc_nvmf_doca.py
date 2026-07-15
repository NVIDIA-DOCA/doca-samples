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

from spdk.rpc.client import print_json

def get_managers(client):
	"""List all available devices that can be used as an emulation managers.
	"""
	return client.call('nvmf_doca_get_managers')

def create_function(client, dev_name):
	"""Creates a function.

	"""
	params = {}

	params['dev-name'] = dev_name
	return client.call('nvmf_doca_create_function', params)


def destroy_function(client, dev_name, vuid):
	"""Destroys a function.

	"""
	params = {}

	params['dev-name'] = dev_name
	params['vuid'] = vuid
	return client.call('nvmf_doca_destroy_function', params)

def list_functions(client, dev_name):
	"""List all avaialble function, for a DOCA device.

	"""
	params = {}

	params['dev-name'] = dev_name
	return client.call('nvmf_doca_list_functions', params)


def spdk_rpc_plugin_initialize(subparsers):
	def nvmf_doca_get_managers(args):
		print_json(get_managers(args.client))

	p = subparsers.add_parser('nvmf_doca_get_managers',
				  help='List all available devices that can be used as an emulation managers.')
	p.set_defaults(func=nvmf_doca_get_managers)

	def nvmf_doca_create_function(args):
		print_json(create_function(args.client, args.dev_name))

	p = subparsers.add_parser('nvmf_doca_create_function',
				  help='Creates a function')
	p.add_argument('-d', '--dev-name', help='The name of the device that function will be created with', type=str)
	p.set_defaults(func=nvmf_doca_create_function)

	def nvmf_doca_destroy_function(args):
		destroy_function(args.client, args.dev_name, args.vuid)

	p = subparsers.add_parser('nvmf_doca_destroy_function',
				  help='Destroys a function')
	p.add_argument('-d', '--dev_name', help='The name of the DOCA device tha function belongs to ', type=str)
	p.add_argument('-v', '--vuid', help='The vuid of the function we want to destroy', type=str)
	p.set_defaults(func=nvmf_doca_destroy_function)

	def nvmf_doca_list_functions(args):
        	print_json(list_functions(args.client, dev_name=args.dev_name))

	p = subparsers.add_parser('nvmf_doca_list_functions',
				  help='List all avaialble function, for a DOCA device')
	p.add_argument('-d', '--dev-name', help='The PCI type', type=str)
	p.set_defaults(func=nvmf_doca_list_functions)


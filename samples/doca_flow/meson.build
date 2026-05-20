#
# Copyright (c) 2022-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

samples = [
	'flow_acl',
	'flow_add_to_meta',
	'flow_aging',
	'flow_control_pipe',
	'flow_copy_to_meta',
	'flow_custom_header',
	'flow_drop',
	'flow_ecmp',
	'flow_esp',
	'flow_entropy',
	'flow_fwd_miss',
	'flow_fwd_target',
	'flow_geneve_encap',
	'flow_geneve_opt',
	'flow_gtp_encap',
	'flow_gtp',
	'flow_port_fwd',
	'flow_hash_pipe',
	'flow_hash_flooding_pipe',
	'flow_mult_flooding_cases',
	'flow_ip_in_ip',
	'flow_ipv6_flow_label',
	'flow_loopback',
	'flow_lpm',
	'flow_lpm_em',
	'flow_match_comparison',
	'flow_modify_header',
	'flow_monitor_meter',
	'flow_multi_actions',
	'flow_multi_fwd',
	'flow_ordered_list',
	'flow_parser_meta',
	'flow_pipe_resize',
	'flow_psp_ext',
	'flow_random',
	'flow_rss_esp',
	'flow_rss_meta',
	'flow_set_meta',
	'flow_sampling',
	'flow_shared_counter',
	'flow_shared_meter',
	'flow_switch',
	'flow_switch_control_pipe',
	'flow_switch_roce_bth_header',
	'flow_switch_rss',
	'flow_switch_single',
	'flow_switch_hot_upgrade',
	'flow_vxlan_encap',
	'flow_switch_to_wire',
	'flow_vxlan_shared_encap',
        'flow_tcp_seq',
        'flow_twamp_header',

	# Flow CT Samples
	'flow_ct_2_ports',
	'flow_ct_iterator',
	'flow_ct_udp',
	'flow_ct_udp_query',
	'flow_ct_udp_tunnel_asymmetric',
	'flow_ct_udp_update',
	'flow_ct_udp_single_match',
	'flow_ct_tcp',
	'flow_ct_aging',
	'flow_ct_tcp_actions',
	'flow_ct_tcp_entry_finalize',
	'flow_ct_tcp_ipv4_ipv6',
	'flow_ct_tcp_wire_to_wire',
]

sample_lib_srcs = files([
	'flow_common.c',
])

sample_lib_hdrs = files([
	'flow_common.h',
	'flow_ct_common.h',
	'flow_switch_common.h',
	'flow_eth_common.h',
])

# dpdk_utils.c is linked per-sample where needed (see individual sample meson files)

sample_lib_includes += [include_directories('.')]
# Needed for dpdk_utils.c and friends
sample_lib_includes += apps_common_base_include

# Code coverage: use same coverage options as doca_flow unit tests
if is_variable('doca_flow_override_options')
	sample_lib_override_options = doca_flow_override_options
endif

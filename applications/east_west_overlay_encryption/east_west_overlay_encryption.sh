#!/bin/bash
#
# Copyright (c) 2021-2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

# This script is used to initiate IPsec connection using strongSwan between two DPUs.

####################
## Configurations ##
####################

script_folder_path=`dirname "$0"`
script_path="$0"

# Define colors
RED="\033[0;31m"
GREEN="\033[0;32m"
NOCOLOR="\033[0;0m"

# Define commands
GREP_COMMAND=/usr/bin/grep
IP_COMMAND=/usr/sbin/ip

# Constant definitions
os_name=`cat /etc/os-release | $GREP_COMMAND -w 'NAME=' | cut -d '"' -f2 | cut -d ' ' -f1`
pciconf=`ls --color=never /dev/mst/ | $GREP_COMMAND --color=never '^m.*f0$' | cut -c 3-`
mlnx_bf_conf_file="/etc/mellanox/mlnx-bf.conf"
back_up_folder="/var/tmp/east_west_backup"
DEFAULT_OFFLOAD_MODE="packet"
NETMASK=24

# Define certificates and key folders
CERT_FOLDER="/etc/swanctl/x509"
KEY_FOLDER="/etc/swanctl/private"
CACERT_FOLDER="/etc/swanctl/x509ca"

######################
## Helper functions ##
######################

function usage() {
        cat << HEREDOC
        usage: `readlink -f "$script_path"` [COMMAND] [PARAMS...]
        Configures IPsec connection between two BlueFields with HW packet offload.
        PARAMS can be be either configured in JSON parameters file or passed on the command line.
        Example using $params_file_path:
                On one of the BlueFields: $script_path --side=r --json=east_west_overlay_encryption_params.json
                On the second BlueField (after the first one finishes): $script_path --side=i \
--json=east_west_overlay_encryption_params.json
                To destroy the connection:
                $script_path -d|--destroy
        Example passing the PARAMS:
                On one of the BlueFields: $script_path --side=r --initiator_ip_addr=192.168.50.1 \
--receiver_ip_addr=192.168.50.2 --port_num=0 --auth_method=psk --preshared_key=swordfish
                On the second BlueField (after the first one finishes): $script_path --side=i \
--initiator_ip_addr=192.168.50.1 --receiver_ip_addr=192.168.50.2 --port_num=0 --auth_method=psk \
--preshared_key=swordfish
                To destroy the connection:
                $script_path -d|--destroy
        Command and parameters:
        -d, --destroy                         Destroys IPsec connections that are configured using this script
                                              and reverts the configurations.
                                              If a port number was provided using --port_num flag then destroys only the
                                              IPsec connection on the given port, else destroys all IPsec
                                              connections.
        -h, --help                            Displays this help text and exit.
        --side=SIDE                           Side of the connection, can be r (meaning receiver, used when
                                              running on the first BlueField), or i (meaning initiator,
                                              used when running on the second BlueField). Note that this
                                              flag can't be configured in the parameters file and must be
                                              passed with the command line.
        -j=PARAMS_FILE, --json=PARAMS_FILE    JSON parameters file that includes the relevant parameters. When using
                                              this flag there is no need to pass any other parameters other than the
                                              side or destroy command.
                                              Note: the JSON file must follow the template as in the JSON
                                              file the came with this script (east_west_overlay_encryption_params.json).
        --initiator_ip_addr=IP_ADDRESS        The initiator's port IP address.
        --receiver_ip_addr=IP_ADDRESS         The receiver's port IP address.
        --port_num=NUM                        The number of the port, can be 0 or 1.
        --offload_mode=MODE                   The hardware offload mode, can be none or packet. If this parameter isn't
                                              passed then by default the mode is packet.
        --auth_method=METHOD                  The authentication method, can be psk (meaning pre-shared key
                                              authentication method), ssc (meaning self-signed
                                              certificates authentication method) or CA (meaning
                                              certificate authority certificates).
        --preshared_key=PSK                   The pre-shared key (relevant if auth_method is psk).
        --initiator_cert_path=CERT_PATH       The initiator's certificate path (relevant if auth_method is
                                              scc or ca).
        --receiver_cert_path=CERT_PATH        The receiver's certificate path (relevant if auth_method is
                                              scc or ca).
        --initiator_key_path=KEY_PATH         The initiator's private-key path (relevant if side is
                                              initiator and auth_method is scc or ca).
        --receiver_key_path=KEY_PATH          The receiver's private-key path (relevant if side is
                                              receiver and auth_method is scc or ca).
        --initiator_cacert_path=CACERT_PATH   The initiator's CA certificate path (relevant if side is
                                              initiator and auth_method is ca).
        --receiver_cacert_path=CACERT_PATH    The receiver's CA certificate path (relevant if side is
                                              receiver and auth_method is ca).
        --initiator_cn=CN                     The common name (CN) of the initiator's certificate (relevant
                                              if side is receiver and auth_method is ca).
        --receiver_cn=CN                      The common name (CN) of the receiver's certificate (relevant
                                              if side is initiator and auth_method is ca).
HEREDOC

        exit $1
}

# This function outputs error message that is passed to this function as argument.
function err() {
        local m="ERROR: $1"
        echo -e "${RED}$m$NOCOLOR"
}

# This function checks if a service is running, and if not it starts it.
# The service name is given as an argument to the function.
function start_service() {
        local service_name=$1
        systemctl is-active --quiet $service_name || systemctl start $service_name
}

# Delete from ovs bridges ports that we use.
function delete_prev_bridges() {
        echo "Deleting vxlan-br${port_num} (if exists) and deleting $PF and $PF_REP from all bridges."
        # Delete vxlan-br${port_num} bridge if exists
        ovs-vsctl del-br vxlan-br${port_num} > /dev/null 2>&1
        # Delete PF and PF_REP ports from all other bridges
        for bridge in `ovs-vsctl show |  $GREP_COMMAND -oP 'Bridge \K\w[^\s]+'`; do
                ovs-vsctl del-port $bridge $PF > /dev/null 2>&1
                ovs-vsctl del-port $bridge $PF_REP > /dev/null 2>&1
        done
}

# This function outputs the MTU of the device that is passed as argument to the function.
function get_mtu() {
        local device=$1
        local device_mtu=`$IP_COMMAND link show dev $device | $GREP_COMMAND -oP --color=never '(?<=mtu\s)\w+'`
        echo $device_mtu
}

# Configure IPsec over OVS VXLAN bridge according to authentication method.
function ovs_config() {
        # start openvswitch according to the operating system.
        if [[ "$os_name" == "Ubuntu" ]]; then
                start_service openvswitch-switch
        elif [[ "$os_name" == "CentOS" ]]; then
                start_service openvswitch
        fi

        # Need to wait before querying OVS to make sure it's up
        sleep 5s

        # Delete ovs bridges and ports that we use.
        delete_prev_bridges

        # Configure IP address for the interface
        $IP_COMMAND addr add ${LOCAL_IP}/${NETMASK} dev $PF
        $IP_COMMAND link set $PF_REP up

        # Set the MTU of PF to be 50 more of the PF_REP's MTU to account for the size of VXLAN headers.
        local pf_mtu=$(get_mtu $PF_REP)
        let pf_mtu+=50
        echo "Setting the MTU of $PF to $pf_mtu (50 more than the MTU of $PF_REP) to account for the size of VXLAN headers."
        $IP_COMMAND link set mtu $pf_mtu dev $PF

        # Enable tc offloading if HW offload mode is full
        if [[ "$offload_mode" == "packet" ]]; then
                echo "Updating hw-tc-offload to $PF."
                ethtool -K $PF hw-tc-offload on
        fi
        # Disable host PF as the port owner
        mlxprivhost -d /dev/mst/mt${pciconf} --disable_port_owner r

        # Start OVS IPsec.
        start_service openvswitch-ipsec.service

        # Configure OVS VXLAN IPsec bridge.
        bride_name=vxlan-br${port_num}
        ovs-vsctl add-br $bride_name
        ovs-vsctl add-port $bride_name $PF_REP

        # Configure tunnel type, port key and destination port.
        local tunnel_type="vxlan"
        local port_key=100
        # destination port of vxlan tunnel is always 4789.
        local dst_port=4789

        # Define the backup files according to the port number
        back_up_file_cert="${back_up_folder}/p${port_num}/backup_cert.txt"
        back_up_file_addr="${back_up_folder}/p${port_num}/backup_addr.txt"

        # Backup IP addresses to delete the IPsec rules in the future
        if [[ ! -f $back_up_file_addr ]]; then
                mkdir -p ${back_up_folder}/p${port_num}
                touch $back_up_file_addr
        fi
        echo "$LOCAL_IP" >> $back_up_file_addr
        echo "$REMOTE_IP" >> $back_up_file_addr

        # Choose authentication method.
        if [[ "$auth_method" == "psk" ]]; then
                echo "Configuring IPsec using pre-shared key."
                ovs_config_psk $tunnel_type $port_key $dst_port
        elif [[ "$auth_method" == "ssc" ]]; then
                echo "Configuring IPsec using self-signed certificates."
                ovs_config_ssc $tunnel_type $port_key $dst_port
        elif [[ "$auth_method" == "ca" ]]; then
                echo "Configuring IPsec using CA-signed Certificate."
                ovs_config_ca $tunnel_type $port_key $dst_port
        fi
}

# Configure IPsec over OVS VXLAN bridge using pre-shared key.
# Tunnel type, port key and dst_port are passed as arguments to the function (in that order).
function ovs_config_psk() {
        # Configure arguments.
        local tunnel_type=$1
        local port_key=$2
        local dst_port=$3

        # Add IPsec rule to the bridge.
        ovs-vsctl add-port $bride_name tun${port_num} -- \
                set interface tun${port_num} type=$tunnel_type \
                options:local_ip=$LOCAL_IP options:remote_ip=$REMOTE_IP \
                options:key=$port_key options:dst_port=$dst_port \
                options:psk=$preshared_key

        ovs-vsctl show
}

# Configure IPsec over OVS VXLAN bridge using self-signed certificates.
# Tunnel type, port key and dst_port are passed as arguments to the function (in that order).
function ovs_config_ssc() {
        # Configure arguments.
        local tunnel_type=$1
        local port_key=$2
        local dst_port=$3

        # Configure certificates and key.
        cp $LOCAL_CERT $REMOTE_CERT $CERT_FOLDER
        cp $KEY $KEY_FOLDER

        # Update variables.
        LOCAL_CERT="${CERT_FOLDER}/`basename $LOCAL_CERT`"
        REMOTE_CERT="${CERT_FOLDER}/`basename $REMOTE_CERT`"
        KEY="${KEY_FOLDER}/`basename $KEY`"

        # Add IPsec rule to the bridge.
        ovs-vsctl set Open_vSwitch . \
                other_config:certificate="$LOCAL_CERT" \
                other_config:private_key="$KEY"

        ovs-vsctl add-port $bride_name ${tunnel_type}p0 -- set interface ${tunnel_type}p0 type=$tunnel_type \
                options:local_ip=$LOCAL_IP options:remote_ip=$REMOTE_IP \
                options:key=$port_key options:dst_port=$dst_port \
                options:remote_cert="$REMOTE_CERT"

        # Backup Certificates and key path so we delete them when reverting IPsec configurations.
        if [[ ! -f $back_up_file_cert ]]; then
                touch $back_up_file_cert
        fi
        echo "$LOCAL_CERT" >> $back_up_file_cert
        echo "$REMOTE_CERT" >> $back_up_file_cert
        echo "$KEY" >> $back_up_file_cert

        ovs-vsctl show
}

# Configure IPsec over OVS VXLAN bridge using certificate authority (CA) certificates.
# Tunnel type, port key and dst_port are passed as arguments to the function (in that order).
function ovs_config_ca() {
        # Configure arguments.
        local tunnel_type=$1
        local port_key=$2
        local dst_port=$3

        # Configure certificates and key.
        cp $LOCAL_CERT $REMOTE_CERT $CERT_FOLDER
        cp $KEY $KEY_FOLDER
        cp $CACERT $CACERT_FOLDER

        # Update variables.
        LOCAL_CERT="${CERT_FOLDER}/`basename $LOCAL_CERT`"
        REMOTE_CERT="${CERT_FOLDER}/`basename $REMOTE_CERT`"
        KEY="${KEY_FOLDER}/`basename $KEY`"
        CACERT="${CACERT_FOLDER}/`basename $CACERT`"

        # Add IPsec rule to the bridge.
        ovs-vsctl set Open_vSwitch . \
                other_config:certificate="$LOCAL_CERT" \
                other_config:private_key="$KEY" \
                other_config:ca_cert="$CACERT"

        ovs-vsctl add-port $bride_name ${tunnel_type}p0 -- set interface ${tunnel_type}p0 type=$tunnel_type \
                options:local_ip=$LOCAL_IP options:remote_ip=$REMOTE_IP \
                options:key=$port_key options:dst_port=$dst_port \
                options:remote_name=$REMOTE_CN

        # Backup Certificates and key paths so we delete them when reverting IPsec configurations.
        if [[ ! -f $back_up_file_cert ]]; then
                touch $back_up_file_cert
        fi
        echo "$LOCAL_CERT" >> $back_up_file_cert
        echo "$REMOTE_CERT" >> $back_up_file_cert
        echo "$KEY" >> $back_up_file_cert
        echo "$CACERT" >> $back_up_file_cert

        ovs-vsctl show
}

# Configure IPsec offload mode to "full" if argument is 1, Configure IPsec offload mode to "none" if argument is 0.
# Note: This function restarts the ib driver.
function config_offload() {
        if [[ $1 -eq 1 ]] ; then
                echo "Configuring IPsec HW offload mode to \"packet\"."
                # Replace "no" with "yes" in conf_file.
                search_string="IPSEC_FULL_OFFLOAD=\"no\""
                replace_string="IPSEC_FULL_OFFLOAD=\"yes\""
                ipsec_wanted_mode="full"
                # Start openvswitch according to the operating system.
                if [[ "$os_name" == "Ubuntu" ]]; then
                        start_service openvswitch-switch
                elif [[ "$os_name" == "CentOS" ]]; then
                        start_service openvswitch
                fi
                # Add HW offload = true to the OVS configuration
                ovs-vsctl set Open_vSwitch . other_config:hw-offload=true
        elif [[ $1 -eq 0 ]] ; then
                echo "Configuring IPsec HW offload mode to \"none\"."
                # Replace "yes" with "no" in conf_file.
                search_string="IPSEC_FULL_OFFLOAD=\"yes\""
                replace_string="IPSEC_FULL_OFFLOAD=\"no\""
                ipsec_wanted_mode="none"
                # Start openvswitch according to the operating system.
                if [[ "$os_name" == "Ubuntu" ]]; then
                        start_service openvswitch-switch
                elif [[ "$os_name" == "CentOS" ]]; then
                        start_service openvswitch
                fi
                # Add HW offload = false to the OVS configuration
                ovs-vsctl set Open_vSwitch . other_config:hw-offload=false
        fi

        # Check if IPsec mode is already the wanted one.
        diff  <(echo $ipsec_wanted_mode) <(echo $ipsec_current_mode) > /dev/null 2>&1
        if (( $? == 0 )); then
                echo "IPsec HW offload mode is already \"$ipsec_wanted_mode\", no need to reconfigure."
                return
        fi

        if  $GREP_COMMAND -Fxq "$replace_string" $mlnx_bf_conf_file; then
                : # Do nothing.
        elif $GREP_COMMAND -Fxq "$search_string" $mlnx_bf_conf_file; then
                sed -i "s/$search_string/$replace_string/" $mlnx_bf_conf_file
        fi

        # Disable regex temporarily.
        echo "Disabling regex temporarily to configure IPsec HW offload mode."
        systemctl stop mlx-regex

        # Reload driver to apply changes.
        echo "Restarting ib driver (/etc/init.d/openibd restart)."
        /etc/init.d/openibd restart

        # Re-enable regex.
        echo "Re-enabling regex."
        systemctl restart mlx-regex
}

# This function delete IPsec states and policies according to the two IP addresses (LOCAL and REMOTE) that are
# passed to this function as parameters.
function delete_ipsec_rules() {
        # Delete relevant policies
        for spi in `$IP_COMMAND xfrm state | $GREP_COMMAND -A1 "src $1 dst $2" | $GREP_COMMAND spi | awk '{ print $4 }'`; do
                $IP_COMMAND xfrm state delete src $1 dst $2 proto esp spi $spi
        done
        for spi in `$IP_COMMAND xfrm state | $GREP_COMMAND -A1 "src $2 dst $1" | $GREP_COMMAND spi | awk '{ print $4 }'`; do
                $IP_COMMAND xfrm state delete src $2 dst $1 proto esp spi $spi
        done

        # Delete relevant states
        $IP_COMMAND xfrm policy | $GREP_COMMAND "src ${1}/32 dst ${2}/32" | while read policy; do
                $IP_COMMAND xfrm policy delete $policy dir out
        done
        $IP_COMMAND xfrm policy | $GREP_COMMAND "src ${2}/32 dst ${1}/32" | while read policy; do
                $IP_COMMAND xfrm policy delete $policy dir in
        done


}

# This function deletes the IPsec configurations according to the local and remote IP addresses.
# The local and remote IP addresses are passed to this function as arguments
function delete_ipsec() {
        local local_ip=$1
        local remote_ip=$2

        # Check if IP addresses are valid (ranging from 1.1.1.1 to 255.255.255.225)
        if [[ $local_ip =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ && $remote_ip =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                # Delete the IPsec rules.
                delete_ipsec_rules $local_ip $remote_ip
        else
                err "IPsec rules deletion failed due to invalid IP addresses."
        fi
}

# This function flushes the local IP address from the interfaces (p0 or p1), and deletes the vxlan bridge.
# The local IP address and port number are passed to this function as arguments.
function flush_interface() {
        local local_ip=$1
        local port_number=$2

        if [[ "$port_number" == 0 || "$port_number" == 1 ]]; then
                # Check if local_ip is valid (ranging from 1.1.1.1 to 255.255.255.225)
                if [[ $local_ip =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                        # Delete the IP address from the port
                        $IP_COMMAND addr del ${local_ip}/${NETMASK} dev p${port_number}
                else
                        err "Flushing of interface p${port_number} failed due to invalid IP addresses."
                fi
        else
                err "Port flush failed due to invalid port number."
        fi
}

# This function reverts the IPsec rules and delete the vxlan bridge according to the port number.
# The port number (0 or 1) is given to this function as an argument
function revert_ipsec_port() {
        local port_num=$1

        # If the backup folder according to the port number doesn't exist then exit the function
        if ! [[ -d ${back_up_folder}/p${port_num} ]]; then
                return
        fi

        # Get the backup folders according to the port number
        local back_up_file_cert="${back_up_folder}/p${port_num}/backup_cert.txt"
        local back_up_file_addr="${back_up_folder}/p${port_num}/backup_addr.txt"

         # Get the relevant IP addresses and delete IPsec rules
        if [[ -f $back_up_file_addr ]]; then
                local local_ip=`sed '1q;d' $back_up_file_addr`
                local remote_ip=`sed '2q;d' $back_up_file_addr`
                delete_ipsec $local_ip $remote_ip
        else
                err "IPsec rules deletion failed due to invalid IP addresses."
        fi

        start_service NetworkManager
        # Flush the interface according to the port number
        flush_interface $local_ip $port_num

        # Delete the vxlan bridge
        ovs-vsctl del-br vxlan-br${port_num}

        # Delete certificates and key if exist.
        if [[ -f $back_up_file_cert ]]; then
                while IFS= read -r line; do
                        sudo rm -f $line > /dev/null 2>&1
                done < $back_up_file_cert
        fi
}

# This function reverts IPsec rules (if they exist) and deletes local certificates and key files (if they exist).
function revert_ipsec() {
        echo "Reverting IPsec configurations..."

        # Check if a port number was given
        if [[ "$port_num" == 0 || "$port_num" == 1 ]]; then
                # Check if a connection was established on this port number
                if [[ -d ${back_up_folder}/p${port_num} ]]; then
                        # If exists continue then revert
                        revert_ipsec_port $port_num

                        # Delete backup folder for this specific port
                        rm -rf ${back_up_folder}/p${port_num}

                        # Check if another connection exists on the other port number
                        if [[ "$port_num" == 0 ]]; then
                                other_port_num=1
                        elif [[ "$port_num" == 1 ]]; then
                                other_port_num=0
                        fi

                        if [[ -d ${back_up_folder}/p${other_port_num} ]]; then
                                # If another connection exists then exit
                                echo "Reverted IPsec configurations for port number ${port_num}."
                                echo "Don't forget to revert the configurations on the other machine (if relevant)."
                                return
                        else
                                # If another connection doesn't exist then cleanup everything

                                # Disable host restriction
                                mlxprivhost -d /dev/mst/mt${pciconf} p
                                # Disable HW offload
                                config_offload 0

                                # Delete the backup folder
                                sudo rm -rf $back_up_folder

                                echo "Reverted IPsec configurations."
                                echo "Don't forget to revert the configurations on the other machine (if relevant)."
                                return
                        fi
                else
                        # If no connection was established using the script on the given port number then print error and exit
                        err "No IPsec connection exists for port ${port_num}, nothing to destroy."
                        exit 1
                fi

        else
                # If no port number was given then delete all connections if exist

                # Check if any IPsec connection was established using the script by checking the backup folder
                if ! [[ -d $back_up_folder ]]; then
                        err "No IPsec connections were configured using this script to destroy."
                        exit 1
                fi

                # Try to revert IPsec connection on both ports
                port_num=0
                revert_ipsec_port $port_num
                port_num=1
                revert_ipsec_port $port_num

                # Disable host restriction
                mlxprivhost -d /dev/mst/mt${pciconf} p

                # Disable HW Offload.
                config_offload 0

                # Delete the backup folder
                sudo rm -rf $back_up_folder
        fi
}


# This functions outputs the number of SADs
function read_sad_counter() {
        local counter=`$IP_COMMAND xfrm state count | awk '{ print $3 }'`
        echo $counter
}

# This functions checks if the IPsec connection is established:
#       Comparing the initial SAD counter (which is passed as argument to the function) with current SAD counter.
#       Checking the IPsec SAD entries if they are correct for this connection.
function validate_ipsec() {
        # Define initial and current SAD states.
        local initial_sad_count=$1
        local current_sad_count=$(read_sad_counter)

        # Check if the current SAD counter is bigger than the initial SAD counter.
        if [ "$current_sad_count" -le "$initial_sad_count" ]; then
                err "IPsec connection failed, check parameters and configurations and then try again."
                revert_ipsec
                exit 1
        fi

        # Also check if the entries are correct for this connection.
        $IP_COMMAND xfrm state | $GREP_COMMAND "src $LOCAL_IP dst $REMOTE_IP" > /dev/null 2>&1
        if (( $? !=0 )); then
                err "IPsec connection failed, check parameters and configurations and then try again."
                revert_ipsec
                exit 1
        fi
        $IP_COMMAND xfrm state | $GREP_COMMAND "src $REMOTE_IP dst $LOCAL_IP" > /dev/null 2>&1
        if (( $? !=0 )); then
                err "IPsec connection failed, check parameters and configurations and then try again."
                revert_ipsec
                exit 1
        fi
        echo -e "${GREEN}IPsec connection is now established. You can now send encrypted" \
                "traffic between ${LOCAL_IP} and ${REMOTE_IP}.$NOCOLOR"
}

# This function receives as parameters the devices and IP address of the PF required for the IPsec connection and checks if they are available.
# If no devices are found the function tries to fix the issue by restarting the ib driver.
# If the fix didn't help, we exit the script.
function check_devices() {
        local PF=$1
        local PF_REP=$2
        local IP_ADDRESS=$3
        echo "Checking devices $PF and $PF_REP."
        $IP_COMMAND link set $PF up > /dev/null 2>&1
        if (( $? != 0 )); then
                echo "$PF device not found, trying to fix."
                # Reload ib driver to find port.
                echo "Restarting ib driver (/etc/init.d/openibd restart)."
                /etc/init.d/openibd restart
                $IP_COMMAND link set $PF up > /dev/null 2>&1
                if (( $? != 0 )); then
                        err "Fix didn't work, $PF device not found, exiting."
                        # Exit
                        exit 1
                fi
        fi

        $IP_COMMAND link set $PF_REP up > /dev/null 2>&1
        if (( $? != 0 )); then
                echo "$PF_REP device not found, trying to fix."
                # Reload ib driver to find port.
                echo "Restarting ib driver (/etc/init.d/openibd restart)."
                /etc/init.d/openibd restart
                $IP_COMMAND link set $PF_REP up > /dev/null 2>&1
                if (( $? != 0 )); then
                        err "Fix didn't work, $PF_REP device not found, exiting."
                        # Flush IP address from the port
                        $IP_COMMAND addr del ${IP_ADDRESS}/${NETMASK} dev p${port_num}
                        # Exit
                        exit 1
                fi
        fi
}

# This function checks if the relevant certificates and keys exist.
# The certificates and keys are passed as arguments to the function.
# The CA certificate should be always passed last.
# If the relevant certificates and keys don't exist then we exit the script.
function check_certificates_key() {
        echo "Checking certificates and keys."
        for ((i = 1; i < $#; i++ )); do
                arg=${!i}
                if [[ ! -f "$arg" ]]; then
                        err "$arg does not exist. Exiting."
                        # Exit
                        exit 1
                fi
        done

        # If ca mode check if the CA certificate exists.
        # The CA certificate is the last argument passed to the function.
        if [[ "$auth_method" == "ca" ]]; then
                arg=${!#}
                if [[ ! -f "$arg" ]]; then
                        err "$arg does not exist. Exiting."
                        # Exit
                        exit 1
                fi
        fi
}

# This function check's from the initiator's side if there is a valid connection between the initiator and the receiver
# using device that is passed as argument to the function.
function check_connection() {
        # Delete ovs bridges and ports that we use so we can check the connection.
        delete_prev_bridges

        # Give the device IP address to check connection.
        local device=$1
        $IP_COMMAND addr add ${LOCAL_IP}/${NETMASK} dev $device > /dev/null 2>&1

        # Check if there's a connection between the initiator and receiver side using PF.
        if [[ "$side" == "initiator" || "$side" == "i" ]]; then
                echo "Checking connection between $LOCAL_IP and $REMOTE_IP..."
                ping -c1 $REMOTE_IP > /dev/null 2>&1
                if (( $? !=0 )); then
                        err "Can't ping between $LOCAL_IP and $REMOTE_IP. Exiting."
                        # Flush IP address from the port
                        $IP_COMMAND addr del ${LOCAL_IP}/${NETMASK} dev $device
                        # Exit
                        exit 1
                else
                        echo "Connection is successful."
                fi
        fi
}

# This function parses the parameters from the json file
function parse_json_file() {
        # Check if file exists as absolute path or relative path
        json_file_relative=${script_folder_path}/${json_file}
        if [[ -f $json_file ]]; then
                :
        elif [[ -f $json_file_relative ]]; then
                json_file=$json_file_relative
        else
                err "JSON file $json_file does not exist. Exiting."
                # Exit
                exit 1
        fi

        echo "Parsing parameters from $json_file..."
        initiator_ip_addr=`$GREP_COMMAND '"initiator_ip_addr":' $json_file | awk -F "\"" '{ print $4 }'`
        receiver_ip_addr=`$GREP_COMMAND '"receiver_ip_addr":' $json_file | awk -F "\"" '{ print $4 }'`
        port_num=`$GREP_COMMAND '"port_num":' $json_file | awk -F "\"" '{ print $4 }'`
        offload_mode=`$GREP_COMMAND '"offload_mode":' $json_file | awk -F "\"" '{ print $4 }'` > /dev/null 2>&1
        auth_method=`$GREP_COMMAND '"auth_method":' $json_file | awk -F "\"" '{ print $4 }'`
        preshared_key=`$GREP_COMMAND '"preshared_key":' $json_file | awk -F "\"" '{ print $4 }'`
        initiator_cert_path=`$GREP_COMMAND '"initiator_cert_path":' $json_file | awk -F "\"" '{ print $4 }'`
        receiver_cert_path=`$GREP_COMMAND '"receiver_cert_path":' $json_file | awk -F "\"" '{ print $4 }'`
        initiator_key_path=`$GREP_COMMAND '"initiator_key_path":' $json_file | awk -F "\"" '{ print $4 }'`
        receiver_key_path=`$GREP_COMMAND '"receiver_key_path":' $json_file | awk -F "\"" '{ print $4 }'`
        initiator_cacert_path=`$GREP_COMMAND '"initiator_cacert_path":' $json_file | awk -F "\"" '{ print $4 }'`
        receiver_cacert_path=`$GREP_COMMAND '"receiver_cacert_path":' $json_file | awk -F "\"" '{ print $4 }'`
        initiator_cn=`$GREP_COMMAND '"initiator_cn":' $json_file | awk -F "\"" '{ print $4 }'`
        receiver_cn=`$GREP_COMMAND '"receiver_cn":' $json_file | awk -F "\"" '{ print $4 }'`
}

# This functions parses the parameters of the script.
function parse_parameters() {
        num_of_params=$#
        for i in "$@"; do
                case $i in
                        -j=*)
                                json_file="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --json=*)
                                json_file="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --side=*)
                                side="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --initiator_ip_addr=*)
                                initiator_ip_addr="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --receiver_ip_addr=*)
                                # Move to the next parameter
                                receiver_ip_addr="${i#*=}"
                                shift
                                ;;
                        --port_num=*)
                                port_num="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --offload_mode=*)
                                offload_mode="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --auth_method=*)
                                auth_method="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --preshared_key=*)
                                preshared_key="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --initiator_cert_path=*)
                                initiator_cert_path="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --receiver_cert_path=*)
                                # Move to the next parameter
                                receiver_cert_path="${i#*=}"
                                shift
                                ;;
                        --initiator_key_path=*)
                                initiator_key_path="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --receiver_key_path=*)
                                receiver_key_path="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --initiator_cacert_path=*)
                                initiator_cacert_path="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --receiver_cacert_path=*)
                                receiver_cacert_path="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --initiator_cn=*)
                                initiator_cn="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        --receiver_cn=*)
                                receiver_cn="${i#*=}"
                                # Move to the next parameter
                                shift
                                ;;
                        -d|--destroy)
                                command="$i"
                                # Move to the next parameter.
                                shift
                                ;;
                        -h|--help)
                                command="$i"
                                # Move to the next parameter.
                                shift
                                ;;
                        *)
                                err "Illegal parameter $i. Run the script with --help for usage."
                                exit 1
                                ;;
                esac
        done

        # Parse the parameters from the json file if it was passed
        if [[ -z $json_file ]]; then
                : # If not set then do nothing
        else
                # If set check if the number of params is legal
                if (( $num_of_params > 2 )); then
                        err "Illegal number of parameters! When running with JSON file, the only other parameter that \
can be passed is the side. Exiting"
                        # Exit
                        exit 1
                fi
                parse_json_file
        fi
}

# This function checks if the parameters passed to the function are set.
# Parameter name is passed with the parameter value in case of an unset parameter.
function check_parameters_set() {
        while [[ $# -gt 0 ]]; do
                # Check if parameter is set
                if [[ -z $2 ]]; then
                        # Get parameter name
                        parameter_name=$1
                        err "Parameter $parameter_name was not set. Run the script with --help for usage."
                        exit 1
                else
                        # Move to the next parameter
                        shift 2
                fi
        done
}

# This function checks and configures the parameters according to the side and auth_method
function configure_parameters() {
        # If command is help or destroy then no need for checking and configuring parameters
        if [[ "$command" == "--help" || "$command" == "-h" || "$command" == "--destroy" || "$command" == "-d" ]]; then
                return 0
        fi

        # Check if the needed parameters were set
        check_parameters_set "side" "$side" "initiator_ip_addr" "$initiator_ip_addr" \
                "receiver_ip_addr" "$receiver_ip_addr" "port_num" "$port_num" "auth_method" "$auth_method"

        # Check if IP addresses are valid (ranging from 1.1.1.1 to 255.255.255.225)
        if [[ ! ( $initiator_ip_addr =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ) ]]; then
                err "Initiator's IP address isn't valid, note that valid IP addresses should range from 1.1.1.1 to 255.255.255.255. Exiting."
                # Exit
                exit 1
        fi
        if [[ ! ( $receiver_ip_addr =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ) ]]; then
                err "Receiver's IP address isn't valid, note that valid IP addresses should range from 1.1.1.1 to 255.255.255.255. Exiting."
                # Exit
                exit 1
        fi

        # Check if the side is legal
        if [[ "$side" != "initiator" && "$side" != "i" && "$side" != "receiver" && "$side" != "r" ]]; then
                err "Illegal side $side, must be i|initiator|r|receiver. Exiting."
                # Exit
                exit 1
        fi

        # Check if HW offload mode is provided, else go to default HW offload mode
        if [[ -z $offload_mode ]]; then
                offload_mode=$DEFAULT_OFFLOAD_MODE
        else
                if [[ "$offload_mode" != "packet" && "$offload_mode" != "none" ]]; then
                        err "Illegal offload mode $offload_mode, offload mode must be packet or none. Exiting."
                        # Exit
                        exit 1
                fi
        fi

        # Check the rest of the parameters according to auth_method and side
        if [[ "$auth_method" == "psk" ]]; then
                check_parameters_set "preshared_key" $preshared_key
        elif [[ "$auth_method" == "ssc" ]]; then
                if [[ "$side" == "initiator" || "$side" == "i" ]]; then
                        check_parameters_set "initiator_cert_path" "$initiator_cert_path" \
                                "receiver_cert_path" "$receiver_cert_path" "initiator_key_path" "$initiator_key_path"
                elif [[ "$side" == "receiver" || "$side" == "r" ]]; then
                        check_parameters_set "initiator_cert_path" "$initiator_cert_path" \
                                "receiver_cert_path" "$receiver_cert_path" "receiver_key_path" "$receiver_key_path"
                fi
        elif [[ "$auth_method" == "ca" ]]; then
                if [[ "$side" == "initiator" || "$side" == "i" ]]; then
                        check_parameters_set "initiator_cert_path" "$initiator_cert_path" \
                                "receiver_cert_path" "$receiver_cert_path" "initiator_key_path" "$initiator_key_path" \
                                "initiator_cacert_path" "$initiator_cacert_path" "receiver_cn" "$receiver_cn"
                elif [[ "$side" == "receiver" || "$side" == "r" ]]; then
                        check_parameters_set "initiator_cert_path" "$initiator_cert_path" \
                                "receiver_cert_path" "$receiver_cert_path" "receiver_key_path" "$receiver_key_path" \
                                "receiver_cacert_path" "$receiver_cacert_path" "initiator_cn" "$initiator_cn"
                fi
        else
                # Illegal authentication method
                err "Illegal auth_method $auth_method, must be psk|ssc|ca. Exiting."
                # Exit
                exit 1
        fi

        # Check port number if legal.
        if (( $port_num != 0 && $port_num != 1 )); then
                err "Illegal port_num $port_num, port_num must be 0 or 1. Exiting"
                # Exit
                exit 1
        fi

        # Configure parameters
        PF=p${port_num}
        PF_REP=pf${port_num}hpf
        if [[ "$side" == "initiator" || "$side" == "i" ]]; then
                LOCAL_IP=$initiator_ip_addr
                REMOTE_IP=$receiver_ip_addr
                LOCAL_CERT=$initiator_cert_path
                REMOTE_CERT=$receiver_cert_path
                CACERT=$initiator_cacert_path
                KEY=$initiator_key_path
                REMOTE_CN=$receiver_cn
        elif [[ "$side" == "receiver" || "$side" == "r" ]]; then
                LOCAL_IP=$receiver_ip_addr
                REMOTE_IP=$initiator_ip_addr
                LOCAL_CERT=$receiver_cert_path
                REMOTE_CERT=$initiator_cert_path
                CACERT=$receiver_cacert_path
                KEY=$receiver_key_path
                REMOTE_CN=$initiator_cn
        fi

        # If in "ssc" or "ca" mode then check if the certificates and key exist.
        if [[ "$auth_method" == "ssc" || "$auth_method" == "ca" ]]; then
                check_certificates_key $LOCAL_CERT $REMOTE_CERT $KEY $CACERT
        fi
}

# This function checks if there's an existing connection on the same port
# If there is then we exit the script, else we continue with no changes
function check_existing_connection() {
        # Check if we already ran the script before and didn't destroy by checking the backup folder
        if [ -d ${back_up_folder}/p${port_num} ]; then
                        err "IPsec was already configured with the same port number. Run the script with --destroy first then create new connection."
                        exit 1
        fi
}

# This function parses and checks the script's parameters.
function get_parameters() {
        # Parse the parameter
        parse_parameters "$@"

        # Configure and check the parameters
        configure_parameters

        if [[ "$command" == "--help" || "$command" == "-h" ]]; then
                usage 0
        elif [[ "$command" == "--destroy" || "$command" == "-d" ]]; then
                revert_ipsec
                exit 0
        fi

        # Check if a connection already exists with the same port number and exit if so
        check_existing_connection

        # Check devices needed for the IPsec connection.
        check_devices $PF $PF_REP $LOCAL_IP

        # Check connection using PF.
        check_connection $PF
}

##################
## Script Start ##
##################

# Parse and check script's parameters.
get_parameters "$@"

# Start strongSwan
start_service strongswan.service

# Get initial SAD count to compare after the connection is established.
initial_sad_count=$(read_sad_counter)

# Configure IPsec offload mode according to the mode
if [[ "$offload_mode" == "packet" ]]; then
        config_offload 1
elif [[ "$offload_mode" == "none" ]]; then
        config_offload 0
fi

# Configure the OVS VXLAN IPsec bridge.
ovs_config

# Check if the IPsec connection is established by checking if the SAD count increased and if the SAD entries are correct.
if [[ "$side" == "initiator" || "$side" == "i" ]]; then
        echo "Waiting 10 seconds for the IPsec connection to be established."
        sleep 10s
        validate_ipsec $initial_sad_count
fi

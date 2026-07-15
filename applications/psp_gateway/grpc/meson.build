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

protoc = find_program('protoc')
grpc_cpp_plugin = find_program('grpc_cpp_plugin')

protobuf_gen_cc = generator(protoc, \
    output    : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
    arguments : [
        '--cpp_out=@BUILD_DIR@',
        '--proto_path=@CURRENT_SOURCE_DIR@',
        '@INPUT@'])

grpc_gen_cc = generator(protoc, \
    output    : ['@BASENAME@.grpc.pb.cc', '@BASENAME@.grpc.pb.h'],
    arguments : [
        '--grpc_out=@BUILD_DIR@',
        '--proto_path=@CURRENT_SOURCE_DIR@',
        '--plugin=protoc-gen-grpc=' + grpc_cpp_plugin.full_path(),
        '@INPUT@'])

psp_gateway_generated_proto_src = protobuf_gen_cc.process('psp_gateway.proto')
psp_gateway_generated_grpc_src = grpc_gen_cc.process('psp_gateway.proto')

lib_psp_gateway_grpc = static_library('psp_gateway_grpc',
    psp_gateway_generated_proto_src,
    psp_gateway_generated_grpc_src,
    cpp_args: grpc_cpp_args,
    override_options: ['cpp_std=c++17'],
    dependencies : grpc_dependencies)

dependency_psp_gateway_grpc = declare_dependency(
    link_with: lib_psp_gateway_grpc,
    include_directories: lib_psp_gateway_grpc.private_dir_include())

# Generate files also for Python bindings
protobuf_gen_py = generator(protoc, \
                output    : ['@BASENAME@_pb2.py'],
                arguments : ['--proto_path=@CURRENT_SOURCE_DIR@',
                    '--python_out=@BUILD_DIR@',
                    '@INPUT@'])

grpc_gen_py = generator(protoc, \
            output    : ['@BASENAME@_pb2_grpc.py'],
            arguments : ['--proto_path=@CURRENT_SOURCE_DIR@',
                '--grpc_out=@BUILD_DIR@',
                '--plugin=protoc-gen-grpc=' + grpc_py_plugin.full_path(),
                '@INPUT@'])

psp_gateway_generated_proto_py = protobuf_gen_py.process('psp_gateway.proto')
psp_gateway_generated_grpc_py = grpc_gen_py.process('psp_gateway.proto')

python_targets = [
    [APP_NAME + '_py_proto', psp_gateway_generated_proto_py],
    [APP_NAME + '_py_grpc', psp_gateway_generated_grpc_py]
]

# Hack to force the generation of non-executable generated files
foreach gen : python_targets
    py_proc = custom_target(gen[0],
                command: [ python_generator, '@INPUT@', '@OUTPUT@' ],
                input : gen[1],
                output : '@BASENAME@.py',
                build_by_default : true)
endforeach

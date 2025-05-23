#!/bin/bash
# Copyright 2018 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Generate the list of boringssl symbols that need to be renamed based on the
# current boringssl submodule. The script should be run after a boringssl
# upgrade in third_party/boringssl-with-bazel. Note that after the script is
# run, you will typically need to manually upgrade the BoringSSL-GRPC podspec
# (templates/src/objective-c/BoringSSL-GRPC.podspec.template) version and the
# corresponding version number in gRPC-Core podspec
# (templates/gRPC-Core.podspec.template).

set -ev

BORINGSSL_ROOT=third_party/boringssl-with-bazel/src

cd "$(dirname $0)"
cd ../../$BORINGSSL_ROOT

BORINGSSL_COMMIT=$(git rev-parse HEAD)
BORINGSSL_PREFIX_HEADERS_DIR=src/boringssl

rm -rf build
mkdir -p build
cd build
cmake ..
make -j
[ -f libssl.a ] || { echo "Failed to build libssl.a" ; exit 1 ; }
[ -f libcrypto.a ] || { echo "Failed to build libcrypto.a" ; exit 1 ; }

# Generates boringssl_prefix_symbols.h. The prefix header is generated by
# BoringSSL's build system as instructed by BoringSSL build guide (see
# https://github.com/google/boringssl/blob/367d64f84c3c1d01381c18c5a239b85eef47633c/BUILDING.md#building-with-prefixed-symbols).
go run ../util/read_symbols.go libssl.a > ./symbols.txt
go run ../util/read_symbols.go libcrypto.a >> ./symbols.txt
cmake .. -DBORINGSSL_PREFIX=GRPC -DBORINGSSL_PREFIX_SYMBOLS=symbols.txt
make boringssl_prefix_symbols
[ -f symbol_prefix_include/boringssl_prefix_symbols.h ] || { echo "Failed to build boringssl_prefix_symbols.sh" ; exit 1 ; }

cd ../../../..
mkdir -p $BORINGSSL_PREFIX_HEADERS_DIR
echo "// generated by generate_boringssl_prefix_header.sh on BoringSSL commit: $BORINGSSL_COMMIT" > $BORINGSSL_PREFIX_HEADERS_DIR/boringssl_prefix_symbols.h
echo "" >> $BORINGSSL_PREFIX_HEADERS_DIR/boringssl_prefix_symbols.h
cat "$BORINGSSL_ROOT/build/symbol_prefix_include/boringssl_prefix_symbols.h" >> $BORINGSSL_PREFIX_HEADERS_DIR/boringssl_prefix_symbols.h

# Regenerated the project
tools/buildgen/generate_projects.sh

exit 0

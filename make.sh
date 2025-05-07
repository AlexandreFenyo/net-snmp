#!/bin/zsh

cd /Users/fenyo/git3/net-snmp

ScriptDir="$( cd "$( dirname "$0" )" && pwd )"
cd - &> /dev/null
PREFIX="${ScriptDir}"/_build
PLATFORMS="${PREFIX}"/platforms
UNIVERSAL="${PREFIX}"/universal

# Compiler options
OPT_FLAGS="-O3 -g3 -fembed-bitcode"
MAKE_JOBS=8
MIN_IOS_VERSION=8.0

# Build for platforms
SDK="iphoneos"
PLATFORM="arm"
PLATFORM_ARM=${PLATFORM}
ARCH_FLAGS="-arch arm64 -arch arm64e"  # -arch armv7 -arch armv7s
HOST_FLAGS="${ARCH_FLAGS} -miphoneos-version-min=${MIN_IOS_VERSION} -isysroot $(xcrun --sdk ${SDK} --show-sdk-path)"
CHOST="arm-apple-darwin"

cd net-snmp-5.9.4

export CC=$(xcrun --find --sdk "${SDK}" clang)
export CXX=$(xcrun --find --sdk "${SDK}" clang++)
# export CPP=$(xcrun --find --sdk "${SDK}" cpp)
export CPP="$ScriptDir/mycpp"
   
export CFLAGS="${HOST_FLAGS} ${OPT_FLAGS}"
export CXXFLAGS="${HOST_FLAGS} ${OPT_FLAGS}"
export LDFLAGS="${HOST_FLAGS}"

EXEC_PREFIX="${PLATFORMS}/${PLATFORM}"

make V=1 -j"${MAKE_JOBS}" --debug=j
make install

cd /Users/fenyo/git3/net-snmp
cp _build/**/lib*.a lib ; cp lib/libnetsnmp* ~/git3/iOS-tools/iOS\ tools/libnetsnmp

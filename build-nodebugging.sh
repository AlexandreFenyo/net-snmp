#! /bin/sh

#
# Build for iOS 64bit-ARM variants and iOS Simulator
# - Place the script at project root
# - Customize MIN_IOS_VERSION and other flags as needed
# 
# Test Environment
# - macOS 10.14.6
# - iOS 13.1
# - Xcode 11.1
#

Build() {
    cd net-snmp-5.9.4

    # Ensure -fembed-bitcode builds, as workaround for libtool macOS bug
    
    #    export MACOSX_DEPLOYMENT_TARGET="10.4"
    # Get the correct toolchain for target platforms
    export CC=$(xcrun --find --sdk "${SDK}" clang)
    export CXX=$(xcrun --find --sdk "${SDK}" clang++)

    # export CPP=$(xcrun --find --sdk "${SDK}" cpp)
    export CPP="$ScriptDir/mycpp"
   
    export CFLAGS="${HOST_FLAGS} ${OPT_FLAGS}"
    export CXXFLAGS="${HOST_FLAGS} ${OPT_FLAGS}"
    export LDFLAGS="${HOST_FLAGS}"

    EXEC_PREFIX="${PLATFORMS}/${PLATFORM}"

    ./configure \
        --host="${CHOST}" \
        --prefix="${PREFIX}" \
        --exec-prefix="${EXEC_PREFIX}" \
        --enable-static \
	--disable-agent \
	--enable-reentrant \
        --disable-shared  # Avoid Xcode loading dylibs even when staticlibs exist


#    	--with-mibdirs="\$HOME/Documents/mibs"
#	--with-mibdirs=""

    make clean
    mkdir -p "${PLATFORMS}" &> /dev/null
    make V=1 -j"${MAKE_JOBS}" --debug=j
    make install
}

echo "HI"

# Locations
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
Build
exit 0

SDK="iphonesimulator"
PLATFORM="arm64-sim"
PLATFORM_ISIM=${PLATFORM}
ARCH_FLAGS="-arch arm64"
#ARCH_FLAGS="-arch x86_64"
HOST_FLAGS="${ARCH_FLAGS} -mios-simulator-version-min=${MIN_IOS_VERSION} -isysroot $(xcrun --sdk ${SDK} --show-sdk-path)"
CHOST="arm-apple-darwin"
Build

exit 0

# Build for platforms
SDK="iphoneos"
PLATFORM="arm"
PLATFORM_ARM=${PLATFORM}
ARCH_FLAGS="-arch arm64 -arch arm64e"  # -arch armv7 -arch armv7s
HOST_FLAGS="${ARCH_FLAGS} -miphoneos-version-min=${MIN_IOS_VERSION} -isysroot $(xcrun --sdk ${SDK} --show-sdk-path)"
CHOST="arm-apple-darwin"
Build

exit 0

# Create universal binary
cd "${PLATFORMS}/${PLATFORM_ARM}/lib"
LIB_NAME=`find . -iname *.a`
cd -
mkdir -p "${UNIVERSAL}" &> /dev/null
lipo -create -output "${UNIVERSAL}/${LIB_NAME}" "${PLATFORMS}/${PLATFORM_ARM}/lib/${LIB_NAME}" "${PLATFORMS}/${PLATFORM_ISIM}/lib/${LIB_NAME}"

echo "BYE"


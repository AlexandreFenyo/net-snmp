#!/bin/zsh
#
# Reconstruit les 3 tranches de libnetsnmp.a utilisées par l'app iOS-tools
# (WiFi Heat Map & Analyzer) à partir des sources patchées de ce dépôt :
#   appareil      : arm64 + arm64e, iphoneos, iOS 16.6 minimum
#   simulateur    : arm64, iphonesimulator, iOS 16.6 minimum
#   Mac Catalyst  : arm64 + x86_64 (universelle), macabi, iOS 16.6 minimum
#
# Usage : ./build-ios-tools.sh <répertoire de travail> [répertoire libnetsnmp de l'app]
#   Les sources sont copiées dans le répertoire de travail (le dépôt n'est pas modifié).
#   Si le répertoire libnetsnmp de l'app est donné (ex. ".../iOS tools/libnetsnmp"), les
#   trois tranches y sont copiées (libnetsnmp.a, simulator/, maccatalyst/).
#
# Pièges rencontrés (sept. 2026, Xcode 27) :
#   - configure pose des questions interactives : --with-defaults et stdin sur /dev/null
#   - le dépôt contient des objets déjà compilés (ignorés par git) : distclean + suppression
#     des .o/.lo/.a de la copie, sinon des objets d'une autre plate-forme sont réutilisés
#   - Catalyst : les outils annexes (snmppcap) ne compilent pas (pcap indisponible) ; seule
#     la bibliothèque snmplib est construite, ce qui suffit à l'app
#   - Catalyst x86_64 : l'assembleur en ligne x86 des MD5/SHA intégrés est refusé par clang,
#     d'où -DOPENSSL_NO_INLINE_ASM (version C portable, même résultat)
#
set -e
[ -n "$1" ] || { sed -n '2,24p' "$0"; exit 1; }
W=${1:A}; DEST=${2:+${2:A}}; SRC=${0:A:h}
mkdir -p $W

build() { # <nom> <sdk> <chost> <cflags d'architecture et de cible> <cible make>
    local name=$1 sdk=$2 chost=$3 archflags=$4 target=$5
    rm -rf $W/src-$name; mkdir -p $W/src-$name
    cp -a $SRC/net-snmp-5.9.4 $W/src-$name/; cp -a $SRC/mycpp $W/src-$name/
    cd $W/src-$name/net-snmp-5.9.4
    [ -f Makefile ] && make distclean >/dev/null 2>&1 || true
    find . \( -name "*.o" -o -name "*.lo" -o -name "*.a" \) -delete
    local sysroot=$(xcrun --sdk $sdk --show-sdk-path)
    export CC=$(xcrun --find --sdk $sdk clang) CXX=$(xcrun --find --sdk $sdk clang++) CPP=$W/src-$name/mycpp
    export CFLAGS="$archflags -isysroot $sysroot -O3 -g3" CXXFLAGS="$archflags -isysroot $sysroot -O3 -g3" LDFLAGS="$archflags -isysroot $sysroot"
    echo "=== $name : configure"
    ./configure --host=$chost --prefix=$W/out-$name --exec-prefix=$W/out-$name \
        --enable-static --disable-agent --enable-reentrant --disable-shared --with-defaults \
        < /dev/null > $W/configure-$name.log 2>&1
    echo "=== $name : make $target"
    if [ "$target" = lib ]; then
        make -C snmplib -j8 > $W/make-$name.log 2>&1
    else
        make -j8 > $W/make-$name.log 2>&1
    fi
    mkdir -p $W/out-$name/lib; cp snmplib/.libs/libnetsnmp.a $W/out-$name/lib/
    lipo -info $W/out-$name/lib/libnetsnmp.a
}

build device iphoneos arm-apple-darwin "-arch arm64 -arch arm64e -miphoneos-version-min=16.6" all
build sim iphonesimulator arm-apple-darwin "-arch arm64 -mios-simulator-version-min=16.6" all
build mac-arm64 macosx arm-apple-darwin "-target arm64-apple-ios16.6-macabi" lib
build mac-x86_64 macosx x86_64-apple-darwin "-target x86_64-apple-ios16.6-macabi -DOPENSSL_NO_INLINE_ASM" lib
lipo -create $W/out-mac-arm64/lib/libnetsnmp.a $W/out-mac-x86_64/lib/libnetsnmp.a -output $W/libnetsnmp-maccatalyst.a
lipo -info $W/libnetsnmp-maccatalyst.a

if [ -n "$DEST" ]; then
    cp $W/out-device/lib/libnetsnmp.a $DEST/libnetsnmp.a
    cp $W/out-sim/lib/libnetsnmp.a $DEST/simulator/libnetsnmp.a
    cp $W/libnetsnmp-maccatalyst.a $DEST/maccatalyst/libnetsnmp.a
    echo "=== tranches copiées dans $DEST"
fi

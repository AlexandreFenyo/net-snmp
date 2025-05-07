#!/bin/zsh

cd /Users/fenyo/git3/net-snmp
yes "" | head -6 | ./build-nodebugging.sh
cp _build/**/lib*.a lib
cp lib/libnetsnmp* ~/git3/iOS-tools/iOS\ tools/libnetsnmp

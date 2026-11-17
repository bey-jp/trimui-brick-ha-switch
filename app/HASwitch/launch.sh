#!/bin/sh
# TRIMUI のアプリ一覧から起動される
progdir=`dirname "$0"`
cd "$progdir"
echo 1 > /tmp/stay_awake        # 起動中は自動スリープしない
./haswitch "$progdir" > /tmp/haswitch.log 2>&1
rm -f /tmp/stay_awake

#!/bin/sh
# Pi5 (aarch64) でネイティブにビルド。Brick は glibc 2.33 なので静的リンクにする
cd "$(dirname "$0")"
gcc -O2 -static -Wall -Wno-unused-function -o ../app/HASwitch/haswitch haswitch.c -lm
gcc -O2 -static -Wall -o ../system/brickkeyd brickkeyd.c

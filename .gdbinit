set architecture aarch64
target remote localhost:1234
set substitute-path /chos/ ./
layout split

file ./build/kernel.img

set logging on
add-symbol-file ./userland/_install/yield_spin.bin
b main.c:99
fs cmd
set architecture aarch64
target remote localhost:1234
set substitute-path /chos/ ./
layout split

file ./build/kernel.img

set logging on
set scheduler-locking step
b page_table.c:238
fs cmd
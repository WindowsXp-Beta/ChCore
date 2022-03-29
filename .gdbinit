set architecture aarch64
target remote localhost:1234
set substitute-path /chos/ ./
layout split

file ./build/kernel.img

set logging on
set scheduler-locking step
b page_table.c:463
b page_table.c:340
b page_table.c:357
dis 2
dis 3
fs cmd
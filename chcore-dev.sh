#!/usr/bin/bash

function start() {
    touch gdb.txt
    tmux new-session -d -s chcore-dev "make qemu-gdb"
    tmux new-window -t chcore-dev: "make gdb"
    tmux new-window -t chcore-dev: "set trace-commands on && set logging on && tail -f ./gdb.txt"
}

function end() {
    tmux send-keys -t chcore-dev:0 C-a x Enter
    tmux send-keys -t chcore-dev:1 quit Enter
    tmux kill-session -t chcore-dev
    test -f gdb.txt && rm gdb.txt
}

if [ $# -ne 1 ]; then
    echo "type start to start\ntype kill to kill"
elif [ "$1" = 'start' ]; then
    start
elif [ "$1" = 'kill' ]; then
    end
elif [ "$1" = 'restart' ]; then
    end
    start
else
    echo "Unsupported argument."
fi
#! /bin/bash

echo Dumping backtraces ... Start
sudo apt install gdb
for PID in $(pgrep -f Build)
do
    echo "${PID}"
    sudo gdb -batch -x gdb_commands.txt -p "${PID}"
done
echo Dumping backtraces ... End

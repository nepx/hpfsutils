@echo off
gcc -O2 hpfsimg.c -o hpfs.exe
gcc -O2 inspect.c -o inspect.exe
gcc -O2 mkhpfs.c -o mkhpfs.exe
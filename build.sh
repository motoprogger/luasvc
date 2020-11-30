#! /bin/sh
# This script builds luasvc linked against Lua 5.2 library.
cc -llua-5.2 -lm -I/usr/local/include/lua52/ -L/usr/local/lib -o luasvc luasvc.c

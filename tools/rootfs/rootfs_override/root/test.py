#!/usr/bin/env python3

import os
import time

print("=== EdgeOS Basic Python Test ===")

print("[*] Loop test...")
count = 0
for i in range(1000000):
    count += 1
print("[+] Loop OK:", count)

print("[*] String test...")
s = ""
for i in range(10000):
    s += "A"
print("[+] String length:", len(s))

print("[*] Memory test...")
data = []
for i in range(10000):
    data.append("X" * 100)
del data
print("[+] Memory OK")

print("[*] File test...")
filename = "testfile.txt"
f = open(filename, "w")
for i in range(1000):
    f.write("EdgeOS Test Line %d\n" % i)
f.close()

f = open(filename, "r")
lines = f.readlines()
f.close()

os.remove(filename)
print("[+] File lines read:", len(lines))

print("[*] Fork test...")
try:
    pid = os.fork()
    if pid == 0:
        print("Child process running")
        os._exit(0)
    else:
        os.wait()
        print("[+] Fork OK")
except AttributeError:
    print("[!] Fork not supported")

print("[*] Exception test...")
try:
    x = 1 / 0
except ZeroDivisionError:
    print("[+] Exception OK")

print("=== Basic Test Completed ===")
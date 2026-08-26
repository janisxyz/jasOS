#!/usr/bin/env python3
"""Turn a binary into a C header with a byte array. Used to seed /bin/hello."""
import sys

if len(sys.argv) != 4:
    sys.stderr.write("usage: embed_elf.py infile outfile symbol\n")
    sys.exit(1)
src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()
with open(dst, "w") as f:
    f.write("/* generated — do not edit */\n#pragma once\n")
    f.write("static const unsigned char %s[] = {\n" % name)
    for i, b in enumerate(data):
        if i % 16 == 0:
            f.write("    ")
        f.write("%u," % b)
        if i % 16 == 15:
            f.write("\n")
    if len(data) % 16:
        f.write("\n")
    f.write("};\nstatic const unsigned int %s_len = %u;\n" % (name, len(data)))

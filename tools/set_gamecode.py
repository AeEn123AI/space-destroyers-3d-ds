#!/usr/bin/env python3
"""Copy an .nds with another 4-letter game code in its header, fixing the header CRC. usage: set_gamecode.py in out CODE"""
import sys
src, dst, code = sys.argv[1:4]
b = bytearray(open(src, 'rb').read())
b[0x0C:0x10] = code.encode()
crc = 0xFFFF
for x in b[:0x15E]:
    crc ^= x
    for _ in range(8):
        crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
b[0x15E:0x160] = crc.to_bytes(2, 'little')
open(dst, 'wb').write(b)

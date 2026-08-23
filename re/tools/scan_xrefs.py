#!/usr/bin/env python3
"""adrp+ldr/add 交叉引用扫描器：在 AArch64 ELF .text 中定位对指定 rodata 常量的引用。

用法：python3 scan_xrefs.py [so路径]   （默认：脚本同级 ../Phira_0.8.2/lib/arm64-v8a/libphira.so）
换版本时先 readelf -lW 更新 TEXT_OFF/TEXT_VA，再按 rodata 实际地址更新 TARGETS。
"""
import os
import struct
import sys

TEXT_OFF = 0x6EC200   # exec LOAD p_offset
TEXT_VA = 0x6F0200    # exec LOAD p_vaddr
TEXT_SZ = 0x10D8A00
# vaddr == file_off（LOAD#1 R 段）
TARGETS = {
    0x277D48: 'LIMIT_PERFECT',
    0x2783D0: 'LIMIT_GOOD_A',
    0x279EA0: 'LIMIT_GOOD_B',
    0x27BC50: 'X_DIFF_MAX',
}


def sx(v, b):
    return v - (1 << b) if v >> (b - 1) else v


def main(path):
    data = open(path, 'rb').read()
    sites = {}
    for off in range(TEXT_OFF, TEXT_OFF + TEXT_SZ - 32, 4):
        w = struct.unpack_from('<I', data, off)[0]
        # adrp: bit31=1, bits28..24=10000
        if not ((w >> 31) & 1) or ((w >> 24) & 0x1F) != 0x10:
            continue
        imm = sx((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21) << 12
        pc_va = TEXT_VA + (off - TEXT_OFF)
        page = (pc_va & ~0xFFF) + imm
        if not (0x270000 <= page < 0x282000):  # 只看常量所在页附近
            continue
        for k in range(1, 7):  # adrp 后 ≤6 条内取用
            w2 = struct.unpack_from('<I', data, off + 4 * k)[0]
            op = w2 >> 22
            if op in (0x3E5, 0x3F5):  # ldr x/d [xn,#imm12]
                t = page + (((w2 >> 10) & 0xFFF) * 8)
            elif op == 0x3D5:  # ldr s
                t = page + (((w2 >> 10) & 0xFFF) * 4)
            elif op == 0x244:  # add xn,xn,#imm12
                t = page + ((w2 >> 10) & 0xFFF)
            else:
                continue
            if t in TARGETS:
                sites.setdefault(t, set()).add(pc_va + 4 * k)
    # 无 break：adrp 后 6 条内所有取用都记录（无寄存器跟踪，宁滥勿缺，set 去重）
    for addr, name in sorted(TARGETS.items(), key=lambda kv: kv[1]):
        vas = sites.get(addr)
        print(f'{name} ({addr:#x}): '
              f'{[hex(v) for v in sorted(vas)] if vas else "(no refs)"}')


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    main(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(here, '..', 'Phira_0.8.2', 'lib', 'arm64-v8a', 'libphira.so'))

from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64


pc = 0x4001A9
decoder = Cs(CS_ARCH_X86, CS_MODE_64)
for name in sys.argv[1:]:
    path = Path(name)
    data = path.read_bytes()
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    found = False
    print(f"=== {path.name} ===")
    for i in range(phnum):
        p_type, flags, offset, vaddr, paddr, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + i * phentsize
        )
        if p_type != 1 or not (vaddr <= pc < vaddr + filesz):
            continue
        found = True
        image = data[offset:offset + filesz]
        relative = pc - vaddr
        start = max(0, relative - 16)
        end = min(len(image), relative + 32)
        print(f"PT_LOAD va=0x{vaddr:x} file_offset=0x{offset:x} bytes={filesz}")
        print(f"bytes around RIP: {image[start:end].hex(' ')}")
        for insn in decoder.disasm(image[start:], vaddr + start):
            marker = " <-- RIP" if insn.address == pc else ""
            print(f"0x{insn.address:016x}: {insn.bytes.hex(' '):<20} {insn.mnemonic} {insn.op_str}{marker}")
            if insn.address >= pc + 15:
                break
    if not found:
        print(f"no PT_LOAD covers RIP 0x{pc:x}")

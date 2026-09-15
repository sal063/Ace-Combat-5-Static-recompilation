import struct
from dataclasses import dataclass, field
from typing import List, Optional

ET_EXEC = 2
EM_MIPS = 8
PT_LOAD = 1

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_NOBITS = 8

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4

STT_FUNC = 2
STT_OBJECT = 1


@dataclass
class Segment:
    type: int
    offset: int
    vaddr: int
    paddr: int
    filesz: int
    memsz: int
    flags: int
    align: int
    data: bytes = b""


@dataclass
class Section:
    name: str
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    info: int
    align: int
    entsize: int
    data: bytes = b""

    @property
    def is_alloc(self) -> bool:
        return bool(self.flags & SHF_ALLOC)

    @property
    def is_exec(self) -> bool:
        return bool(self.flags & SHF_EXECINSTR)


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    bind: int
    type: int
    shndx: int


class ElfFile:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as fp:
            self.data = fp.read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise ValueError(f"{path}: not an ELF file")
        if d[4] != 1 or d[5] != 1:
            raise ValueError(f"{path}: expected 32-bit little-endian ELF")
        (self.e_type, self.e_machine, self.e_version, self.entry, self.phoff,
         self.shoff, self.eflags, self.ehsize, self.phentsize, self.phnum,
         self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from(
            "<HHIIIIIHHHHHH", d, 0x10)
        if self.e_machine != EM_MIPS:
            raise ValueError(f"{path}: not a MIPS ELF (machine={self.e_machine})")

        self.segments: List[Segment] = []
        for i in range(self.phnum):
            o = self.phoff + i * self.phentsize
            (p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
             p_flags, p_align) = struct.unpack_from("<8I", d, o)
            seg = Segment(p_type, p_offset, p_vaddr, p_paddr, p_filesz,
                          p_memsz, p_flags, p_align,
                          d[p_offset:p_offset + p_filesz])
            self.segments.append(seg)

        self.sections: List[Section] = []
        if self.shoff and self.shnum:
            raw = []
            for i in range(self.shnum):
                o = self.shoff + i * self.shentsize
                vals = struct.unpack_from("<10I", d, o)
                raw.append(vals)
            shstr_off, shstr_size = raw[self.shstrndx][4], raw[self.shstrndx][5]
            shstr = d[shstr_off:shstr_off + shstr_size]
            for (nm, typ, flags, addr, off, size, link, info,
                 align, entsize) in raw:
                end = shstr.find(b"\0", nm)
                name = shstr[nm:end].decode("latin1")
                body = b"" if typ == SHT_NOBITS else d[off:off + size]
                self.sections.append(Section(name, typ, flags, addr, off, size,
                                             link, info, align, entsize, body))

        self.symbols: List[Symbol] = self._read_symbols()

    def _read_symbols(self) -> List[Symbol]:
        out: List[Symbol] = []
        for s in self.sections:
            if s.type != SHT_SYMTAB:
                continue
            strs = self.sections[s.link].data
            n = s.size // 16
            for i in range(n):
                o = s.offset + i * 16
                nameoff, value, size, info, other, shndx = struct.unpack_from(
                    "<IIIBBH", self.data, o)
                end = strs.find(b"\0", nameoff)
                name = strs[nameoff:end].decode("latin1")
                out.append(Symbol(name, value, size, info >> 4, info & 0xF, shndx))
        return out

    def image(self):
        loads = [s for s in self.segments if s.type == PT_LOAD and s.memsz]
        if not loads:
            raise ValueError("no PT_LOAD segments")
        base = min(s.vaddr for s in loads)
        top = max(s.vaddr + s.memsz for s in loads)
        buf = bytearray(top - base)
        for s in loads:
            off = s.vaddr - base
            buf[off:off + s.filesz] = s.data
        return base, buf

    def text_ranges(self):
        out = []
        for s in self.sections:
            if s.is_alloc and s.is_exec and s.size and s.name != ".vutext":
                out.append((s.addr, s.addr + s.size))
        if not out:
            for s in self.segments:
                if s.type == PT_LOAD and (s.flags & 1):
                    out.append((s.vaddr, s.vaddr + s.filesz))
        return sorted(out)

    def section(self, name: str) -> Optional[Section]:
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def read(self, addr: int, size: int) -> bytes:
        for s in self.segments:
            if s.type != PT_LOAD:
                continue
            if s.vaddr <= addr and addr + size <= s.vaddr + s.filesz:
                o = addr - s.vaddr
                return s.data[o:o + size]
        return b"\0" * size

    def word(self, addr: int) -> int:
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if len(b) == 4 else 0

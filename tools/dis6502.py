#!/usr/bin/env python3
# dis6502.py — desassembleur 6502 minimal. Usage: dis6502.py <rom.bin> <baseHex> <startHex> [nBytes]
import sys

M = {}  # opcode -> (mnemonique, mode, taille)
def add(table, mode, size):
    for mn, op in table.items():
        if op is not None: M[op] = (mn, mode, size)

add({'BRK':0x00,'PHP':0x08,'CLC':0x18,'PLP':0x28,'SEC':0x38,'RTI':0x40,'PHA':0x48,'CLI':0x58,
     'RTS':0x60,'PLA':0x68,'SEI':0x78,'DEY':0x88,'TXA':0x8A,'TYA':0x98,'TXS':0x9A,'TAY':0xA8,
     'TAX':0xAA,'CLV':0xB8,'TSX':0xBA,'INY':0xC8,'DEX':0xCA,'CLD':0xD8,'INX':0xE8,'NOP':0xEA,'SED':0xF8}, 'imp', 1)
add({'ORA':0x09,'AND':0x29,'EOR':0x49,'ADC':0x69,'LDY':0xA0,'LDX':0xA2,'LDA':0xA9,'CPY':0xC0,'CMP':0xC9,'CPX':0xE0,'SBC':0xE9}, 'imm', 2)
add({'ORA':0x05,'AND':0x25,'EOR':0x45,'ADC':0x65,'STA':0x85,'LDA':0xA5,'CMP':0xC5,'SBC':0xE5,
     'ASL':0x06,'ROL':0x26,'LSR':0x46,'ROR':0x66,'STX':0x86,'LDX':0xA6,'DEC':0xC6,'INC':0xE6,
     'BIT':0x24,'STY':0x84,'LDY':0xA4,'CPY':0xC4,'CPX':0xE4}, 'zp', 2)
add({'ORA':0x15,'AND':0x35,'EOR':0x55,'ADC':0x75,'STA':0x95,'LDA':0xB5,'CMP':0xD5,'SBC':0xF5,
     'ASL':0x16,'ROL':0x36,'LSR':0x56,'ROR':0x76,'DEC':0xD6,'INC':0xF6,'STY':0x94,'LDY':0xB4}, 'zpx', 2)
add({'STX':0x96,'LDX':0xB6}, 'zpy', 2)
add({'ORA':0x0D,'AND':0x2D,'EOR':0x4D,'ADC':0x6D,'STA':0x8D,'LDA':0xAD,'CMP':0xCD,'SBC':0xED,
     'ASL':0x0E,'ROL':0x2E,'LSR':0x4E,'ROR':0x6E,'STX':0x8E,'LDX':0xAE,'DEC':0xCE,'INC':0xEE,
     'BIT':0x2C,'STY':0x8C,'LDY':0xAC,'CPY':0xCC,'CPX':0xEC,'JMP':0x4C,'JSR':0x20}, 'abs', 3)
add({'ORA':0x1D,'AND':0x3D,'EOR':0x5D,'ADC':0x7D,'STA':0x9D,'LDA':0xBD,'CMP':0xDD,'SBC':0xFD,
     'ASL':0x1E,'ROL':0x3E,'LSR':0x5E,'ROR':0x7E,'DEC':0xDE,'INC':0xFE,'LDY':0xBC}, 'abx', 3)
add({'ORA':0x19,'AND':0x39,'EOR':0x59,'ADC':0x79,'STA':0x99,'LDA':0xB9,'CMP':0xD9,'SBC':0xF9,'LDX':0xBE}, 'aby', 3)
add({'ORA':0x01,'AND':0x21,'EOR':0x41,'ADC':0x61,'STA':0x81,'LDA':0xA1,'CMP':0xC1,'SBC':0xE1}, 'izx', 2)
add({'ORA':0x11,'AND':0x31,'EOR':0x51,'ADC':0x71,'STA':0x91,'LDA':0xB1,'CMP':0xD1,'SBC':0xF1}, 'izy', 2)
add({'ASL':0x0A,'ROL':0x2A,'LSR':0x4A,'ROR':0x6A}, 'acc', 1)
add({'BPL':0x10,'BMI':0x30,'BVC':0x50,'BVS':0x70,'BCC':0x90,'BCS':0xB0,'BNE':0xD0,'BEQ':0xF0}, 'rel', 2)
add({'JMP':0x6C}, 'ind', 3)

def dis(rom, base, start, n=160):
    pc = start
    out = []
    while pc - start < n and 0 <= pc - base < len(rom):
        o = pc - base
        op = rom[o]
        if op not in M:
            out.append(f"{pc:04X}: {op:02X}        ???"); pc += 1; continue
        mn, mode, sz = M[op]
        b = rom[o:o+sz]
        if   mode == 'imp' or mode == 'acc': arg = ''
        elif mode == 'imm': arg = f"#${b[1]:02X}"
        elif mode == 'zp':  arg = f"${b[1]:02X}"
        elif mode == 'zpx': arg = f"${b[1]:02X},X"
        elif mode == 'zpy': arg = f"${b[1]:02X},Y"
        elif mode == 'abs': arg = f"${b[2]:02X}{b[1]:02X}"
        elif mode == 'abx': arg = f"${b[2]:02X}{b[1]:02X},X"
        elif mode == 'aby': arg = f"${b[2]:02X}{b[1]:02X},Y"
        elif mode == 'izx': arg = f"(${b[1]:02X},X)"
        elif mode == 'izy': arg = f"(${b[1]:02X}),Y"
        elif mode == 'ind': arg = f"(${b[2]:02X}{b[1]:02X})"
        elif mode == 'rel':
            d = b[1] - 256 if b[1] > 127 else b[1]
            arg = f"${pc + 2 + d:04X}"
        out.append(f"{pc:04X}: {' '.join(f'{x:02X}' for x in b):<9} {mn} {arg}")
        pc += sz
    return '\n'.join(out)

if __name__ == '__main__':
    rom = open(sys.argv[1], 'rb').read()
    base, start = int(sys.argv[2], 16), int(sys.argv[3], 16)
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 160
    print(dis(rom, base, start, n))

#!/usr/bin/env python3
"""Generate real plugin formatting call sites, then check pinned libk on ARM.

Generation uses only Python's standard library. Emulation requires Unicorn and
pyelftools on Linux; this is an ABI regression check, not a Vita runtime test.
"""
import argparse
import json
from pathlib import Path
import re


def call(source, prefix):
    found = re.search(re.escape(prefix) + r'.*?;\n', source, re.S)
    if not found:
        raise ValueError('formatting call missing: ' + prefix)
    return found[0]


def generate(old_path, current_path, output):
    old = old_path.read_text()
    current = current_path.read_text()
    log_prefix = 'snprintf(line, sizeof(line),'
    session_prefix = 'snprintf(session, sizeof(session),'
    old_log, new_log = [call(s, log_prefix) for s in (old, current)]
    old_session, new_session = [call(s, session_prefix) for s in (old, current)]
    text = '''#include <libk/stdio.h>
#include <libk/string.h>
#include <stdint.h>
#include "bootstrap.h"
char output[192];
static const char *last_operation = "none";
const char *tb_phase_name(unsigned phase) { return "PREFLIGHT"; }
uint64_t sceKernelGetSystemTimeWide(void) { return 0x99aabbccddeeff00ULL; }
'''
    for name, statement in [('probe_bad', old_log), ('probe_good', new_log)]:
        text += f'''void {name}(void) {{
 char line[192];
 TBState value = {{.sequence=1,.phase=TB_PREFLIGHT,.index=0,.count=1,.owned=0,.pid=-1,.error=0}};
 const TBState *s = &value;
 {statement}
 memcpy(output,line,sizeof(line));
}}
'''
    for name, statement in [('probe_bad_session', old_session), ('probe_session', new_session)]:
        text += f'''void {name}(void) {{
 char session[33];
 struct {{ uint64_t tick; }} rtc = {{0x1122334455667788ULL}};
 uint64_t monotonic = sceKernelGetSystemTimeWide();
 {statement}
 memcpy(output,session,sizeof(session));
}}
'''
    output.write_text(text)


def emulate(path):
    from elftools.elf.elffile import ELFFile
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_LITTLE_ENDIAN, UcError
    from unicorn.arm_const import (UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R3,
                                  UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC)
    with path.open('rb') as handle:
        elf = ELFFile(handle)
        segments = [(s['p_vaddr'], s['p_memsz'], s.data()) for s in elf.iter_segments()
                    if s['p_type'] == 'PT_LOAD']
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    expected_session = '112233445566778899aabbccddeeff00'
    expected_log = 'seq_hex=0000000000000001 phase=PREFLIGHT item=0/1 owned=0 pid=-1 error=00000000 operation=none\n'
    for name in ['probe_bad', 'probe_good', 'probe_bad_session', 'probe_session']:
        machine = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_LITTLE_ENDIAN)
        pages = {p for address, size, _ in segments
                 for p in range(address & ~4095, (address + size + 4095) & ~4095, 4096)}
        for page in sorted(pages):
            machine.mem_map(page, 4096)
        for address, _, data in segments:
            machine.mem_write(address, data)
        machine.mem_map(0x200000, 0x20000)
        machine.mem_write(0x200000, b'\xef\xbe\xad\xde' * (0x20000 // 4))
        machine.reg_write(UC_ARM_REG_SP, 0x210000)
        machine.reg_write(UC_ARM_REG_LR, 0x80000)
        machine.reg_write(UC_ARM_REG_R3, 0xdeadbeef)
        machine.reg_write(UC_ARM_REG_C1_C0_2, 0xf << 20)
        machine.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        try:
            machine.emu_start(symbols[name], 0x80000, count=1000000)
            error = None
        except UcError as ex:
            error = str(ex)
        output = bytes(machine.mem_read(symbols['output'], 192)).split(b'\0')[0].decode()
        print(json.dumps({'probe': name, 'error': error, 'output': output}))
        if name == 'probe_bad':
            assert error and 'READ_UNMAPPED' in error
        elif name == 'probe_bad_session':
            assert error or output != expected_session
        else:
            assert error is None
            assert output == (expected_log if name == 'probe_good' else expected_session)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    gen = sub.add_parser('generate')
    gen.add_argument('old', type=Path)
    gen.add_argument('current', type=Path)
    gen.add_argument('output', type=Path)
    run = sub.add_parser('emulate')
    run.add_argument('elf', type=Path)
    args = parser.parse_args()
    if args.action == 'generate':
        generate(args.old, args.current, args.output)
    else:
        emulate(args.elf)

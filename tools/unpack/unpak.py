#!/usr/bin/env python3
"""unpak.py - extract data.pak from ExplodeArena (Symbian S60, iDreams/"DDJ" build).

usage:  python3 unpak.py ExplodeArena.app data.pak OUTDIR

data.pak layout (reverse-engineered from ExplodeArena.app: Pak_Open @0x00acd8, Pak_ReadAt @0x00a9f4)
    0x00  16 bytes   zero (unused)
    0x10  u32        entry count (stored in clear)
    0x14  count * 16 index, every byte XOR 0xAA, little endian fields:
                       u32 hash       Pak_HashName(filename)
                       u32 offset     absolute file offset of the payload
                       u32 usize      uncompressed size
                       u32 type<<24 | csize   type: 0 raw, 2 LZARI, 3/4 "IMP" container
    payloads: every byte XOR 0xAA, then
        type 2 : u32 usize, then Okumura LZARI stream          -> lzari.py (pure Python)
        type 3 : "IMP\\n" block container (custom LZ/Huffman)    -> executed with armemu.c, i.e. the
                 game's own Imp_Unpack() routine (0x011bc8) is run in a tiny ARM interpreter.

hash:   h = 0x12345678; for i, c in enumerate(name): h += (i*c) << ((i & 3) * 8)     (mod 2**32)
"""
import os, struct, subprocess, sys, tempfile, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzari

NAMES = """loading.pic waiting.pic mask1.raw soundtrack.mod menu_move.wav menu_select.wav menu_abort.wav
bomb_explode1.wav bomb_explode2.wav bomb_place.wav bonus.wav hero_respawn.wav girl_death.wav fatman_death.wav
skater_death.wav worker_death.wav key_collect.wav door_open.wav alien_death.wav""".split()
for theme, nlv in (('explode', 0), ('explodearena', 41), ('menu', 21)):
    pass


def candidate_names():
    c = list(NAMES)
    for t, kind in (('explode', 'sprites'), ('explodearena', 'arena'), ('menu', 'menu')):
        for ext in ('.nps', '.npp', '.nps.and.dat', '.nps.anh.dat', '.nps.ans.dat', '.pals.dat', '.brk.dat',
                    '.brk.pals.dat', '.lvltypes.dat', '.msk.dat'):
            c.append(t + ext)
        for n in range(0, 100):
            c.append('%s.lvl%03d.dat' % (t, n))
    return c


def name_hash(s):
    h = 0x12345678
    for i, ch in enumerate(s.encode('latin1')):
        h = (h + ((i * ch) << ((i & 3) * 8))) & 0xffffffff
    return h


def build_emulator_inputs(app_path, tmp):
    d = open(app_path, 'rb').read()
    u32 = lambda o: struct.unpack_from('<I', d, o)[0]
    code_size, text_size, code_off, import_off = u32(0x30), u32(0x60), u32(0x64), u32(0x6c)
    open(os.path.join(tmp, 'code.bin'), 'wb').write(d[code_off:code_off + code_size])
    o = import_off + 4
    slots = []
    for _ in range(u32(0x54)):
        nameoff, n = u32(o), u32(o + 4)
        p = import_off + nameoff
        name = d[p:d.index(b'\0', p)].decode('latin1').split('[')[0].replace('.DLL', '').lower()
        for k in range(n):
            slots.append((name, u32(o + 8 + 4 * k)))
        o += 8 + 4 * n
    with open(os.path.join(tmp, 'slots.txt'), 'w') as f:
        for k, (dll, od) in enumerate(slots):
            f.write('%d %s %d\n' % (text_size + 4 * k, dll, od))


def main():
    app, pak, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    d = open(pak, 'rb').read()
    n = struct.unpack_from('<I', d, 0x10)[0]
    by_hash = {name_hash(x): x for x in candidate_names()}
    tmp = tempfile.mkdtemp()
    emu = None
    for i in range(n):
        h, off, usz, c = [x ^ 0xAAAAAAAA for x in struct.unpack_from('<4I', d, 0x14 + 16 * i)]
        typ, csz = c >> 24, c & 0xffffff
        payload = bytes(b ^ 0xAA for b in d[off:off + csz])
        if typ == 0:
            data = payload
        elif typ == 2:
            data = lzari.decode(payload)
        elif typ in (3, 4):
            if emu is None:
                build_emulator_inputs(app, tmp)
                emu = os.path.join(tmp, 'armemu')
                subprocess.check_call(['gcc', '-O2', '-o', emu, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'armemu.c')])
            fin, fout = os.path.join(tmp, 'in.bin'), os.path.join(tmp, 'out.bin')
            open(fin, 'wb').write(payload)
            subprocess.run([emu, os.path.join(tmp, 'code.bin'), os.path.join(tmp, 'slots.txt')],
                           input='IMP %s %s %d %d\nQUIT\n' % (fin, fout, csz, usz), text=True, check=True, capture_output=True)
            data = open(fout, 'rb').read()
        else:
            print('entry %d: unknown type %d' % (i, typ)); continue
        name = by_hash.get(h, 'entry_%02d_%08x.bin' % (i, h))
        open(os.path.join(out, name), 'wb').write(data)
        print('%2d  %-28s type %d  %7d -> %7d' % (i, name, typ, csz, len(data)))


if __name__ == '__main__':
    main()

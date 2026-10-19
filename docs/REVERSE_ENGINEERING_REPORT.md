# Explode Arena (Symbian S60) – reverse-engineering report

## Short answer
The game is a **native ARM Symbian application**, so the original C++ source cannot be recovered
(no symbols, no class/member names, no comments in the binary). What I could do, and did:

| Result | Where |
|---|---|
| Unpacked the `.sis` installer (5 files) | `sis_contents/` |
| Full annotated ARM disassembly – 62,450 instructions, 1,707 functions, strings, vtables | `disasm/ExplodeArena.asm` |
| Function map (callers, callees, imports, referenced strings) and symbol list | `disasm/functions.csv`, `disasm/symbols.csv`, `disasm/vtables.txt` |
| Reverse-engineered the **data.pak** archive format and extracted **all 98 assets** | `assets/` |
| Working extractor + clean-room LZARI port | `tools/unpak.py`, `tools/lzari.py` |
| Raw code image ready for Ghidra/IDA (real decompiler → C-like pseudocode) | `disasm/code.bin` |

## 1. Package
* `ExplodeArena-DDJ.sis` – SIS (EPOC release 6), v1.0, app UID `0x10015b92`, requires platform UID `0x101f6f88` (Series 60).
* Installs to `!:\system\apps\ExplodeArena\`: `ExplodeArena.app` (337,300 B), `data.pak` (412,088 B), `ExplodeArena.rsc` (44 B), `ExplodeArena.aif` (2,414 B), `game.cfg` (36 B placeholder text; the game rewrites it).

## 2. Executable (`ExplodeArena.app`)
* E32 image, ARM, flagged as DLL (Series 60 apps are DLLs). Code 329,820 B at base `0x10000000`; stack 8 KB; heap 4 KB–1 MB; priority 350. One export (ordinal 1 @ `0x012204`) = the application factory (`NewApplication`).
* **ARM-state only**, BX-interworking style (ARMv4T). No Thumb.
* **307 imports from 22 DLLs** – EUSER 80, EIKCORE 59, CONE 45, ESOCK 26, AVKON 16, SDPDATABASE 15, BTMANCLIENT 11, ESTLIB 11, BLUETOOTH 6, EFSRV 6, ESTOR 6, APPARC/SDPAGENT/WS32 4 each, FBSCLI/SYSAGT 3, BITGDI/PROFILEENGINE 2, BAFL/HAL/MEDIACLIENTAUDIOSTREAM/PLPVARIANT 1.
* Disassembly quality: recursive descent from the entry, export, `bl` targets, tail calls, relocation-table pointers and vtables. 249,800 of 328,584 `.text` bytes are code (76 %); the rest is read-only data (strings/tables, 66.9 KB) and 307 import thunks. **All 4,155 `bl` instructions resolve to a function or import thunk**, a good consistency check. 65 vtable-like pointer tables (733 slots) ≈ C++ classes.
* ~48 functions are named (`Pak_*`, `LZARI_*`, `Imp_*`, `CGlobals::setGameParams`, `Theme_Load*`, …). Names are only given where behaviour was verified; everything else is `sub_XXXXXX`. A few import ordinals were named from behaviour, not from a `.def` file (`sprintf`, `memset`, `strcpy`, `strncmp`, `operator_new/delete`, `__divsi3/__udivsi3`).

### What the code tells us about the game
* A **Bomberman-style arena game** (assets: bombs, keys/doors, bonuses; enemies `ALIEN1`–`ALIEN7`; characters girl / fatman / skater / worker / hero). 41 arena levels + 21 "menu" levels (the menu is itself a level of the engine).
* Modes seen in strings: arcade, up-to-4 players (`Player1`–`Player4`), teams (`Team 1`/`Team 2`, "playoff", "match no."), time limit parameter 0–3, a demo mode.
* **Bluetooth multiplayer**: 26 functions (cluster at `0x38bc–0x4bec`, plus `0x29aa8–0x2a3a4`) using BTMANCLIENT/ESOCK/SDP; UTF-16 strings `RFCOMM`, `BTLinkManager`.
* Custom software engine: very little system API is used for the game itself. Music is an in-engine **tracker (.mod) player**; one function (`0x22e24`) feeds the platform audio stream.
* Debug text is Polish (e.g. `CGlobals::setGameParams(): parametr TIME LIMIT poza zakresem od 0 do 3`); branding strings `IDREAMS` and `S.FORCE`.

## 3. `data.pak` format (reverse-engineered)
```
0x00  16 bytes  zero
0x10  u32       entry count (clear)               = 98
0x14  count*16  index – every byte XOR 0xAA; fields (LE):
                  u32 hash   u32 offset   u32 usize   u32 (type<<24 | csize)
...   payloads – every byte XOR 0xAA
```
* Name hash (`Pak_HashName`, `0x00af58`): `h=0x12345678; for i,c in enumerate(name): h += (i*c) << ((i&3)*8)`; the index is searched linearly. I recovered all 98 names by hashing strings/templates from the binary (`%s.nps`, `%s.lvl%03d.dat`, …).
* Entry types (`Pak_ReadByIndex`, `0x00adb4`): 0 raw · 1 no-op · **2 = LZARI** (68 entries) · **3/4 = "IMP" container** (30 entries).
* **LZARI** = Okumura's LZSS + adaptive arithmetic coder (N=4096, F=60, THRESHOLD=2, 314 symbols). Payload = `u32 usize` + stream. Ported to clean Python (`tools/lzari.py`) and **verified byte-for-byte against the game's own routine** on all 68 entries. (The game's decoder never flushes the final byte of odd-length output; `door_open.wav` – 863 B – therefore ends in a different last sample byte than the raw game memory dump. I kept the algorithmically correct byte.)
* **IMP** = custom block container: magic `IMP\n`, 42-byte header with CRC-32 (poly `0xEDB88320`), block tags `IMPLH`/`IMPDE`, LSB-first bitstream with a 4-bit block type, final-block flag and two 20-bit sizes, Huffman tables. I did **not** port this to Python; instead `tools/unpak.py` runs the game's own `Imp_Unpack` (`0x011bc8`) in a small ARM interpreter (`tools/armemu.c`, ~300 lines). Check: the 211,776-byte `soundtrack.mod` it produces matches the size computed from the module's own header exactly.

## 4. Extracted assets (`assets/`)
| Group | Files |
|---|---|
| Screens | `loading.pic`, `waiting.pic` (1,796 B each, RGB565-looking pixel data; not decoded), `mask1.raw` (43,008 B) |
| Music | `soundtrack.mod` – 6-channel ProTracker module "explode arena", 89 patterns; sample text credits *falcon/cloud#9^ap*, dated 27.06.2004 (tracks: ingame 1–3, menu, level complete, game over) |
| SFX | 15 × WAV, 8-bit mono PCM @ 8/11.025/16 kHz (menu_*, bomb_*, bonus, hero_respawn, *_death, key_collect, door_open) |
| Theme `explode` (sprites) | `explode.nps`, `explode.nps.and.dat` (573,856 B), `.anh.dat`, `.ans.dat` |
| Theme `explodearena` | `.pals.dat` (RGB565 palettes), `.lvltypes.dat`, `.brk.dat`, `.brk.pals.dat`, `.npp`, `.lvl000–.lvl040.dat` |
| Theme `menu` | `.nps`, `.pals.dat`, `.brk.dat`, `.brk.pals.dat`, `.npp`, `.lvl000–.lvl020.dat`, `.nps.and/.anh/.ans.dat` |

`.nps` files are a sequence of sections: `u32 section_size, u32 count, u32 offsets[count], data…` (seen in `Theme_LoadA`, `0x0208a0`). Sprite, level and animation formats are otherwise **not yet decoded**.

## 5. Going further
* **Real decompiler:** in Ghidra choose *File → Import → Raw Binary* on `disasm/code.bin`, language `ARM:LE:32:v4t`, base address `0x10000000`; then import names from `disasm/symbols.csv`. I could not run Ghidra here (no network or installed tools), so the pseudocode step is yours.
* Natural next steps: decode `.nps/.brk` sprite formats and the `.lvl` layout, port the IMP container to Python, document the Bluetooth protocol.
* Contents belong to the original authors; this is intended for preservation and study.

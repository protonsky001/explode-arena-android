# Porting notes

## Approach
The Symbian build of Explode Arena is a native ARM application (`ExplodeArena.app`). Rather than rewriting the
game, this port **executes the original machine code** and replaces only the operating-system layer.

* `ea_host.c` contains an ARMv4 interpreter (ARM state only; the binary has no Thumb code), the guest memory map
  (code image at 0x10000000, heap, stack) and "high level emulation" (HLE) of the Symbian services the engine uses.
* The engine is entered at the same places the original UI glue used: construct the globals object, initialise its
  pak/file object, create the frame bitmap, then call the per-frame function (`0x00dd2c`) and latch key state.
* Key events go through the original key handler (`0x012f04`), so the original scan-code table is used unchanged.
* The game's 16.16 fixed-point maths, tile blitters and sound mixer are all original code.

## Symbian services provided by the host
| Area | What the host does |
|---|---|
| Heap | zero-filled allocator behind `operator new/delete`, `User::Alloc/Free` and the engine's pool allocator |
| TLS | `Dll::Tls/SetTls` hold the globals pointer |
| Files | `data.pak` index is served from `pakhead.bin`; entry payloads come from the pre-decoded `NN.bin` assets; `game.cfg` is persisted by Java |
| Bitmaps | fake `CFbsBitmap` (Create/DataAddress) whose pixels live in guest memory; the visible 176x208 area is copied out each frame |
| Audio | fake `CMdaAudioOutputStream` (vtable traps for Open/SetAudioPropertiesL/WriteL/Stop...). The engine's own mixer runs in the guest and the PCM (16 kHz mono) is ring-buffered for `AudioTrack` |
| C library / maths | printf family, string and memory functions, `qsort` (with guest callback), soft-float helpers, integer division helpers |
| Device | system-agent queries report "not silent"; the phone ID formatter uses a dummy IMEI |
| Bluetooth | not provided; unknown imports return 0 and the engine is soft-reset if it faults |

## Native shortcuts (verified identical)
Five 8x8 tile blitters (`0x028468`, `0x028784`, `0x028934`, `0x028c80`, `0x028d74`) and the fixed-point multiply
(`0x003534`) are replaced by native code. Build with `-DEA_NOBLIT` for the pure-emulation reference; a scripted
session of ~2,500 frames (menus, hero select, gameplay) produces **bit-identical frames** with and without the shortcuts,
and the core is clean under AddressSanitizer/UBSan.

## Input
The original key handler maps Symbian scan codes to logical keys. In the game's default keyboard layout the bomb is on
keys 4/5/6 and the detonator on 7/8/9 (menus select with the joystick press), so the on-screen BOMB button sends joystick-press +
key 5 and DET sends key 8. `KeyFeeder` keeps every press visible to the engine for at least one frame, because the game polls key
state once per frame.

## Speed
Typical frames cost 0.2-4 M guest instructions (the original needed ~30 MIPS at 32 fps; the shortcuts cut this by
about 60 %). Loading a level is one heavier frame (~58 M instructions). Boot is ~59 M instructions.

## Known gaps / ideas
* Bluetooth multiplayer (would need a transport shim for the original ESOCK/SDP calls).
* Landscape layout, on-screen number keys, save-state support.
* Replace more hot guest functions with native code if low-end devices need it.

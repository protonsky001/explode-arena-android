"""LZARI decoder (Haruhiko Okumura's LZSS + adaptive arithmetic coding).

This is a clean-room Python port of the algorithm that ExplodeArena.app implements in
sub_017eac / sub_017a40 / sub_017c18 / sub_017d68 (pak entry type 2).  Constants match the
binary: N=4096, F=60, THRESHOLD=2, N_CHAR=314, M=15, position model 10000/(i+200).
"""
import struct

N = 4096; F = 60; THRESHOLD = 2
N_CHAR = 256 - THRESHOLD + F          # 314
M = 15
Q1 = 1 << M; Q2 = 2 * Q1; Q3 = 3 * Q1; Q4 = 4 * Q1
MAX_CUM = Q1 - 1


def decode(src: bytes) -> bytes:
    textsize = struct.unpack_from('<I', src, 0)[0]
    pos = 4
    bitbuf = 0; mask = 0

    def getbit():
        nonlocal bitbuf, mask, pos
        mask >>= 1
        if mask == 0:
            bitbuf = src[pos] if pos < len(src) else 0
            pos += 1
            mask = 128
        return 1 if (bitbuf & mask) else 0

    # --- model ---
    sym_freq = [0] * (N_CHAR + 1)
    sym_cum = [0] * (N_CHAR + 1)
    char_to_sym = [0] * N_CHAR
    sym_to_char = [0] * (N_CHAR + 1)
    position_cum = [0] * (N + 1)
    sym_cum[N_CHAR] = 0
    for sym in range(N_CHAR, 0, -1):
        ch = sym - 1
        char_to_sym[ch] = sym; sym_to_char[sym] = ch
        sym_freq[sym] = 1
        sym_cum[sym - 1] = sym_cum[sym] + sym_freq[sym]
    sym_freq[0] = 0
    position_cum[N] = 0
    for i in range(N, 0, -1):
        position_cum[i - 1] = position_cum[i] + 10000 // (i + 200)

    def update_model(sym):
        if sym_cum[0] >= MAX_CUM:
            c = 0
            for i in range(N_CHAR, 0, -1):
                sym_cum[i] = c
                sym_freq[i] = (sym_freq[i] + 1) >> 1
                c += sym_freq[i]
            sym_cum[0] = c
        i = sym
        while sym_freq[i] == sym_freq[i - 1]:
            i -= 1
        if i < sym:
            ch_i = sym_to_char[i]; ch_sym = sym_to_char[sym]
            sym_to_char[i] = ch_sym; sym_to_char[sym] = ch_i
            char_to_sym[ch_i] = sym; char_to_sym[ch_sym] = i
        sym_freq[i] += 1
        i -= 1
        while i >= 0:
            sym_cum[i] += 1
            i -= 1

    low = 0; high = Q4; value = 0
    for _ in range(M + 2):
        value = 2 * value + getbit()

    def normalize():
        nonlocal low, high, value
        while True:
            if low >= Q2:
                value -= Q2; low -= Q2; high -= Q2
            elif low >= Q1 and high <= Q3:
                value -= Q1; low -= Q1; high -= Q1
            elif high > Q2:
                break
            low += low; high += high
            value = 2 * value + getbit()

    def decode_char():
        nonlocal low, high
        rng = high - low
        x = ((value - low + 1) * sym_cum[0] - 1) // rng
        i, j = 1, N_CHAR
        while i < j:
            k = (i + j) // 2
            if sym_cum[k] > x: i = k + 1
            else: j = k
        sym = i
        high = low + (rng * sym_cum[sym - 1]) // sym_cum[0]
        low += (rng * sym_cum[sym]) // sym_cum[0]
        normalize()
        ch = sym_to_char[sym]
        update_model(sym)
        return ch

    def decode_position():
        nonlocal low, high
        rng = high - low
        x = ((value - low + 1) * position_cum[0] - 1) // rng
        i, j = 1, N
        while i < j:
            k = (i + j) // 2
            if position_cum[k] > x: i = k + 1
            else: j = k
        position = i - 1
        high = low + (rng * position_cum[position]) // position_cum[0]
        low += (rng * position_cum[position + 1]) // position_cum[0]
        normalize()
        return position

    text = bytearray(b' ' * (N - F)) + bytearray(F)
    out = bytearray()
    r = N - F
    while len(out) < textsize:
        c = decode_char()
        if c < 256:
            out.append(c); text[r] = c; r = (r + 1) & (N - 1)
        else:
            i = (r - decode_position() - 1) & (N - 1)
            j = c - 255 + THRESHOLD
            for k in range(j):
                ch = text[(i + k) & (N - 1)]
                out.append(ch); text[r] = ch; r = (r + 1) & (N - 1)
    return bytes(out[:textsize])

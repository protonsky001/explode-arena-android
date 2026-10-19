"""Small ARMv4T/v5 + Thumb disassembler (no external deps)."""
import struct

CC = ['eq','ne','cs','cc','mi','pl','vs','vc','hi','ls','ge','lt','gt','le','','nv']
RN = ['r0','r1','r2','r3','r4','r5','r6','r7','r8','r9','sl','fp','ip','sp','lr','pc']
SH = ['lsl','lsr','asr','ror']
DP = ['and','eor','sub','rsb','add','adc','sbc','rsc','tst','teq','cmp','cmn','orr','mov','bic','mvn']


def ror32(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xffffffff


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def hx(v):
    return '#%d' % v if -10 < v < 10 else ('#0x%x' % v if v >= 0 else '#-0x%x' % -v)


def _shreg(i):
    rm = i & 15
    t = (i >> 5) & 3
    if i & 0x10:
        return '%s, %s %s' % (RN[rm], SH[t], RN[(i >> 8) & 15])
    imm = (i >> 7) & 31
    if imm == 0:
        if t == 0:
            return RN[rm]
        if t == 3:
            return '%s, rrx' % RN[rm]
        imm = 32
    return '%s, %s #%d' % (RN[rm], SH[t], imm)


def _reglist(m):
    out = []
    r = 0
    while r < 16:
        if m & (1 << r):
            s = r
            while r + 1 < 16 and m & (1 << (r + 1)):
                r += 1
            out.append(RN[s] if s == r else '%s-%s' % (RN[s], RN[r]))
        r += 1
    return '{' + ', '.join(out) + '}'


class Ins:
    __slots__ = ('addr', 'raw', 'size', 'text', 'kind', 'target', 'lit', 'thumb')

    def __init__(self, addr, raw, size, text, kind='', target=None, lit=None, thumb=False):
        self.addr = addr; self.raw = raw; self.size = size; self.text = text
        self.kind = kind      # '', 'b','bl','bcond','ret','indirect','swi', 'ldrpc'
        self.target = target  # branch target address
        self.lit = lit        # literal address for pc-relative loads
        self.thumb = thumb


def dis_arm(i, addr):
    """Decode one ARM word located at `addr`. Returns Ins."""
    cond = i >> 28
    c = CC[cond]
    top = (i >> 25) & 7

    def mk(text, kind='', target=None, lit=None):
        return Ins(addr, i, 4, text, kind, target, lit)

    # ---- unconditional space (v5) ----
    if cond == 15:
        if top == 5:
            off = sx(i & 0xffffff, 24) * 4 + ((i >> 24) & 1) * 2
            t = (addr + 8 + off) & 0xffffffff
            return mk('blx 0x%x' % t, 'bl', t)
        return mk('.word 0x%08x' % i)

    if top == 0:
        # multiply / swap / halfword / misc
        if (i & 0x0ffffff0) == 0x012fff10:
            kind = 'ret' if (i & 15) == 14 and cond == 14 else ('indirect' if cond == 14 else 'bcond_ind')
            return mk('bx%s %s' % (c, RN[i & 15]), kind)
        if (i & 0x0ffffff0) == 0x012fff30:
            return mk('blx%s %s' % (c, RN[i & 15]), 'blx')
        if (i & 0x0fff0ff0) == 0x016f0f10:
            return mk('clz%s %s, %s' % (c, RN[(i >> 12) & 15], RN[i & 15]))
        if (i & 0xfff000f0) == 0xe1200070:
            return mk('bkpt #0x%x' % (((i >> 4) & 0xfff0) | (i & 15)))
        if (i & 0x0fbf0fff) == 0x010f0000:
            return mk('mrs%s %s, %s' % (c, RN[(i >> 12) & 15], 'spsr' if i & 0x400000 else 'cpsr'))
        if (i & 0x0fb0fff0) == 0x0120f000:
            f = ''.join(ch for b, ch in ((19, 'f'), (18, 's'), (17, 'x'), (16, 'c')) if i & (1 << b))
            return mk('msr%s %s_%s, %s' % (c, 'spsr' if i & 0x400000 else 'cpsr', f, RN[i & 15]))
        if (i & 0x0fc000f0) == 0x00000090:
            s = 's' if i & 0x100000 else ''
            rd, rn, rs, rm = (i >> 16) & 15, (i >> 12) & 15, (i >> 8) & 15, i & 15
            if i & 0x200000:
                return mk('mla%s%s %s, %s, %s, %s' % (s, c, RN[rd], RN[rm], RN[rs], RN[rn]))
            return mk('mul%s%s %s, %s, %s' % (s, c, RN[rd], RN[rm], RN[rs]))
        if (i & 0x0f8000f0) == 0x00800090:
            s = 's' if i & 0x100000 else ''
            nm = ('s' if i & 0x400000 else 'u') + ('mlal' if i & 0x200000 else 'mull')
            return mk('%s%s%s %s, %s, %s, %s' % (nm, s, c, RN[(i >> 12) & 15], RN[(i >> 16) & 15], RN[i & 15], RN[(i >> 8) & 15]))
        if (i & 0x0fb000f0) == 0x01000090:
            b = 'b' if i & 0x400000 else ''
            return mk('swp%s%s %s, %s, [%s]' % (b, c, RN[(i >> 12) & 15], RN[i & 15], RN[(i >> 16) & 15]))
        if (i & 0x90) == 0x90 and (i & 0x60):
            # halfword / signed transfer
            L = i & 0x100000
            sh = (i >> 5) & 3
            P, U, W = i & 0x1000000, i & 0x800000, i & 0x200000
            rn, rd = (i >> 16) & 15, (i >> 12) & 15
            if L:
                nm = {1: 'ldrh', 2: 'ldrsb', 3: 'ldrsh'}[sh]
            else:
                nm = {1: 'strh', 2: 'ldrd', 3: 'strd'}[sh]
            if i & 0x400000:
                off = ((i >> 4) & 0xf0) | (i & 15)
                sign = '' if U else '-'
                offs = '#%s0x%x' % (sign, off) if off else None
                lit = None
                if rn == 15:
                    lit = (addr + 8 + (off if U else -off)) & 0xffffffff
            else:
                offs = ('' if U else '-') + RN[i & 15]
                lit = None
            if P:
                if offs:
                    t = '[%s, %s]%s' % (RN[rn], offs, '!' if W else '')
                else:
                    t = '[%s]%s' % (RN[rn], '!' if W else '')
            else:
                t = '[%s], %s' % (RN[rn], offs if offs else '#0')
            return mk('%s%s %s, %s' % (nm, c, RN[rd], t), lit=lit)
        # fallthrough to data processing

    if top in (0, 1):
        op = (i >> 21) & 15
        S = i & 0x100000
        # misc space in tst/teq/cmp/cmn with S=0
        if 8 <= op <= 11 and not S:
            if top == 1 and (i & 0x0fb0f000) == 0x0320f000:
                v = ror32(i & 0xff, ((i >> 8) & 15) * 2)
                f = ''.join(ch for b, ch in ((19, 'f'), (18, 's'), (17, 'x'), (16, 'c')) if i & (1 << b))
                return mk('msr%s %s_%s, #0x%x' % (c, 'spsr' if i & 0x400000 else 'cpsr', f, v))
            return mk('.word 0x%08x' % i)
        rn, rd = (i >> 16) & 15, (i >> 12) & 15
        if top == 1:
            v = ror32(i & 0xff, ((i >> 8) & 15) * 2)
            o2 = hx(v)
        else:
            o2 = _shreg(i)
        nm = DP[op]
        s = 's' if (S and not 8 <= op <= 11) else ''
        if 8 <= op <= 11:
            text = '%s%s %s, %s' % (nm, c, RN[rn], o2)
        elif op in (13, 15):
            text = '%s%s%s %s, %s' % (nm, s, c, RN[rd], o2)
        else:
            text = '%s%s%s %s, %s, %s' % (nm, s, c, RN[rd], RN[rn], o2)
        kind = ''
        tgt = None
        lit = None
        if rd == 15 and not 8 <= op <= 11:
            if op == 13 and top == 0 and (i & 15) == 14 and not (i & 0xff0):
                kind = 'ret' if cond == 14 else 'bcond_ind'
            elif op == 4 and rn == 15 and top == 0:
                kind = 'switch'
            else:
                kind = 'indirect' if cond == 14 else 'bcond_ind'
        # adr form: add/sub rd, pc, #imm
        if rn == 15 and top == 1 and op in (2, 4) and not S:
            v = ror32(i & 0xff, ((i >> 8) & 15) * 2)
            lit = (addr + 8 + (v if op == 4 else -v)) & 0xffffffff
            kind = kind or 'adr'
        return mk(text, kind, tgt, lit)

    if top in (2, 3):
        if top == 3 and (i & 0x10):
            return mk('.word 0x%08x' % i)
        I = top == 3
        P, U, B, W, L = i & 0x1000000, i & 0x800000, i & 0x400000, i & 0x200000, i & 0x100000
        rn, rd = (i >> 16) & 15, (i >> 12) & 15
        nm = ('ldr' if L else 'str') + ('b' if B else '') + ('t' if (not P and W) else '')
        lit = None
        if not I:
            off = i & 0xfff
            offs = ('#%s0x%x' % ('' if U else '-', off)) if off else None
            if rn == 15 and P:
                lit = (addr + 8 + (off if U else -off)) & 0xffffffff
        else:
            offs = ('' if U else '-') + _shreg(i)
        if P:
            t = '[%s, %s]%s' % (RN[rn], offs, '!' if W else '') if offs else '[%s]%s' % (RN[rn], '!' if W else '')
        else:
            t = '[%s], %s' % (RN[rn], offs if offs else '#0')
        kind = ''
        if L and rd == 15:
            kind = 'ldrpc'
        return mk('%s%s %s, %s' % (nm, c, RN[rd], t), kind, None, lit)

    if top == 4:
        P, U, S, W, L = i & 0x1000000, i & 0x800000, i & 0x400000, i & 0x200000, i & 0x100000
        rn = (i >> 16) & 15
        m = i & 0xffff
        if rn == 13:
            mode = ('ldmfd' if U and not P else 'ldmed' if U and P else 'ldmfa' if not U and not P else 'ldmea') if L else \
                   ('stmea' if U and not P else 'stmfa' if U and P else 'stmed' if not U and not P else 'stmfd')
            mode = mode[:3] + mode[3:]
        else:
            mode = ('ldm' if L else 'stm') + ('i' if U else 'd') + ('b' if P else 'a')
        base = mode[:3] + c + mode[3:]
        text = '%s %s%s, %s%s' % (base, RN[rn], '!' if W else '', _reglist(m), '^' if S else '')
        kind = 'ret' if (L and (m & 0x8000) and cond == 14 and rn == 13 and W) else ('ldrpc' if (L and (m & 0x8000)) else '')
        return mk(text, kind)

    if top == 5:
        off = sx(i & 0xffffff, 24) * 4
        t = (addr + 8 + off) & 0xffffffff
        if i & 0x1000000:
            return mk('bl%s 0x%x' % (c, t), 'bl', t)
        return mk('b%s 0x%x' % (c, t), 'b' if cond == 14 else 'bcond', t)

    if top == 6:
        P, U, N, W, L = i & 0x1000000, i & 0x800000, i & 0x400000, i & 0x200000, i & 0x100000
        off = (i & 0xff) * 4
        t = '[%s, #%s0x%x]%s' % (RN[(i >> 16) & 15], '' if U else '-', off, '!' if W else '') if P else '[%s], #%s0x%x' % (RN[(i >> 16) & 15], '' if U else '-', off)
        return mk('%s%s%s p%d, c%d, %s' % ('ldc' if L else 'stc', 'l' if N else '', c, (i >> 8) & 15, (i >> 12) & 15, t))

    # top == 7
    if i & 0x1000000:
        return mk('swi%s #0x%x' % (c, i & 0xffffff), 'swi')
    if i & 0x10:
        nm = 'mrc' if i & 0x100000 else 'mcr'
        return mk('%s%s p%d, %d, %s, c%d, c%d, %d' % (nm, c, (i >> 8) & 15, (i >> 21) & 7, RN[(i >> 12) & 15], (i >> 16) & 15, i & 15, (i >> 5) & 7))
    return mk('cdp%s p%d, %d, c%d, c%d, c%d, %d' % (c, (i >> 8) & 15, (i >> 20) & 15, (i >> 12) & 15, (i >> 16) & 15, i & 15, (i >> 5) & 7))


# ------------------------------- Thumb --------------------------------
def dis_thumb(h, addr, h2=None):
    """Decode one Thumb halfword at addr (h2 = next halfword for BL pair). Returns Ins (size 2 or 4)."""
    def mk(text, kind='', target=None, lit=None, size=2, raw=None):
        return Ins(addr, h if raw is None else raw, size, text, kind, target, lit, True)

    top = h >> 13
    if top == 0:
        op = (h >> 11) & 3
        if op == 3:
            I = h & 0x400
            sub = h & 0x200
            rn_or_imm = (h >> 6) & 7
            rs, rd = (h >> 3) & 7, h & 7
            nm = 'sub' if sub else 'add'
            if I:
                return mk('%ss r%d, r%d, #%d' % (nm, rd, rs, rn_or_imm))
            return mk('%ss r%d, r%d, r%d' % (nm, rd, rs, rn_or_imm))
        imm = (h >> 6) & 31
        if imm == 0 and op != 0:
            imm = 32
        return mk('%ss r%d, r%d, #%d' % (SH[op], h & 7, (h >> 3) & 7, imm))
    if top == 1:
        op = (h >> 11) & 3
        rd = (h >> 8) & 7
        imm = h & 0xff
        return mk('%s%s r%d, #%d' % (['mov', 'cmp', 'add', 'sub'][op], '' if op == 1 else 's', rd, imm))
    if top == 2:
        if (h >> 10) & 7 == 0:
            op = (h >> 6) & 15
            nm = ['and', 'eor', 'lsl', 'lsr', 'asr', 'adc', 'sbc', 'ror', 'tst', 'neg', 'cmp', 'cmn', 'orr', 'mul', 'bic', 'mvn'][op]
            s = '' if op in (8, 10, 11) else 's'
            return mk('%s%s r%d, r%d' % (nm, s, h & 7, (h >> 3) & 7))
        if (h >> 10) & 7 == 1:
            op = (h >> 8) & 3
            rd = (h & 7) | ((h >> 4) & 8)
            rs = (h >> 3) & 15
            if op == 3:
                if h & 0x80:
                    return mk('blx %s' % RN[rs], 'blx')
                return mk('bx %s' % RN[rs], 'ret' if rs == 14 else 'indirect')
            nm = ['add', 'cmp', 'mov'][op]
            kind = 'indirect' if (rd == 15 and op != 1) else ''
            return mk('%s %s, %s' % (nm, RN[rd], RN[rs]), kind)
        if (h >> 11) & 3 == 1:
            off = (h & 0xff) * 4
            lit = ((addr + 4) & ~3) + off
            return mk('ldr r%d, [pc, #0x%x]' % ((h >> 8) & 7, off), lit=lit)
        # load/store reg offset
        rm, rn, rd = (h >> 6) & 7, (h >> 3) & 7, h & 7
        if h & 0x200:
            nm = ['strh', 'ldrsb', 'ldrh', 'ldrsh'][(h >> 10) & 3]
        else:
            nm = ['str', 'strb', 'ldr', 'ldrb'][(h >> 10) & 3]
        return mk('%s r%d, [r%d, r%d]' % (nm, rd, rn, rm))
    if top == 3:
        B = h & 0x1000
        L = h & 0x800
        off = (h >> 6) & 31
        rn, rd = (h >> 3) & 7, h & 7
        nm = ('ldr' if L else 'str') + ('b' if B else '')
        off = off if B else off * 4
        return mk('%s r%d, [r%d%s]' % (nm, rd, rn, ', #0x%x' % off if off else ''))
    if top == 4:
        if not h & 0x1000:
            off = ((h >> 6) & 31) * 2
            nm = 'ldrh' if h & 0x800 else 'strh'
            return mk('%s r%d, [r%d%s]' % (nm, h & 7, (h >> 3) & 7, ', #0x%x' % off if off else ''))
        off = (h & 0xff) * 4
        nm = 'ldr' if h & 0x800 else 'str'
        return mk('%s r%d, [sp%s]' % (nm, (h >> 8) & 7, ', #0x%x' % off if off else ''))
    if top == 5:
        if not h & 0x1000:
            off = (h & 0xff) * 4
            if h & 0x800:
                return mk('add r%d, sp, #0x%x' % ((h >> 8) & 7, off))
            return mk('add r%d, pc, #0x%x' % ((h >> 8) & 7, off), lit=((addr + 4) & ~3) + off)
        if (h >> 8) & 15 == 0:
            off = (h & 0x7f) * 4
            return mk('%s sp, sp, #0x%x' % ('sub' if h & 0x80 else 'add', off))
        if (h >> 9) & 3 == 2:
            rl = h & 0xff
            if h & 0x800:
                if h & 0x100:
                    rl |= 0x8000
                return mk('pop %s' % _reglist(rl), 'ret' if h & 0x100 else '')
            if h & 0x100:
                rl |= 0x4000
            return mk('push %s' % _reglist(rl))
        return mk('.hword 0x%04x' % h)
    if top == 6:
        if not h & 0x1000:
            rl = h & 0xff
            return mk('%s r%d!, %s' % ('ldmia' if h & 0x800 else 'stmia', (h >> 8) & 7, _reglist(rl)))
        cond = (h >> 8) & 15
        if cond == 15:
            return mk('swi #0x%x' % (h & 0xff), 'swi')
        if cond == 14:
            return mk('.hword 0x%04x' % h)
        t = (addr + 4 + sx(h & 0xff, 8) * 2) & 0xffffffff
        return mk('b%s 0x%x' % (CC[cond], t), 'bcond', t)
    if top == 7:
        sub = (h >> 11) & 3
        if sub == 0:
            t = (addr + 4 + sx(h & 0x7ff, 11) * 2) & 0xffffffff
            return mk('b 0x%x' % t, 'b', t)
        if sub == 2:
            if h2 is not None and (h2 >> 11) in (0x1f, 0x1d):
                off = (sx(h & 0x7ff, 11) << 12) + ((h2 & 0x7ff) << 1)
                t = (addr + 4 + off) & 0xffffffff
                if (h2 >> 11) == 0x1d:
                    t &= ~3
                    return mk('blx 0x%x' % t, 'bl', t, size=4, raw=(h << 16) | h2)
                return mk('bl 0x%x' % t, 'bl', t, size=4, raw=(h << 16) | h2)
            return mk('.hword 0x%04x' % h)
        return mk('.hword 0x%04x' % h)
    return mk('.hword 0x%04x' % h)

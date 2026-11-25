#!/usr/bin/env python3
"""Recompilador estático ARMv5TE (modo ARM) -> C.

Lê extracted/ (gerado por tools/extract.py) e analysis/functions_list.txt
(gerado por tools/analyze.py) e escreve C em build/gen/.

Cada função da ROM vira  `void f_<modulo>_<endereco>(Cpu *c)`.
- Saltos dentro da função viram `goto`.
- BL para função conhecida vira chamada direta em C.
- Saltos/chamadas indiretas (bx rX, blx rX, ldr pc, ...) passam por `rt_call()`,
  que escolhe a função pelo endereço e pelo overlay carregado.
- Instruções não suportadas viram `rt_unimpl()`, que para o programa com o endereço.

Uso: python3 -I recomp/armrecomp.py [extracted] [analysis/functions_list.txt] [build/gen]
"""
import collections, json, os, struct, sys

EXT = sys.argv[1] if len(sys.argv) > 1 else "extracted"
FLIST = sys.argv[2] if len(sys.argv) > 2 else "analysis/functions_list.txt"
OUT = sys.argv[3] if len(sys.argv) > 3 else "build/gen"
FUNCS_PER_FILE = 400

COND = ["c->Z", "!c->Z", "c->C", "!c->C", "c->N", "!c->N", "c->V", "!c->V",
        "(c->C && !c->Z)", "(!c->C || c->Z)", "(c->N == c->V)", "(c->N != c->V)",
        "(!c->Z && c->N == c->V)", "(c->Z || c->N != c->V)", None, None]


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Module:
    def __init__(self, name, ram, data, kind):
        self.name, self.ram, self.data, self.kind = name, ram, data, kind
        self.end = ram + len(data)

    def has(self, a):
        return self.ram <= a < self.end

    def word(self, a):
        return struct.unpack_from("<I", self.data, a - self.ram)[0]


def load():
    lay = json.load(open(f"{EXT}/layout.json"))
    mods = {}
    for s in lay["arm9"]["sections"]:
        name = "arm9" if s["file"] == "arm9.bin" else ("itcm" if s["ram"] < 0x02000000 else "dtcm")
        mods[name] = Module(name, s["ram"], open(f"{EXT}/{s['file']}", "rb").read(), "static")
    for o in lay["overlays"]:
        n = f"ov{o['id']:03d}"
        mods[n] = Module(n, o["ram"], open(f"{EXT}/{o['file']}", "rb").read(), "overlay")
    funcs = collections.defaultdict(dict)
    extra = os.path.join(os.path.dirname(FLIST), "extra_functions.txt")
    for path in [FLIST] + ([extra] if os.path.exists(extra) else []):
        for line in open(path):
            if not line.strip() or line.startswith("#"):
                continue
            m, a, t = line.split()
            funcs[m][int(a, 16)] = (t == "thumb")
    return lay, mods, funcs


class FuncGen:
    """Traduz uma função ARM."""

    def __init__(self, mod, addr, ctx):
        self.mod, self.addr, self.ctx = mod, addr, ctx
        self.lines = []
        self.labels = set()
        self.insns = {}
        self.unimpl = 0

    # ---------- resolução de alvos ----------
    def fname(self, mod, a):
        return f"f_{mod.name}_{a:08x}"

    def resolve(self, target, thumb):
        """Retorna nome C da função se o alvo é estático e conhecido."""
        return self.ctx.resolve(self.mod, target, thumb)

    def emit_call(self, target, thumb, lr):
        name = self.resolve(target, thumb)
        if name:
            self.ctx.called.add(name)
            return f"CALL(c, 0x{lr:08x}u, {name});"
        return f"c->r[14] = 0x{lr:08x}u; rt_call(c, 0x{(target | (1 if thumb else 0)):08x}u);"

    # ---------- descoberta do corpo ----------
    def discover(self):
        mod, ctx = self.mod, self.ctx
        work = [self.addr]
        seen = set()
        while work:
            a = work.pop()
            while mod.has(a) and a not in seen and a + 4 <= mod.end:
                if a != self.addr and a in ctx.funcs_of(mod):
                    break  # entrou no início de outra função: vira rt_fallthrough
                seen.add(a)
                w = mod.word(a)
                self.insns[a] = w
                cond = w >> 28
                op = (w >> 25) & 7
                nxt = a + 4
                if cond == 0xF:
                    a = nxt
                    continue
                if op == 5:  # B / BL
                    tgt = (a + 8 + sx(w & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF
                    if not (w >> 24) & 1:
                        if mod.has(tgt) and not (tgt != self.addr and tgt in ctx.funcs_of(mod)):
                            self.labels.add(tgt)
                            work.append(tgt)
                        if cond == 0xE:
                            break
                elif self.is_terminator(w, a):
                    if cond == 0xE:
                        # tabela de salto
                        if self.jump_table(w):
                            for p in self.jump_table_entries(mod, a):
                                self.labels.add(p)
                                work.append(p)
                        break
                    if self.jump_table(w):
                        for p in self.jump_table_entries(mod, a):
                            self.labels.add(p)
                            work.append(p)
                a = nxt
        self.order = sorted(self.insns)

    def jump_table_entries(self, mod, a):
        """Entradas de 'add pc, pc, rX, lsl #2': o tamanho vem do 'cmp rX, #n' logo antes (n+1 entradas).
        As entradas podem ser 'b', 'pop {..., pc}' ou qualquer outra instrução."""
        w = mod.word(a)
        rm = w & 0xF
        n = 0  # maior 'cmp rm, #imm' logo antes (pode haver 'cmp #0' + 'cmp #N' para faixa com sinal)
        for back in range(1, 7):
            q = a - 4 * back
            if not mod.has(q):
                break
            v = mod.word(q)
            if (v & 0x0FF00000) == 0x03500000 and ((v >> 16) & 0xF) == rm:
                rot = ((v >> 8) & 0xF) * 2
                imm = ((v & 0xFF) >> rot | (v & 0xFF) << (32 - rot)) & 0xFFFFFFFF
                if imm < 512:
                    n = max(n, imm + 1)
        out, p = [], a + 8
        while mod.has(p) and ((mod.word(p) >> 24) == 0xEA or len(out) < n):
            out.append(p)
            p += 4
        return out

    @staticmethod
    def jump_table(w):
        # addCC pc, pc, rX, lsl #2
        return (w & 0x0FFFFFF0) == 0x008FF100

    def link_call(self, w, a):
        """'mov lr, pc' ou 'add lr, pc, #0' logo antes de 'bx rX' / 'mov pc, rX': é uma chamada
        (usado no decodificador Mobiclip escrito à mão) e a execução volta para a+4."""
        if not self.mod.has(a - 4):
            return False
        prev = self.mod.word(a - 4)
        if prev not in (0xE1A0E00F, 0xE28FE000):
            return False
        if (w & 0xFFFFFFF0) == 0xE12FFF10 and (w & 15) != 14:
            return True
        if (w & 0xFFFFFFF0) == 0xE1A0F000 and (w & 15) not in (14, 15):
            return True
        return False

    def is_terminator(self, w, a):
        if self.link_call(w, a):
            return False
        cond = w >> 28
        if (w & 0x0FFFFFF0) == 0x012FFF10:  # bx
            return True
        op = (w >> 25) & 7
        if op == 4 and (w >> 20) & 1 and (w >> 15) & 1:  # ldm com pc
            return True
        if op in (2, 3) and (w >> 20) & 1 and ((w >> 12) & 15) == 15 and not (op == 3 and (w >> 4) & 1):
            return True  # ldr pc
        if op in (0, 1) and ((w >> 12) & 15) == 15:
            opc = (w >> 21) & 15
            if opc not in (8, 9, 10, 11) and not (op == 0 and (w >> 4) & 9 == 9 and (w >> 7) & 1):
                if not ((w & 0x0FB00000) == 0x01000000 or (w & 0x0FB00000) == 0x01200000):
                    return True
        return False

    # ---------- registradores ----------
    def R(self, n, a, plus=8):
        return f"0x{(a + plus) & 0xFFFFFFFF:08x}u" if n == 15 else f"c->r[{n}]"

    def shifter(self, w, a, want_carry):
        """Retorna (expr_c_para_op2, codigo_prévio). Se want_carry, define `sc` (carry do shifter)."""
        pre = []
        if (w >> 25) & 1:
            imm = w & 0xFF
            rot = ((w >> 8) & 15) * 2
            v = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
            if want_carry:
                pre.append(f"uint32_t sc = {'%du' % (v >> 31) if rot else 'c->C'};")
            return f"0x{v:08x}u", pre
        rm = w & 15
        stype = (w >> 5) & 3
        if (w >> 4) & 1:  # deslocamento por registrador
            rs = (w >> 8) & 15
            rmv = self.R(rm, a, 12)
            fn = ["sh_lsl_r", "sh_lsr_r", "sh_asr_r", "sh_ror_r"][stype]
            pre.append(f"uint32_t sc; uint32_t op2 = {fn}(c, {rmv}, c->r[{rs}] & 0xFF, &sc);")
            if not want_carry:
                pre[-1] = pre[-1]  # sc ignorado
            return "op2", pre
        amt = (w >> 7) & 31
        rmv = self.R(rm, a)
        if stype == 0:
            if amt == 0:
                if want_carry:
                    pre.append("uint32_t sc = c->C;")
                return rmv, pre
            if want_carry:
                pre.append(f"uint32_t sc = ({rmv} >> {32 - amt}) & 1;")
            return f"({rmv} << {amt})", pre
        if stype == 1:
            if amt == 0:
                if want_carry:
                    pre.append(f"uint32_t sc = {rmv} >> 31;")
                return "0u", pre
            if want_carry:
                pre.append(f"uint32_t sc = ({rmv} >> {amt - 1}) & 1;")
            return f"({rmv} >> {amt})", pre
        if stype == 2:
            if amt == 0:
                if want_carry:
                    pre.append(f"uint32_t sc = {rmv} >> 31;")
                return f"(uint32_t)((int32_t){rmv} >> 31)", pre
            if want_carry:
                pre.append(f"uint32_t sc = ({rmv} >> {amt - 1}) & 1;")
            return f"(uint32_t)((int32_t){rmv} >> {amt})", pre
        if amt == 0:  # RRX
            if want_carry:
                pre.append(f"uint32_t sc = {rmv} & 1;")
            return f"((c->C << 31) | ({rmv} >> 1))", pre
        if want_carry:
            pre.append(f"uint32_t sc = ({rmv} >> {amt - 1}) & 1;")
        return f"(({rmv} >> {amt}) | ({rmv} << {32 - amt}))", pre

    # ---------- tradução ----------
    def translate(self, a, w):
        cond = w >> 28
        if cond == 0xF:
            if (w >> 25) & 7 == 5:  # BLX imm
                tgt = (a + 8 + sx(w & 0xFFFFFF, 24) * 4 + ((w >> 24) & 1) * 2) & 0xFFFFFFFF
                return self.emit_call(tgt, True, a + 4)
            if (w & 0xFD70F000) == 0xF550F000:  # PLD
                return "/* pld */"
            return self.unimp(a, w)
        body = self.translate_body(a, w)
        c = COND[cond]
        if c is None:
            return body
        return f"if ({c}) {{ {body} }}"

    def unimp(self, a, w):
        self.unimpl += 1
        return f"rt_unimpl(c, 0x{a:08x}u, 0x{w:08x}u); return;"

    def ret_or_jump(self, expr, a):
        # Escrita em PC: retorno (lr) ou salto indireto.
        return f"{{ uint32_t t_ = {expr}; if (t_ != c->r[14] || 1) {{ rt_jump(c, t_); return; }} }}"

    def translate_body(self, a, w):
        op = (w >> 25) & 7
        # ---- BX / BLX reg
        if self.link_call(w, a):
            rm = w & 15
            return f"{{ uint32_t t_ = c->r[{rm}]; c->r[14] = 0x{a + 4:08x}u; rt_call(c, t_); }}"
        if (w & 0x0FFFFFF0) == 0x012FFF10:
            rm = w & 15
            if rm == 14:
                return "return;"
            return f"{{ rt_jump(c, c->r[{rm}]); return; }}"
        if (w & 0x0FFFFFF0) == 0x012FFF30:
            rm = w & 15
            return f"{{ uint32_t t_ = c->r[{rm}]; c->r[14] = 0x{a + 4:08x}u; rt_call(c, t_); }}"
        # ---- CLZ
        if (w & 0x0FFF0FF0) == 0x016F0F10:
            return f"c->r[{(w >> 12) & 15}] = c->r[{w & 15}] ? __builtin_clz(c->r[{w & 15}]) : 32;"
        # ---- MRS / MSR
        if (w & 0x0FBF0FFF) == 0x010F0000:
            rd = (w >> 12) & 15
            return f"c->r[{rd}] = {'rt_get_spsr(c)' if (w >> 22) & 1 else 'rt_get_cpsr(c)'};"
        if (w & 0x0DB0F000) == 0x0120F000:
            mask = (w >> 16) & 15
            if (w >> 25) & 1:
                imm = w & 0xFF
                rot = ((w >> 8) & 15) * 2
                v = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
                val = f"0x{v:08x}u"
            else:
                val = f"c->r[{w & 15}]"
            fn = "rt_set_spsr" if (w >> 22) & 1 else "rt_set_cpsr"
            return f"{fn}(c, {val}, {mask});"
        # ---- QADD etc / SMULxy
        if (w & 0x0F900090) == 0x01000080 and op == 0:
            return self.smul_xy(a, w)
        if (w & 0x0F900FF0) == 0x01000050:
            return self.unimp(a, w)
        # ---- multiplicações
        if op == 0 and (w & 0xF0) == 0x90:
            sub = (w >> 23) & 3
            if sub == 0:
                return self.mul(a, w)
            if sub == 1:
                return self.mull(a, w)
            if sub == 2 and (w & 0x0FB00FF0) == 0x01000090:  # SWP
                rd, rm, rn = (w >> 12) & 15, w & 15, (w >> 16) & 15
                b = (w >> 22) & 1
                rd_, wr = ("rd8", "wr8") if b else ("rd32", "wr32")
                return f"{{ uint32_t t_ = {rd_}(c, c->r[{rn}]); {wr}(c, c->r[{rn}], c->r[{rm}]); c->r[{rd}] = t_; }}"
            return self.unimp(a, w)
        # ---- ldrh/strh/ldrsb/ldrsh/ldrd/strd
        if op == 0 and (w & 0x90) == 0x90:
            return self.halfword(a, w)
        if op in (0, 1):
            if (w & 0x01900000) == 0x01000000:  # TST.. sem S = outras
                return self.unimp(a, w)
            return self.dataproc(a, w)
        if op in (2, 3):
            if op == 3 and (w >> 4) & 1:
                return self.unimp(a, w)
            return self.ldrstr(a, w)
        if op == 4:
            return self.ldmstm(a, w)
        if op == 5:
            tgt = (a + 8 + sx(w & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF
            if (w >> 24) & 1:
                return self.emit_call(tgt, False, a + 4)
            if tgt in self.labels and tgt in self.insns:
                return f"goto L_{tgt:08x};"
            name = self.resolve(tgt, False)
            if name:
                self.ctx.called.add(name)
                return f"{{ {name}(c); return; }}"
            return f"{{ rt_jump(c, 0x{tgt:08x}u); return; }}"
        if op == 7:
            if (w >> 24) & 1:
                return f"rt_swi(c, 0x{(w >> 16) & 0xFF:02x});"
            if (w >> 4) & 1:  # MCR/MRC
                l = (w >> 20) & 1
                rd = (w >> 12) & 15
                cp = (w >> 8) & 15
                args = f"{cp}, {(w >> 21) & 7}, {(w >> 16) & 15}, {w & 15}, {(w >> 5) & 7}"
                if l:
                    return f"c->r[{rd}] = rt_mrc(c, {args});"
                return f"rt_mcr(c, {args}, c->r[{rd}]);"
        return self.unimp(a, w)

    def smul_xy(self, a, w):
        o = (w >> 21) & 3
        rd, rn, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
        x, y = (w >> 5) & 1, (w >> 6) & 1
        hx = f"(int32_t)(int16_t)(c->r[{rm}] >> {16 * x})"
        hy = f"(int32_t)(int16_t)(c->r[{rs}] >> {16 * y})"
        if o == 3:  # SMULxy
            return f"c->r[{rd}] = (uint32_t)({hx} * {hy});"
        if o == 0:  # SMLAxy
            return f"{{ int64_t p_ = (int64_t)({hx} * {hy}) + (int32_t)c->r[{rn}]; c->r[{rd}] = (uint32_t)p_; if (p_ != (int32_t)p_) c->Q = 1; }}"
        if o == 1:  # SMULWy / SMLAWy
            p = f"(uint32_t)(((int64_t)(int32_t)c->r[{rm}] * {hy}) >> 16)"
            if x:
                return f"c->r[{rd}] = {p};"
            return f"c->r[{rd}] = {p} + c->r[{rn}];"
        return self.unimp(a, w)

    def setnz(self, v):
        return f"c->N = {v} >> 31; c->Z = {v} == 0;"

    def mul(self, a, w):
        rd, rn, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
        s = (w >> 20) & 1
        acc = (w >> 21) & 1
        e = f"c->r[{rm}] * c->r[{rs}]" + (f" + c->r[{rn}]" if acc else "")
        out = f"c->r[{rd}] = {e};"
        if s:
            out += " " + self.setnz(f"c->r[{rd}]")
        return out

    def mull(self, a, w):
        hi, lo, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
        sgn, acc, s = (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        if sgn:
            e = f"(uint64_t)((int64_t)(int32_t)c->r[{rm}] * (int64_t)(int32_t)c->r[{rs}])"
        else:
            e = f"((uint64_t)c->r[{rm}] * (uint64_t)c->r[{rs}])"
        if acc:
            e += f" + (((uint64_t)c->r[{hi}] << 32) | c->r[{lo}])"
        out = f"{{ uint64_t p_ = {e}; c->r[{lo}] = (uint32_t)p_; c->r[{hi}] = (uint32_t)(p_ >> 32);"
        if s:
            out += " c->N = (uint32_t)(p_ >> 63); c->Z = p_ == 0;"
        return out + " }"

    def dataproc(self, a, w):
        opc = (w >> 21) & 15
        s = (w >> 20) & 1
        rn = (w >> 16) & 15
        rd = (w >> 12) & 15
        logical = opc in (0, 1, 8, 9, 12, 13, 14, 15)
        reg_shift = not ((w >> 25) & 1) and (w >> 4) & 1
        op2, pre = self.shifter(w, a, s and logical)
        rnv = self.R(rn, a, 12 if reg_shift else 8)
        res = None
        flags = ""
        if opc == 0: res = f"{rnv} & {op2}"
        elif opc == 1: res = f"{rnv} ^ {op2}"
        elif opc == 2: res = f"sub_f(c, {rnv}, {op2}, 1, {s})"
        elif opc == 3: res = f"sub_f(c, {op2}, {rnv}, 1, {s})"
        elif opc == 4: res = f"add_f(c, {rnv}, {op2}, 0, {s})"
        elif opc == 5: res = f"add_f(c, {rnv}, {op2}, c->C, {s})"
        elif opc == 6: res = f"sub_f(c, {rnv}, {op2}, c->C, {s})"
        elif opc == 7: res = f"sub_f(c, {op2}, {rnv}, c->C, {s})"
        elif opc == 8: res = f"{rnv} & {op2}"
        elif opc == 9: res = f"{rnv} ^ {op2}"
        elif opc == 10: res = f"sub_f(c, {rnv}, {op2}, 1, 1)"
        elif opc == 11: res = f"add_f(c, {rnv}, {op2}, 0, 1)"
        elif opc == 12: res = f"{rnv} | {op2}"
        elif opc == 13: res = f"{op2}"
        elif opc == 14: res = f"{rnv} & ~({op2})"
        elif opc == 15: res = f"~({op2})"
        body = " ".join(pre)
        test = opc in (8, 9, 10, 11)
        if test:
            body += f" uint32_t r_ = {res};"
            if logical:
                body += " c->N = r_ >> 31; c->Z = r_ == 0; c->C = sc;"
            else:
                body += " (void)r_;"
            return "{ " + body + " }"
        body += f" uint32_t r_ = {res};"
        if s and logical:
            body += " c->N = r_ >> 31; c->Z = r_ == 0; c->C = sc;"
        if rd == 15:
            if s:
                return "{ " + body + f" rt_exception_return(c, r_); return; }}"
            if opc == 13 and not ((w >> 25) & 1) and (w & 0xFFF) == 14:
                return "{ " + body + " return; }"  # mov pc, lr
            if self.jump_table(w):
                cases = " ".join(f"case 0x{p:08x}u: goto L_{p:08x};" for p in sorted(self.labels) if p > a and p <= a + 4 * 600 and p in self.insns)
                return "{ " + body + f" switch (r_) {{ {cases} default: rt_jump(c, r_); return; }} }}"
            return "{ " + body + " rt_jump(c, r_); return; }"
        body += f" c->r[{rd}] = r_;"
        return "{ " + body + " }"

    def addr_mode(self, w, a, imm_off, off_expr):
        """Gera (endereço_efetivo, writeback) para ldr/str."""
        p, u, wb = (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1
        rn = (w >> 16) & 15
        base = self.R(rn, a)
        sign = "+" if u else "-"
        if p:
            ea = f"{base} {sign} {off_expr}"
            write = f"c->r[{rn}] = ea_;" if wb else ""
        else:
            ea = base
            write = f"c->r[{rn}] = {base} {sign} {off_expr};"
        return ea, write, rn

    def ldrstr(self, a, w):
        l, b = (w >> 20) & 1, (w >> 22) & 1
        rd = (w >> 12) & 15
        rn = (w >> 16) & 15
        if (w >> 25) & 1:
            off, pre = self.shifter(w & ~(1 << 25), a, False)
        else:
            off, pre = f"0x{w & 0xFFF:x}u", []
        # literal pool: ldr rX, [pc, #imm] -> constante
        if l and rn == 15 and not (w >> 25) & 1 and (w >> 24) & 1 and not b:
            u = (w >> 23) & 1
            lit = a + 8 + ((w & 0xFFF) if u else -(w & 0xFFF))
            if self.mod.has(lit) and rd != 15:
                return f"c->r[{rd}] = 0x{self.mod.word(lit):08x}u;"
        ea, write, rnn = self.addr_mode(w, a, None, off)
        body = " ".join(pre) + f" uint32_t ea_ = {ea};"
        if l:
            fn = "rd8" if b else "rd32r"
            body += f" uint32_t v_ = {fn}(c, ea_);"
            if write and rnn != rd:
                body += " " + write
            if rd == 15:
                if rnn == 13 and not (w >> 24) & 1:  # ldr pc, [sp], #4
                    return "{ " + body + " return; }"
                return "{ " + body + " rt_jump(c, v_); return; }"
            body += f" c->r[{rd}] = v_;"
        else:
            val = self.R(rd, a, 12)
            fn = "wr8" if b else "wr32"
            body += f" {fn}(c, ea_, {val});"
            if write:
                body += " " + write
        return "{ " + body + " }"

    def halfword(self, a, w):
        l = (w >> 20) & 1
        sh = (w >> 5) & 3
        rd = (w >> 12) & 15
        if (w >> 22) & 1:
            off = f"0x{((w >> 4) & 0xF0) | (w & 15):x}u"
        else:
            off = f"c->r[{w & 15}]"
        ea, write, rnn = self.addr_mode(w, a, None, off)
        body = f"uint32_t ea_ = {ea};"
        if l:
            fn = {1: "rd16", 2: "(uint32_t)(int32_t)(int8_t)rd8", 3: "(uint32_t)(int32_t)(int16_t)rd16"}[sh]
            body += f" uint32_t v_ = {fn}(c, ea_);"
            if write and rnn != rd:
                body += " " + write
            body += f" c->r[{rd}] = v_;"
        else:
            if sh == 1:
                body += f" wr16(c, ea_, {self.R(rd, a, 12)});"
            elif sh == 2:  # LDRD
                body += f" c->r[{rd}] = rd32(c, ea_); c->r[{rd + 1}] = rd32(c, ea_ + 4);"
            else:  # STRD
                body += f" wr32(c, ea_, c->r[{rd}]); wr32(c, ea_ + 4, c->r[{rd + 1}]);"
            if write:
                body += " " + write
        return "{ " + body + " }"

    def ldmstm(self, a, w):
        p, u, sbit, wb, l = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn = (w >> 16) & 15
        regs = [i for i in range(16) if (w >> i) & 1]
        if sbit:
            return self.unimp(a, w)
        n = len(regs)
        if u:
            start = "c->r[%d] + 4" % rn if p else "c->r[%d]" % rn
            newbase = f"c->r[{rn}] + {4 * n}"
        else:
            start = f"c->r[{rn}] - {4 * n}" + (" " if p else " + 4")
            newbase = f"c->r[{rn}] - {4 * n}"
        body = f"uint32_t ea_ = {start}; uint32_t nb_ = {newbase};"
        tail = ""
        for i, r in enumerate(regs):
            if l:
                if r == 15:
                    body += f" uint32_t pc_ = rd32(c, ea_ + {4 * i});"
                else:
                    body += f" c->r[{r}] = rd32(c, ea_ + {4 * i});"
            else:
                v = self.R(r, a, 12)
                body += f" wr32(c, ea_ + {4 * i}, {v});"
        if wb and not (l and rn in regs):
            body += f" c->r[{rn}] = nb_;"
        if l and 15 in regs:
            if rn == 13:
                body += " (void)pc_; return;"  # pop {..., pc}: retorno
            else:
                body += " rt_jump(c, pc_); return;"
        return "{ " + body + " }"

    def ends_flow(self, w, a):
        """Instrução incondicional que nunca cai na seguinte."""
        if w >> 28 != 0xE:
            return False
        op = (w >> 25) & 7
        if op == 5 and not (w >> 24) & 1:
            return True
        return self.is_terminator(w, a)

    def fallthrough(self, nxt):
        name = self.resolve(nxt, False)
        if name:
            self.ctx.called.add(name)
            return f"  {{ {name}(c); return; }} /* continua em {nxt:08x} */"
        return f"  rt_fallthrough(c, 0x{nxt:08x}u); return;"

    def generate(self):
        self.discover()
        name = self.fname(self.mod, self.addr)
        out = [f"void {name}(Cpu *c) {{"]
        if self.order and self.order[0] != self.addr:
            # a função salta para código compartilhado em endereço menor que a entrada
            # (ex.: divisão de 64 bits): o C sai em ordem de endereço, então começa com um goto
            self.labels.add(self.addr)
            out.append(f"  goto L_{self.addr:08x};")
        prev = None
        for a in self.order:
            if prev is not None and a != prev + 4 and not self.ends_flow(self.insns[prev], prev):
                out.append(self.fallthrough(prev + 4))
            if a in self.labels:
                out.append(f"L_{a:08x}:;")
            out.append(f"  /* {a:08x} */ {self.translate(a, self.insns[a])}")
            prev = a
        if prev is not None and not self.ends_flow(self.insns[prev], prev):
            out.append(self.fallthrough(prev + 4))
        out.append("}")
        return name, "\n".join(out)


class Ctx:
    def __init__(self, mods, funcs):
        self.mods, self.funcs = mods, funcs
        self.called = set()
        self.static = [mods["arm9"], mods["itcm"]]

    def funcs_of(self, mod):
        return self.funcs.get(mod.name, {})

    def resolve(self, frm, target, thumb):
        """Nome C da função alvo, ou None para usar rt_call (Thumb, outro overlay)."""
        if thumb:
            return None
        for m in ([frm] + self.static):
            if m.has(target):
                if (m.name, target) in self.hle:
                    return self.hle[(m.name, target)]
                if self.funcs_of(m).get(target, True):
                    self.new_funcs.add((m.name, target))  # função nova achada por BL
                return f"f_{m.name}_{target:08x}"
        # BL de um overlay para outro (ex.: ov000 chamando o ov016, que fica residente): não dá para saber
        # qual overlay estará carregado, então a chamada fica dinâmica (rt_call), mas a função é gerada em
        # todo overlay que cobre o alvo e não se sobrepõe a quem chama
        if frm.kind == "overlay":
            for m in self.mods.values():
                if m.kind != "overlay" or m is frm or not m.has(target):
                    continue
                if m.ram < frm.ram + len(frm.data) and frm.ram < m.ram + len(m.data):
                    continue
                if (m.name, target) not in self.hle and self.funcs_of(m).get(target, True):
                    self.spec_funcs.add((m.name, target))
        return None


def main():
    lay, mods, funcs = load()
    ctx = Ctx(mods, funcs)
    ctx.new_funcs = set()
    ctx.hle = {}
    ctx.keep = []
    hle_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hle.txt")
    for line in open(hle_path):
        line = line.split("#")[0].split()
        if len(line) == 3 and line[0] == "keep":
            ctx.keep.append((line[1], int(line[2], 16)))
        elif len(line) == 3:
            ctx.hle[(line[0], int(line[1], 16))] = line[2]
    os.makedirs(OUT, exist_ok=True)
    todo = [(m, a) for m in funcs for a, t in funcs[m].items() if not t and m in mods]
    todo += ctx.keep
    done = set()
    generated = []
    stats = collections.Counter()
    ctx.spec_funcs = set()
    spec = set()  # palpites (alvos de BL entre overlays): descartados se não parecem código
    while todo:
        m, a = todo.pop()
        if (m, a) in done or (m, a) in ctx.hle or m not in mods or not mods[m].has(a):
            continue
        is_spec = (m, a) in spec
        g = FuncGen(mods[m], a, ctx)
        ctx.new_funcs.clear(); ctx.spec_funcs.clear()
        name, code = g.generate()
        if is_spec and g.unimpl:
            stats["spec_drop"] += 1
            continue
        done.add((m, a))
        generated.append((m, a, name, code))
        stats["funcs"] += 1
        stats["insns"] += len(g.insns)
        stats["unimpl"] += g.unimpl
        for nm, na in list(ctx.new_funcs):
            if (nm, na) not in done:
                todo.append((nm, na))
                if is_spec and (nm, na) not in done: spec.add((nm, na))
        for nm, na in list(ctx.spec_funcs):
            if (nm, na) not in done:
                todo.append((nm, na)); spec.add((nm, na))
        ctx.new_funcs.clear(); ctx.spec_funcs.clear()
    # palpites que chamam (por nome) uma função descartada também saem; uma função de verdade que chame
    # algo descartado faz o descartado ser gerado mesmo assim
    import re as _re
    ref_re = _re.compile(r"\b(f_[a-z0-9]+_[0-9a-f]{8})\(c\)")
    while True:
        names = {name for _, _, name, _ in generated} | set(ctx.hle.values())
        drop, force = set(), set()
        for m, a, name, code in generated:
            miss = [r for r in ref_re.findall(code) if r not in names]
            if not miss:
                continue
            if (m, a) in spec:
                drop.add(name)
            else:
                force.update(miss)
        if not drop and not force:
            break
        generated = [g for g in generated if g[2] not in drop]
        stats["spec_drop"] += len(drop)
        for n in force:
            _, mn, ad = n.split("_")
            g = FuncGen(mods[mn], int(ad, 16), ctx)
            ctx.new_funcs.clear(); ctx.spec_funcs.clear()
            nm, code = g.generate()
            spec.discard((mn, int(ad, 16)))
            generated.append((mn, int(ad, 16), nm, code))
            stats["forced"] += 1
    stats["funcs"] = len(generated)
    generated.sort()
    names = {name for _, _, name, _ in generated} | set(ctx.hle.values())

    def write_if_changed(path, text):
        if os.path.exists(path) and open(path).read() == text:
            return
        with open(path, "w") as f:
            f.write(text)

    # protótipos (usado por table.c e pelo runtime)
    hdr = ["#pragma once", '#include "cpu.h"']
    for (m, a), n in sorted(ctx.hle.items()):
        hdr.append(f"void {n}(Cpu *c); /* HLE de {m}:{a:08x} */")
    for _, _, name, _ in generated:
        hdr.append(f"void {name}(Cpu *c);")
    write_if_changed(f"{OUT}/funcs.h", "\n".join(hdr) + "\n")
    # tabela de funções: (módulo, endereço, ponteiro)
    modidx = {"arm9": -1, "itcm": -2, "dtcm": -3}
    tl = ['#include "funcs.h"', '#include "runtime.h"', "const FuncEntry g_funcs[] = {"]
    for m, a, name, _ in generated:
        tl.append(f"  {{{modidx.get(m, None) if m in modidx else int(m[2:])}, 0x{a:08x}u, {name}}},")
    for (m, a), n in sorted(ctx.hle.items()):
        tl.append(f"  {{{modidx.get(m, None) if m in modidx else int(m[2:])}, 0x{a:08x}u, {n}}},")
    tl.append("};")
    tl.append(f"const int g_funcs_count = {len(generated) + len(ctx.hle)};")
    write_if_changed(f"{OUT}/table.c", "\n".join(tl) + "\n")
    # arquivos estáveis: um por módulo e bloco de 16 KB, para recompilar só o que mudou
    buckets = collections.defaultdict(list)
    for m, a, name, code in generated:
        buckets[f"{m}_{a >> 14:05x}"].append((name, code))
    import re
    call_re = re.compile(r"\b(f_[a-z0-9]+_[0-9a-f]{8}|hle_[A-Za-z0-9_]+)\(c\)")
    wanted = set()
    for key, items in buckets.items():
        fname = f"gen_{key}.c"
        wanted.add(fname)
        protos = set()
        for name, code in items:
            protos.update(call_re.findall(code))
            protos.add(name)
        text = '#include "runtime.h"\n\n' + "".join(f"void {p}(Cpu *c);\n" for p in sorted(protos)) + "\n"
        text += "\n\n".join(code for _, code in items) + "\n"
        write_if_changed(f"{OUT}/{fname}", text)
    for old in os.listdir(OUT):
        if old.startswith("gen_") and old.endswith(".c") and old not in wanted:
            os.remove(f"{OUT}/{old}")
    files = len(buckets)
    # tabela de overlays para o runtime
    ovs = lay["overlays"]
    with open(f"{OUT}/overlays.h", "w") as h:
        h.write("#pragma once\n#include <stdint.h>\n")
        h.write("typedef struct { uint32_t ram, size, bss, sinit_start, sinit_end; } OverlayInfo;\n")
        h.write(f"#define NUM_OVERLAYS {len(ovs)}\nextern const OverlayInfo g_overlays[NUM_OVERLAYS];\n")
    with open(f"{OUT}/overlays.c", "w") as f:
        f.write("#include \"overlays.h\"\nconst OverlayInfo g_overlays[NUM_OVERLAYS] = {\n")
        for o in ovs:
            f.write(f"  {{0x{o['ram']:08x}u, 0x{o['size']:x}u, 0x{o['bss']:x}u, 0x{o['sinit_start']:08x}u, 0x{o['sinit_end']:08x}u}},\n")
        f.write("};\n")
    print(dict(stats), "arquivos:", files)


if __name__ == "__main__":
    main()

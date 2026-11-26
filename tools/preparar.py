#!/usr/bin/env python3
"""Prepara o jogo a partir da SUA ROM: extrai o código, recompila para C e compila o ie3.exe.

É o que a release roda na primeira vez ("Preparar jogo.bat"). A release só traz ferramentas
e o código do projeto; o código do jogo é gerado aqui, na máquina de quem tem a ROM.

Uso: python -X utf8 -I tools/preparar.py <rom.nds> [opções]
  --saida <pasta>     onde fica o ie3.exe (padrão: a pasta "jogo" da release, ao lado de "ferramentas")
  --zig <zig>         compilador (padrão: ferramentas/zig/zig.exe)
  --sdl <pasta>       SDL2 "devel mingw" (padrão: ferramentas/sdl2)
  --alvo windows|linux  (padrão: o sistema atual; linux usa o SDL2 do sistema)
  --tarefas N         compilações em paralelo (padrão: núcleos do processador)
  --progresso <arq>   escreve "<porcentagem>\t<mensagem>" neste arquivo (o launcher mostra a barra);
                      -1 na porcentagem quer dizer erro
  --versao <v>        grava <saida>/versao.txt no fim (o launcher confere se o jogo é desta versão)
"""
import concurrent.futures, glob, hashlib, os, shutil, subprocess, sys, time

ROM_SHA1 = "a70e9a336f6ea8ff78003938dcd024533f034a68"
FONTE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # raiz do código do projeto
CFLAGS = ["-O1", "-w", "-fno-strict-aliasing", "-fwrapv",
          # o C gerado chama funções de outros arquivos sem protótipo (o GCC aceita; o clang do zig não)
          "-Wno-error=implicit-function-declaration", "-Wno-error=int-conversion",
          "-Wno-error=incompatible-pointer-types", "-Wno-error=implicit-int"]
RT = ["runtime", "thumb", "arm_interp", "threads", "video", "gx", "snd", "rom", "platform",
      "hostfiber", "config", "launcher", "launcher_sdl", "main"]


PROGRESSO = None


def progresso(pct, msg):
    if not PROGRESSO:
        return
    tmp = PROGRESSO + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(f"{pct}\t{msg}\n")
    for _ in range(20):  # o launcher pode estar lendo o arquivo (no Windows isso bloqueia a troca)
        try:
            os.replace(tmp, PROGRESSO); return
        except PermissionError:
            time.sleep(0.05)


def passo(msg, pct=None):
    print(f"\n== {msg}", flush=True)
    if pct is not None:
        progresso(pct, msg.split(" ", 1)[1] if msg[:1].isdigit() else msg)


def falha(msg):
    print(f"\nERRO: {msg}", flush=True)
    m = msg.splitlines()[0]
    progresso(-1, f"Erro: {m[:1].upper()}{m[1:]}. Detalhes em jogo/preparo.log")
    sys.exit(1)


def args():
    a = sys.argv[1:]
    if not a or a[0] in ("-h", "--help"):
        print(__doc__); sys.exit(0 if a else 1)
    o = {"rom": a[0], "saida": None, "zig": None, "sdl": None,
         "alvo": "windows" if os.name == "nt" else "linux", "tarefas": os.cpu_count() or 4,
         "progresso": None, "versao": None}
    i = 1
    while i < len(a):
        k = a[i].lstrip("-")
        if k not in o or i + 1 >= len(a):
            falha(f"opção desconhecida: {a[i]}")
        o[k] = int(a[i + 1]) if k == "tarefas" else a[i + 1]
        i += 2
    # na release: <release>/ferramentas/fonte (este código), <release>/ferramentas/{zig,sdl2,python}
    na_release = os.path.basename(FONTE) == "fonte"
    ferr = os.path.dirname(FONTE) if na_release else os.path.join(FONTE, "ferramentas")
    o["saida"] = o["saida"] or (os.path.join(os.path.dirname(ferr), "jogo") if na_release else os.path.join(FONTE, "build-local"))
    o["zig"] = o["zig"] or os.path.join(ferr, "zig", "zig.exe" if os.name == "nt" else "zig")
    o["sdl"] = o["sdl"] or os.path.join(ferr, "sdl2")
    o["ferr"] = ferr
    return o


def conferir_rom(path):
    if not os.path.isfile(path):
        falha(f"não achei a ROM em {path}")
    h = hashlib.sha1(open(path, "rb").read()).hexdigest()
    if h != ROM_SHA1:
        falha(f"esta ROM é de outra versão (SHA-1 {h}).\nO projeto precisa de Inazuma Eleven 3 - Sekai e no Chousen!! The Ogre (BOEJ) "
              f"com a tradução BR T1.3 (SHA-1 {ROM_SHA1}).")
    print("ROM conferida (BOEJ, tradução BR T1.3).")


def rodar_py(script, *a, pct=None, msg="", estimativa=30):
    """Roda outro script do projeto. Sem saída de progresso dele, a barra avança pelo tempo estimado."""
    p = subprocess.Popen([sys.executable, "-X", "utf8", "-I", script, *a], cwd=FONTE, stdin=subprocess.DEVNULL)
    t0 = time.time()
    while p.poll() is None:
        time.sleep(0.5)
        if pct:
            f = min(0.95, (time.time() - t0) / estimativa)
            progresso(int(pct[0] + (pct[1] - pct[0]) * f), msg)
    r = p
    if r.returncode:
        falha(f"{os.path.basename(script)} falhou (código {r.returncode})")


def compilar(o, trab):
    zig = o["zig"]
    if not os.path.isfile(zig):
        falha(f"não achei o compilador em {zig}")
    win = o["alvo"] == "windows"
    cc = [zig, "cc"] + (["-target", "x86_64-windows-gnu"] if win else [])  # linux: alvo nativo, com o SDL2 do sistema
    gen = os.path.join(trab, "gen")
    inc = ["-I" + os.path.join(FONTE, "runtime"), "-I" + gen]
    if win:
        sdl = o["sdl"]
        inc += ["-DHAVE_SDL2", "-Dmain=SDL_main", "-I" + os.path.join(sdl, "include"), "-I" + os.path.join(sdl, "include", "SDL2")]
    else:
        flags = subprocess.run(["sdl2-config", "--cflags"], capture_output=True, text=True).stdout.split()
        inc += ["-DHAVE_SDL2", *flags]
    obj = os.path.join(trab, "obj")
    os.makedirs(obj, exist_ok=True)
    jobs = [(os.path.join(FONTE, "runtime", n + ".c"), os.path.join(obj, "rt_" + n + ".o")) for n in RT]
    jobs += [(c, os.path.join(obj, os.path.basename(c)[:-2] + ".o")) for c in sorted(glob.glob(os.path.join(gen, "*.c")))]

    def um(job):
        src, dst = job
        if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            return None  # já compilado (dá para continuar de onde parou)
        r = subprocess.run(cc + CFLAGS + inc + ["-c", src, "-o", dst + ".tmp"], capture_output=True, text=True, stdin=subprocess.DEVNULL)
        if r.returncode:
            return f"{os.path.basename(src)}:\n{r.stderr[-3000:]}"
        os.replace(dst + ".tmp", dst)
        return None

    # Na primeira ligação o zig monta a biblioteca C do Windows (MinGW-w64) e o compiler_rt a partir do
    # código-fonte, o que leva minutos. Um programa vazio ligado em paralelo já deixa isso no cache
    # enquanto os arquivos do jogo compilam; a ligação final fica rápida.
    crt = os.path.join(o["ferr"], "crt")
    usa_crt = win and os.path.isfile(os.path.join(crt, "ligacao.txt"))
    aquecer = None
    if win and not usa_crt:
        vazio = os.path.join(obj, "aquecer.c")
        with open(vazio, "w") as f:
            f.write("int main(void) { return 0; }\n")
        aquecer = subprocess.Popen(cc + CFLAGS + [vazio, "-o", os.path.join(obj, "aquecer.exe"), "-lmingw32", "-lcomdlg32",
                                                  "-Wl,--subsystem,windows", "-s"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    t0, feitos, erros = time.time(), 0, []
    with concurrent.futures.ThreadPoolExecutor(o["tarefas"]) as ex:
        for e in ex.map(um, jobs):
            feitos += 1
            if e:
                erros.append(e)
            if feitos % 10 == 0 or feitos == len(jobs):
                print(f"\r  {feitos}/{len(jobs)} arquivos ({int(time.time() - t0)} s)", end="", flush=True)
                progresso(30 + 67 * feitos // len(jobs), f"Compilando o jogo ({feitos} de {len(jobs)} arquivos)")
    print()
    if erros:
        print(erros[0])
        falha(f"{len(erros)} arquivo(s) não compilaram")

    objs = [d for _, d in jobs]
    t1 = time.time()
    if aquecer:
        while aquecer.poll() is None:
            progresso(97, f"Preparando a biblioteca do Windows (só na primeira vez; {int(time.time() - t1)} s)")
            time.sleep(1)
    print(f"  biblioteca do Windows pronta ({int(time.time() - t1)} s de espera)", flush=True)
    progresso(98, "Montando o executável")
    os.makedirs(o["saida"], exist_ok=True)
    if win:
        res = os.path.join(obj, "ie3.res")  # ícone, manifesto e dados de versão; o lld liga o .res direto
        progresso(97, "Preparando o ícone do executável")
        if usa_crt:
            shutil.copy2(os.path.join(crt, "ie3.res"), res)
            r = subprocess.CompletedProcess([], 0)
        else:
          try:  # o ícone é opcional: se o zig rc travar ou falhar, segue sem ele
            r = subprocess.run([zig, "rc", "/c", "65001", "ie3.rc", res], capture_output=True, text=True,
                               cwd=os.path.join(FONTE, "win"), stdin=subprocess.DEVNULL, timeout=120)
          except subprocess.TimeoutExpired:
            r = None
        if r and r.returncode == 0 and os.path.exists(res):
            objs.append(res)
        else:
            print("aviso: sem ícone no .exe (o recurso win/ie3.rc não compilou)")
        exe = os.path.join(o["saida"], "ie3.exe")
        sdl = o["sdl"]
        link = cc + objs + ["-o", exe, "-L" + os.path.join(sdl, "lib"), "-lmingw32", "-lSDL2main",
                            os.path.join(sdl, "lib", "libSDL2.dll.a"),  # SDL2.dll (não a libSDL2.a estática)
                            "-lcomdlg32",  # janela de escolher a ROM do launcher
                            "-Wl,--subsystem,windows", "-Wl,--stack,67108864", "-s"]
        if usa_crt:
            # liga direto com o lld, usando a biblioteca C do Windows que já vem pronta na release
            libs = [os.path.join(crt, n) for n in open(os.path.join(crt, "ligacao.txt"), encoding="utf-8").read().split()]
            link = [zig, "lld-link", "-lldmingw", "-ERRORLIMIT:0", "-NOLOGO", "-MLLVM:-float-abi=hard", "-STACK:67108864",
                    "-BASE:5368709120", "-BUILD-ID:NO", "-MACHINE:X64", "-BREPRO", "-OUT:" + exe,
                    "-IMPLIB:" + os.path.join(obj, "ie3.lib"), *objs,
                    os.path.join(sdl, "lib", "libSDL2main.a"), os.path.join(sdl, "lib", "libSDL2.dll.a"),
                    "-SUBSYSTEM:windows,6.0", "-ALTERNATENAME:__image_base__=__ImageBase", *libs]
    else:
        exe = os.path.join(o["saida"], "ie3")
        libs = subprocess.run(["sdl2-config", "--libs"], capture_output=True, text=True).stdout.split()
        link = cc + objs + ["-o", exe] + libs + ["-lm"]
    t1 = time.time()
    saida_link = os.path.join(obj, "ligacao.log")
    with open(saida_link, "w", encoding="utf-8", errors="replace") as lf:
        r = subprocess.Popen(link, stdout=lf, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        while r.poll() is None:
            progresso(98, f"Montando o executável ({int(time.time() - t1)} s)")
            time.sleep(1)
    print(f"  ligação: {int(time.time() - t1)} s", flush=True)
    if r.returncode:
        print(open(saida_link, encoding="utf-8", errors="replace").read()[-4000:])
        falha("a ligação do executável falhou")
    if win:
        pdb = os.path.splitext(exe)[0] + ".pdb"
        if os.path.exists(pdb):
            os.remove(pdb)
        dll = os.path.join(o["sdl"], "bin", "SDL2.dll")
        if os.path.exists(dll):
            shutil.copy(dll, o["saida"])
    shutil.copy(os.path.join(FONTE, "recomplauncher.json"), o["saida"])
    return exe


def main():
    global PROGRESSO
    o = args()
    PROGRESSO = o["progresso"]
    rom = os.path.abspath(o["rom"])
    trab = os.path.join(o["saida"], "preparo")  # arquivos intermediários (podem ser apagados depois)
    print("Inazuma Eleven 3 Recomp: preparando o jogo a partir da sua ROM.")
    passo("1/4 Conferindo a ROM", 1)
    conferir_rom(rom)
    passo("2/4 Extraindo o código da ROM", 3)
    ext = os.path.join(trab, "extracted")
    rodar_py(os.path.join(FONTE, "tools", "extract.py"), rom, ext, pct=(3, 8), msg="Extraindo o código da ROM", estimativa=15)
    passo("3/4 Traduzindo o código do jogo para C", 8)
    rodar_py(os.path.join(FONTE, "recomp", "armrecomp.py"), ext, os.path.join(FONTE, "analysis", "functions_list.txt"), os.path.join(trab, "gen"),
             pct=(8, 30), msg="Traduzindo o código do jogo para C", estimativa=50)
    # o cache do zig fica junto dos intermediários: apagar "preparo" limpa tudo
    os.environ.setdefault("ZIG_GLOBAL_CACHE_DIR", os.path.join(trab, "zig-cache"))
    os.environ.setdefault("ZIG_LOCAL_CACHE_DIR", os.path.join(trab, "zig-cache"))
    passo(f"4/4 Compilando ({o['tarefas']} de cada vez; leva alguns minutos)")
    progresso(30, "Compilando o jogo")
    exe = compilar(o, trab)
    if o["versao"]:
        with open(os.path.join(o["saida"], "versao.txt"), "w", encoding="utf-8") as f:
            f.write(o["versao"] + "\n")
    progresso(100, "Pronto")
    # rodando sozinho (sem o launcher): deixa a ROM já escolhida no .ini
    ini = os.path.join(o["saida"], "ie3recomp.ini")
    if not PROGRESSO and not os.path.exists(ini):
        with open(ini, "w", encoding="utf-8") as f:
            f.write(f"rom={rom}\n")
    print(f"\nPronto: {exe}\nOs arquivos em {trab} podem ser apagados (contêm o código do jogo gerado da sua ROM).")


if __name__ == "__main__":
    main()

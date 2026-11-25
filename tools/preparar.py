#!/usr/bin/env python3
"""Prepara o jogo a partir da SUA ROM: extrai o código, recompila para C e compila o ie3.exe.

É o que a release roda na primeira vez ("Preparar jogo.bat"). A release só traz ferramentas
e o código do projeto; o código do jogo é gerado aqui, na máquina de quem tem a ROM.

Uso: python -X utf8 -I tools/preparar.py <rom.nds> [opções]
  --saida <pasta>     onde fica o ie3.exe (padrão: a pasta da release, ao lado de "ferramentas")
  --zig <zig>         compilador (padrão: ferramentas/zig/zig.exe)
  --sdl <pasta>       SDL2 "devel mingw" (padrão: ferramentas/sdl2)
  --alvo windows|linux  (padrão: o sistema atual; linux usa o SDL2 do sistema)
  --tarefas N         compilações em paralelo (padrão: núcleos do processador)
"""
import concurrent.futures, glob, hashlib, os, shutil, subprocess, sys, time

ROM_SHA1 = "a70e9a336f6ea8ff78003938dcd024533f034a68"
FONTE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # raiz do código do projeto
CFLAGS = ["-O1", "-w", "-fno-strict-aliasing", "-fwrapv",
          # o C gerado chama funções de outros arquivos sem protótipo (o GCC aceita; o clang do zig não)
          "-Wno-error=implicit-function-declaration", "-Wno-error=int-conversion",
          "-Wno-error=incompatible-pointer-types", "-Wno-error=implicit-int"]
RT = ["runtime", "thumb", "arm_interp", "threads", "video", "gx", "snd", "rom", "platform",
      "hostfiber", "config", "launcher", "main"]


def passo(msg):
    print(f"\n== {msg}", flush=True)


def falha(msg):
    print(f"\nERRO: {msg}", flush=True)
    sys.exit(1)


def args():
    a = sys.argv[1:]
    if not a or a[0] in ("-h", "--help"):
        print(__doc__); sys.exit(0 if a else 1)
    o = {"rom": a[0], "saida": None, "zig": None, "sdl": None,
         "alvo": "windows" if os.name == "nt" else "linux", "tarefas": os.cpu_count() or 4}
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
    o["saida"] = o["saida"] or (os.path.dirname(ferr) if na_release else os.path.join(FONTE, "build-local"))
    o["zig"] = o["zig"] or os.path.join(ferr, "zig", "zig.exe" if os.name == "nt" else "zig")
    o["sdl"] = o["sdl"] or os.path.join(ferr, "sdl2")
    return o


def conferir_rom(path):
    if not os.path.isfile(path):
        falha(f"não achei a ROM em {path}")
    h = hashlib.sha1(open(path, "rb").read()).hexdigest()
    if h != ROM_SHA1:
        falha(f"esta ROM tem SHA-1 {h}.\nO projeto precisa de Inazuma Eleven 3 - Sekai e no Chousen!! The Ogre (BOEJ) "
              f"com a tradução BR T1.3 (SHA-1 {ROM_SHA1}).")
    print("ROM conferida (BOEJ, tradução BR T1.3).")


def rodar_py(script, *a):
    r = subprocess.run([sys.executable, "-X", "utf8", "-I", script, *a], cwd=FONTE)
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
        r = subprocess.run(cc + CFLAGS + inc + ["-c", src, "-o", dst + ".tmp"], capture_output=True, text=True)
        if r.returncode:
            return f"{os.path.basename(src)}:\n{r.stderr[-3000:]}"
        os.replace(dst + ".tmp", dst)
        return None

    t0, feitos, erros = time.time(), 0, []
    with concurrent.futures.ThreadPoolExecutor(o["tarefas"]) as ex:
        for e in ex.map(um, jobs):
            feitos += 1
            if e:
                erros.append(e)
            if feitos % 10 == 0 or feitos == len(jobs):
                print(f"\r  {feitos}/{len(jobs)} arquivos ({int(time.time() - t0)} s)", end="", flush=True)
    print()
    if erros:
        print(erros[0])
        falha(f"{len(erros)} arquivo(s) não compilaram")

    objs = [d for _, d in jobs]
    os.makedirs(o["saida"], exist_ok=True)
    if win:
        res = os.path.join(obj, "ie3.res")  # ícone, manifesto e dados de versão; o lld liga o .res direto
        r = subprocess.run([zig, "rc", "/c", "65001", "ie3.rc", res], capture_output=True, text=True, cwd=os.path.join(FONTE, "win"))
        if r.returncode == 0 and os.path.exists(res):
            objs.append(res)
        else:
            print("aviso: sem ícone no .exe (o recurso win/ie3.rc não compilou)")
        exe = os.path.join(o["saida"], "ie3.exe")
        sdl = o["sdl"]
        link = cc + objs + ["-o", exe, "-L" + os.path.join(sdl, "lib"), "-lmingw32", "-lSDL2main",
                            os.path.join(sdl, "lib", "libSDL2.dll.a"),  # SDL2.dll (não a libSDL2.a estática)
                            "-Wl,--subsystem,windows", "-Wl,--stack,67108864", "-s"]
    else:
        exe = os.path.join(o["saida"], "ie3")
        libs = subprocess.run(["sdl2-config", "--libs"], capture_output=True, text=True).stdout.split()
        link = cc + objs + ["-o", exe] + libs + ["-lm"]
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-4000:])
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
    o = args()
    rom = os.path.abspath(o["rom"])
    trab = os.path.join(o["saida"], "preparo")  # arquivos intermediários (podem ser apagados depois)
    print("Inazuma Eleven 3 Recomp: preparando o jogo a partir da sua ROM.")
    passo("1/4 Conferindo a ROM")
    conferir_rom(rom)
    passo("2/4 Extraindo o código da ROM")
    ext = os.path.join(trab, "extracted")
    rodar_py(os.path.join(FONTE, "tools", "extract.py"), rom, ext)
    passo("3/4 Traduzindo o código do jogo para C")
    rodar_py(os.path.join(FONTE, "recomp", "armrecomp.py"), ext, os.path.join(FONTE, "analysis", "functions_list.txt"), os.path.join(trab, "gen"))
    # o cache do zig fica junto dos intermediários: apagar "preparo" limpa tudo
    os.environ.setdefault("ZIG_GLOBAL_CACHE_DIR", os.path.join(trab, "zig-cache"))
    os.environ.setdefault("ZIG_LOCAL_CACHE_DIR", os.path.join(trab, "zig-cache"))
    passo(f"4/4 Compilando ({o['tarefas']} de cada vez; leva alguns minutos)")
    exe = compilar(o, trab)
    # deixa a ROM já escolhida para o launcher
    ini = os.path.join(o["saida"], "ie3recomp.ini")
    if not os.path.exists(ini):
        with open(ini, "w", encoding="utf-8") as f:
            f.write(f"rom={rom}\n")
    print(f"\nPronto: {exe}\nOs arquivos em {trab} podem ser apagados (contêm o código do jogo gerado da sua ROM).")


if __name__ == "__main__":
    main()

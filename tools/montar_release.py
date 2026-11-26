#!/usr/bin/env python3
"""Monta o zip da release para Windows: ferramentas + código do projeto, SEM nada do jogo.

Uso: python3 -I tools/montar_release.py <versão> <saída.zip> --zig <ziglang win_amd64 .whl>
       --python <pasta com o Python do conda-forge para win-64 já descompactado>
       --pylibs <ndspy .whl> <crcmod .tar.gz> --sdl <pasta x86_64-w64-mingw32 do SDL2 devel mingw>
       --zig-host <zig que roda nesta máquina, para compilar o launcher>

O usuário descompacta e abre "Inazuma Eleven 3.exe" (o launcher, que só tem código do projeto).
Na primeira vez o launcher roda tools/preparar.py sem janela e gera jogo/ie3.exe a partir da ROM.
Assim a release não leva código do jogo, nem traduzido.
"""
import glob, io, os, shutil, subprocess, sys, tarfile, tempfile, zipfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# o que vai do repositório para ferramentas/fonte (só código do projeto)
FONTE = ["runtime", "recomp", "win", "analysis/functions_list.txt", "analysis/extra_functions.txt",
         "tools/extract.py", "tools/preparar.py", "recomplauncher.json", "README.md"]
# partes do Python que o preparo não usa
PY_FORA = ["Lib/test", "Lib/idlelib", "Lib/tkinter", "Lib/turtledemo", "Lib/ensurepip", "Lib/lib2to3",
           "Lib/pydoc_data", "Lib/site-packages", "include", "libs", "Tools", "Scripts", "tcl", "etc",
           "Library/include", "Library/lib", "Library/share", "Library/ssl", "conda-meta", "info"]

LEIA = """Inazuma Eleven 3 Recomp {versao} (versão de testes)

Esta pasta NÃO tem o jogo. Ela traz o launcher e as ferramentas que geram o jogo a partir da SUA ROM.

1. Abra "Inazuma Eleven 3.exe".
2. Arraste a sua ROM (.nds) para a janela, ou clique no quadro para procurar o arquivo.
   A ROM precisa ser Inazuma Eleven 3 - Sekai e no Chousen!! The Ogre (BOEJ) com a tradução BR T1.3.
3. Na primeira vez o launcher gera o jogo a partir da ROM (confere, extrai, traduz para C e
   compila). Leva alguns minutos, dependendo do processador; a barra mostra o andamento.
   Se fechar no meio, ele continua de onde parou na próxima vez.
4. Clique em JOGAR.

O save (ie3.sav) e as opções (ie3recomp.ini) ficam nesta pasta. O jogo gerado fica em "jogo",
com o log (jogo/ie3recomp.log) e o log do preparo (jogo/preparo.log).
A pasta "jogo/preparo" pode ser apagada depois: ela tem os arquivos intermediários gerados da sua ROM.
Não compartilhe a pasta "jogo": ela contém o código do jogo.

Código e instruções: https://github.com/VitorL02/inazumaEleven3Recomp
Ferramentas incluídas e licenças: ferramentas/licencas
"""


def copiar_arvore(src, dst, fora=()):
    for raiz, dirs, arqs in os.walk(src):
        rel = os.path.relpath(raiz, src).replace("\\", "/")
        dirs[:] = [d for d in dirs if (f"{rel}/{d}" if rel != "." else d) not in fora and d != "__pycache__"]
        for a in arqs:
            if a.endswith(".pdb"):
                continue
            os.makedirs(os.path.join(dst, rel), exist_ok=True)
            shutil.copy2(os.path.join(raiz, a), os.path.join(dst, rel, a))


# o zig traz a libc de todos os sistemas; para gerar o .exe só servem a do Windows (MinGW-w64)
ZIG_FORA = ["lib/libc/glibc", "lib/libc/musl", "lib/libc/darwin", "lib/libc/wasi", "lib/libc/freebsd",
            "lib/libc/netbsd", "lib/libc/openbsd", "lib/libcxx", "lib/libcxxabi", "lib/libtsan", "doc", "lib/docs"]


def podar_zig(z):
    for f in ZIG_FORA:
        shutil.rmtree(os.path.join(z, f), ignore_errors=True)
    inc = os.path.join(z, "lib", "libc", "include")
    for d in os.listdir(inc):
        if d != "any-windows-any":
            shutil.rmtree(os.path.join(inc, d), ignore_errors=True)


def main():
    a = sys.argv[1:]
    versao, saida = a[0], a[1]
    o = {}
    i = 2
    while i < len(a):
        k = a[i].lstrip("-")
        j = i + 1
        vals = []
        while j < len(a) and not a[j].startswith("--"):
            vals.append(a[j]); j += 1
        o[k] = vals
        i = j
    tmp = tempfile.mkdtemp()
    top = os.path.join(tmp, f"IE3Recomp-{versao}")
    fer = os.path.join(top, "ferramentas")
    lic = os.path.join(fer, "licencas")
    os.makedirs(lic)

    # código do projeto
    for f in FONTE:
        s, d = os.path.join(RAIZ, f), os.path.join(fer, "fonte", f)
        if os.path.isdir(s):
            copiar_arvore(s, d)
        else:
            os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy2(s, d)

    # Python (conda-forge win-64): as DLLs de Library/bin vão para DLLs/, ao lado dos .pyd
    py = os.path.join(fer, "python")
    copiar_arvore(o["python"][0], py, PY_FORA)
    lb = os.path.join(py, "Library", "bin")
    if os.path.isdir(lb):
        for dll in glob.glob(os.path.join(lb, "*.dll")):
            shutil.copy2(dll, os.path.join(py, "DLLs"))
        shutil.rmtree(os.path.join(py, "Library"))
    shutil.copy2(os.path.join(py, "LICENSE_PYTHON.txt"), os.path.join(lic, "Python.txt"))
    sp = os.path.join(py, "Lib", "site-packages")
    os.makedirs(sp, exist_ok=True)
    for p in o["pylibs"]:
        if p.endswith(".whl"):
            zipfile.ZipFile(p).extractall(sp)
        else:  # crcmod (só fonte): usa a versão em Python puro
            t = tarfile.open(p)
            base = os.path.commonprefix(t.getnames()).split("/")[0]
            t.extractall(tmp, filter="data")
            shutil.copytree(os.path.join(tmp, base, "python3", "crcmod"), os.path.join(sp, "crcmod"))
            shutil.copy2(os.path.join(tmp, base, "LICENSE"), os.path.join(lic, "crcmod.txt"))
    for d in glob.glob(os.path.join(sp, "ndspy-*.dist-info")):
        for f in ("LICENSE", "LICENSE.txt", "licenses/LICENSE"):
            if os.path.exists(os.path.join(d, f)):
                shutil.copy2(os.path.join(d, f), os.path.join(lic, "ndspy.txt"))

    # zig (compilador C com clang, lld e as bibliotecas do MinGW-w64)
    z = zipfile.ZipFile(o["zig"][0])
    for n in z.namelist():
        if n.startswith("ziglang/") and not n.endswith("/"):
            d = os.path.join(fer, "zig", n[len("ziglang/"):])
            os.makedirs(os.path.dirname(d), exist_ok=True)
            with open(d, "wb") as f:
                f.write(z.read(n))
    shutil.copy2(os.path.join(fer, "zig", "LICENSE"), os.path.join(lic, "Zig.txt"))
    podar_zig(os.path.join(fer, "zig"))

    # SDL2 (cabeçalhos, bibliotecas de ligação e a DLL)
    sdl = o["sdl"][0]
    for sub in ("include", "lib", "bin"):
        copiar_arvore(os.path.join(sdl, sub), os.path.join(fer, "sdl2", sub), ("lib/cmake", "lib/pkgconfig"))
    for cand in glob.glob(os.path.join(os.path.dirname(sdl), "LICENSE*")) + glob.glob(os.path.join(sdl, "share", "licenses", "*", "*")):
        shutil.copy2(cand, os.path.join(lic, "SDL2.txt"))

    # o launcher (só código do projeto: launcher, opções, conferência da ROM)
    fonte = os.path.join(fer, "fonte")
    srcs = [os.path.join(fonte, "runtime", n + ".c") for n in ("launcher", "launcher_sdl", "launcher_main", "config", "platform", "rom")]
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(tmp, "zc"), ZIG_LOCAL_CACHE_DIR=os.path.join(tmp, "zc"))
    zh = o["zig-host"][0]
    res = os.path.join(tmp, "launcher.res")
    subprocess.run([zh, "rc", "/c", "65001", "ie3.rc", res], check=True, cwd=os.path.join(fonte, "win"), env=env)
    sdlr = os.path.join(fer, "sdl2")
    exe = os.path.join(top, "Inazuma Eleven 3.exe")
    subprocess.run([zh, "cc", "-target", "x86_64-windows-gnu", "-O2", "-w", "-DHAVE_SDL2", "-Dmain=SDL_main",
                    "-I" + os.path.join(fonte, "runtime"), "-I" + os.path.join(sdlr, "include"), "-I" + os.path.join(sdlr, "include", "SDL2"),
                    *srcs, res, "-o", exe, "-L" + os.path.join(sdlr, "lib"), "-lmingw32", "-lSDL2main",
                    os.path.join(sdlr, "lib", "libSDL2.dll.a"), "-lcomdlg32", "-Wl,--subsystem,windows", "-s"], check=True, env=env)
    for pdb in glob.glob(os.path.join(top, "*.pdb")):
        os.remove(pdb)
    shutil.copy2(os.path.join(sdlr, "bin", "SDL2.dll"), top)

    # Biblioteca C do Windows já pronta (ferramentas/crt). Sem ela o zig monta o MinGW-w64 e o
    # compiler_rt no PC do usuário na primeira ligação, e no Windows isso chegou a travar.
    # Liga um programa vazio aqui, lê a linha do lld-link (ZIG_VERBOSE_LINK) e guarda os .obj/.lib
    # que vieram do cache do zig; o preparar.py liga o jogo chamando "zig lld-link" com eles.
    crt = os.path.join(fer, "crt")
    os.makedirs(crt)
    vazio = os.path.join(tmp, "vazio.c")
    with open(vazio, "w") as f:
        f.write("int main(void) { return 0; }\n")
    r = subprocess.run([zh, "cc", "-target", "x86_64-windows-gnu", "-O1", vazio, "-o", os.path.join(tmp, "vazio.exe"),
                        "-lcomdlg32", "-Wl,--subsystem,windows"], env=dict(env, ZIG_VERBOSE_LINK="1"),
                       capture_output=True, text=True, check=True)
    linha = [l for l in r.stderr.splitlines() if l.startswith("lld-link ")][-1].split()
    zc = os.path.join(tmp, "zc")
    libs = []
    for a in linha:
        p = a if os.path.isabs(a) else os.path.join(os.path.dirname(vazio), a)
        if a.endswith((".obj", ".lib")) and os.path.realpath(p).startswith(os.path.realpath(zc)) and not a.endswith("vazio.obj"):
            nome = os.path.basename(a)
            assert nome not in libs, nome
            shutil.copy2(p, os.path.join(crt, nome))
            libs.append(nome)
    assert "crt2.obj" in libs and "libmingw32.lib" in libs, libs
    with open(os.path.join(crt, "ligacao.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(libs) + "\n")
    shutil.copy2(res, os.path.join(crt, "ie3.res"))  # ícone e versão do ie3.exe (o zig rc não roda no PC do usuário)
    # recomplauncher.json da release: o RecompLauncher roda o preparo (prepare) e depois jogo/ie3.exe
    import json
    m = json.load(open(os.path.join(RAIZ, "recomplauncher.json"), encoding="utf-8"))
    v = versao.lstrip("v")
    m["start"]["program"] = ["jogo/ie3.exe"]
    m["prepare"] = {
        "program": ["ferramentas/python/python.exe"],
        "args": ["-X", "utf8", "-I", "{install}/ferramentas/fonte/tools/preparar.py", "{game}",
                 "--saida", "{install}/jogo", "--versao", v, "--rl"],
        "label": "Gerando o jogo a partir da sua ROM (alguns minutos, só na primeira vez)",
        "done": [f"{{install}}/jogo/pronto-{v}.txt"],
    }
    with open(os.path.join(top, "recomplauncher.json"), "w", encoding="utf-8") as f:
        json.dump(m, f, ensure_ascii=False, indent=2)
    with open(os.path.join(top, "LEIA-ME.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(LEIA.format(versao=versao))

    if os.path.exists(saida):
        os.remove(saida)
    with zipfile.ZipFile(saida, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for raiz, _, arqs in os.walk(top):
            for a2 in arqs:
                p = os.path.join(raiz, a2)
                zf.write(p, os.path.relpath(p, tmp))
    shutil.rmtree(tmp)
    print(f"ok: {saida} ({os.path.getsize(saida) // (1 << 20)} MB)")


if __name__ == "__main__":
    main()

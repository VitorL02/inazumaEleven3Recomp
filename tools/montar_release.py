#!/usr/bin/env python3
"""Monta o zip da release para Windows: ferramentas + código do projeto, SEM nada do jogo.

Uso: python3 -I tools/montar_release.py <versão> <saída.zip> --zig <ziglang win_amd64 .whl>
       --python <pasta com o Python do conda-forge para win-64 já descompactado>
       --pylibs <ndspy .whl> <crcmod .tar.gz> --sdl <pasta x86_64-w64-mingw32 do SDL2 devel mingw>

O usuário descompacta, arrasta a ROM em "Preparar jogo.bat" e o ie3.exe é gerado no PC dele
(tools/preparar.py). Assim a release não leva código do jogo, nem traduzido.
"""
import glob, io, os, shutil, sys, tarfile, tempfile, zipfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# o que vai do repositório para ferramentas/fonte (só código do projeto)
FONTE = ["runtime", "recomp", "win", "analysis/functions_list.txt", "analysis/extra_functions.txt",
         "tools/extract.py", "tools/preparar.py", "recomplauncher.json", "README.md"]
# partes do Python que o preparo não usa
PY_FORA = ["Lib/test", "Lib/idlelib", "Lib/tkinter", "Lib/turtledemo", "Lib/ensurepip", "Lib/lib2to3",
           "Lib/pydoc_data", "Lib/site-packages", "include", "libs", "Tools", "Scripts", "tcl", "etc",
           "Library/include", "Library/lib", "Library/share", "Library/ssl", "conda-meta", "info"]

BAT = r"""@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
echo Inazuma Eleven 3 Recomp - preparar o jogo
echo.
set "ROM=%~1"
if not "%ROM%"=="" goto tem_rom
echo Arraste o arquivo .nds da sua ROM para esta janela e aperte Enter:
set /p "ROM="
:tem_rom
set "ROM=%ROM:"=%"
"%~dp0ferramentas\python\python.exe" -X utf8 -I "%~dp0ferramentas\fonte\tools\preparar.py" "%ROM%"
if errorlevel 1 goto erro
echo.
echo Pronto! Abra o ie3.exe para jogar.
pause
exit /b 0
:erro
echo.
echo Não deu certo. Veja a mensagem acima (e o README em ferramentas\fonte).
pause
exit /b 1
"""

LEIA = """Inazuma Eleven 3 Recomp {versao} (versão de testes)

Esta pasta NÃO tem o jogo. Ela traz as ferramentas que geram o jogo a partir da SUA ROM.

1. Arraste a sua ROM (.nds) para cima de "Preparar jogo.bat"
   (ou abra o .bat e arraste a ROM para a janela).
   A ROM precisa ser Inazuma Eleven 3 - Sekai e no Chousen!! The Ogre (BOEJ) com a tradução BR T1.3.
2. Espere: ele confere a ROM, extrai o código, traduz para C e compila. Leva alguns minutos
   (depende do processador). Só precisa fazer isso uma vez.
3. Abra o ie3.exe.

O save (ie3.sav), as opções (ie3recomp.ini) e o log (ie3recomp.log) ficam nesta pasta.
A pasta "preparo" pode ser apagada depois: ela tem os arquivos intermediários gerados da sua ROM.
Não compartilhe o ie3.exe gerado nem a pasta "preparo": eles contêm o código do jogo.

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

    with open(os.path.join(top, "Preparar jogo.bat"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(BAT)
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

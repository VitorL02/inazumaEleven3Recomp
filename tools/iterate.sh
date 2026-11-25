#!/bin/sh
# Roda o protótipo; se ele achar uma função desconhecida, regenera, recompila e tenta de novo.
# Uso: tools/iterate.sh <rom.nds> [rodadas]
ROM="$1"; N="${2:-10}"
for i in $(seq 1 "$N"); do
  before=$(wc -l < analysis/extra_functions.txt 2>/dev/null || echo 0)
  RECOMP_WATCHDOG=${RECOMP_WATCHDOG:-20} ./build/ie3 "$ROM" --sem-janela > build/last_run.log 2>&1
  after=$(wc -l < analysis/extra_functions.txt 2>/dev/null || echo 0)
  if [ "$after" = "$before" ]; then echo "rodada $i: parou por outro motivo"; tail -20 build/last_run.log; exit 0; fi
  echo "rodada $i: nova função $(tail -1 analysis/extra_functions.txt)"
  make gen >/dev/null && make -j"$(nproc)" $MAKEFLAGS_EXTRA >/dev/null || exit 1
done

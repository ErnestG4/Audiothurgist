#!/usr/bin/env bash
# The boot-path check the desktop build cannot do: does any static initialiser
# store into SDRAM (0xC000_0000..) before main()? A default member initialiser
# or a constructor on an object in .sdram_bss does exactly that, and the module
# hard-faults before hw.Init() has configured the FMC (docs/sdk-quirks.md).
# Usage: tools/sdramcheck.sh [build/audiothurgist.elf]
set -u
ELF=${1:-build/audiothurgist.elf}
[ -f "$ELF" ] || { echo "no $ELF"; exit 2; }
# every function reachable from .init_array, by symbol
syms=$(arm-none-eabi-nm "$ELF" | awk '$3 ~ /^_GLOBAL__sub_I_|^_Z41__static_initialization/ {print $3}')
# literal-pool words in the SDRAM range → the object they point into
allow=$(grep -vE '^\s*(#|$)' "$(dirname "$0")/sdram-allow.txt" | tr '\n' ' ')
bad=0; seen=""
for s in $syms; do
  lits=$(arm-none-eabi-objdump -d "$ELF" --disassemble="$s" 2>/dev/null | grep -oiE "@ \(?[0-9a-f]{8}" | grep -oiE "[0-9a-f]{8}$" | sort -u)
  for lit in $lits; do
    # the literal is a pool ADDRESS; read the word it holds
    word=$(arm-none-eabi-objdump -s --start-address=0x$lit --stop-address=$((0x$lit + 4)) "$ELF" | tail -1 | awk '{print $2}')
    [ -n "$word" ] || continue
    le=$(echo "$word" | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')
    case "$le" in c[0-3]*) ;; *) continue;; esac
    obj=$(arm-none-eabi-nm -n "$ELF" | awk -v a="$le" 'BEGIN{t=strtonum("0x" a)} { v=strtonum("0x"$1); if (v<=t && $3!="_ssdram_bss") last=$3; if (v>t) {print last; exit} }' | sed 's/^_ZL[0-9]*//')
    seen="$seen $obj"
    case " $allow " in *" $obj "*) echo "sdramcheck: $obj is written by a static initialiser (allowed, see tools/sdram-allow.txt)";;
      *) echo "sdramcheck: FAIL — $obj in .sdram_bss is written by static initialiser $s (add a reason to tools/sdram-allow.txt or make it a POD)"; bad=1;; esac
  done
done
n=$(echo "$syms" | grep -c . || true)
[ $bad = 0 ] && echo "sdramcheck: $n static initialisers checked, nothing unexpected touches SDRAM" || exit 1

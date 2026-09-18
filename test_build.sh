#!/usr/bin/env bash
# Build test (no radio / no donor needed): builds both variants and asserts each .bin is large
# enough to receive the AMBE codec merge. Optionally validates a donor firmware offline.
#
# Usage:  ./test_build.sh [/path/to/MD9600-CSV2571V5-V26.45.bin]
set -euo pipefail
cd "$(dirname "$0")/MDUV380_firmware"
MERGE_MIN=$(( 0x6937C + 0x48BB0 ))   # 728876 = codec write offset + codec size
ok=1
# 2026-09-18: раніше `make clean`/`make` глушились через >/dev/null, тож при падінні
# збірки в консолі не було ЖОДНОГО рядка помилки -- лише "[FAIL: too small for codec
# merge]" (бо build/*.bin лишався від попередньої успішної збірки чи не з'являвся
# взагалі, і stat падав окремо, теж без пояснення). Тепер вивід кожної збірки пишеться
# в лог-файл, а при невдалому `make` (ненульовий код виходу) друкується його хвіст --
# щоб причину було видно одразу, а не перезапускати вручну без >/dev/null.
LOG_DIR="$(mktemp -d)"
trap 'rm -rf "$LOG_DIR"' EXIT
for AES in 0 1; do
  log="$LOG_DIR/make_aes${AES}.log"
  make clean >"$log" 2>&1
  if ! make -j"$(nproc)" ENABLE_AES="$AES" >>"$log" 2>&1; then
    echo "ENABLE_AES=$AES -> ЗБІРКА ВПАЛА. Останні рядки $log:"
    tail -n 40 "$log"
    ok=0
    continue
  fi
  sz=$(stat -c%s build/openuv380-10w.bin)
  printf "ENABLE_AES=%s -> %s bytes  " "$AES" "$sz"
  if [ "$sz" -ge "$MERGE_MIN" ]; then echo "[merge-capable]"; else echo "[FAIL: too small for codec merge]"; ok=0; fi
done
[ "$ok" = 1 ] && echo "PASS: builds are codec-merge-capable." || { echo "BUILD TEST FAILED"; exit 1; }
DONOR="${1:-}"
if [ -n "$DONOR" ] && [ -f "$DONOR" ]; then
  echo ">> Validating donor offline ..."
  python3 tools/verify_codec_donor.py "$DONOR"
fi

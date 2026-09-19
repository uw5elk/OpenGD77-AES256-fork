#!/usr/bin/env bash
#
# Збирає й запускає ВСІ хостові тести. Без рації, без ARM-тулчейну -- звичайний gcc.
#
#     MDUV380_firmware/tests/run_tests.sh          # усі
#     MDUV380_firmware/tests/run_tests.sh rctl     # лише ті, чия назва містить "rctl"
#
# НАВІЩО ЦЕЙ ФАЙЛ. Тести тут лежали давно, але рядки збірки були розкидані по
# коментарях у самих тестах, у кожного свій набір -I та своїх файлів. Через це їх
# фактично ніхто не ганяв, і в CI вони не потрапляли.
#
# Гірше: при ручному запуску легко ОБМАНУТИ САМОГО СЕБЕ. 2026-09-04 я прогнав тест
# однорядковим циклом, компіляція впала через брак -I, але бінарник із попереднього
# запуску лишався в /tmp -- і цикл радісно надрукував "ПРОЙДЕНО". Тому тут:
#   * кожен тест збирається у СВІЙ свіжий каталог, який спершу видаляється;
#   * помилка компіляції -- це провал тесту, а не "пропустимо";
#   * ненульовий код виходу самого тесту -- теж провал;
#   * наприкінці друкується підсумок, і скрипт повертає 1, якщо впав хоч один.
set -u

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$TESTS_DIR/../application"
INC=(-I "$APP/include" -I "$APP/include/crypto")
# -DENABLE_CALL_REPLAY -- глобально, як і AES/DMR_DATA вище: лише test_call_replay
# лінкує functions/callReplay.c (порожній translation unit без цього прапорця), решті
# тестів прапорець просто не зустрічається -- нешкідливо додати його для всіх.
CFLAGS=(-O2 -Wall -Wextra -std=gnu11 -DENABLE_AES -DENABLE_DMR_DATA -DENABLE_CALL_REPLAY)
BUILD="$TESTS_DIR/.build"

CC="${CC:-gcc}"
FILTER="${1:-}"

# назва -> додаткові файли для лінкування (крім самого тесту)
declare -A SRCS=(
	[test_dmr_aes]="$APP/source/crypto/dmr_aes.c"
	[test_dmr_rctl_pdu]="$APP/source/crypto/dmr_rctl_pdu.c"
	[test_dmr_rctl_frame]="$APP/source/crypto/dmr_rctl_frame.c $APP/source/crypto/dmr_rctl_pdu.c $APP/source/crypto/dmr_aes.c"
	[test_dmr_rctl_cfg]="$TESTS_DIR/mock_codeplug.c $APP/source/functions/dmr_rctl_cfg.c $APP/source/crypto/dmr_rctl_pdu.c"
	[test_dmr_rctl_cap]="$APP/source/functions/dmr_rctl_cap.c"
	[test_dmr_rctl_stock]="$APP/source/crypto/dmr_rctl_stock.c"
	[test_emb_sb]="$APP/source/crypto/dmr_aes.c"
	[test_le_mi]="$APP/source/crypto/dmr_aes.c"
	[test_dmr_sms_ack]=""   # самодостатній: контракт формату SMS-квитанції, без чужих джерел
	[test_call_replay]="$APP/source/functions/callReplay.c"   # лише ЧИСТА кільцева логіка -- callReplayPlayback.c (codec/sound/trx/FreeRTOS) сюди навмисно не тягнеться
)

rm -rf "$BUILD" && mkdir -p "$BUILD"

pass=0; fail=0
for src in "$TESTS_DIR"/test_*.c; do
	name="$(basename "$src" .c)"
	if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then continue; fi

	if [ -z "${SRCS[$name]+x}" ]; then
		echo "ПРОПУЩЕНО $name -- немає запису в таблиці SRCS цього скрипта"
		continue
	fi
	extra="${SRCS[$name]}"   # може бути порожнім -> самодостатній тест (лінкуємо лише сам файл)

	bin="$BUILD/$name"
	# shellcheck disable=SC2086
	if ! out="$("$CC" "${CFLAGS[@]}" "${INC[@]}" -o "$bin" "$src" $extra 2>&1)"; then
		fail=$((fail + 1))
		echo "ЗБІРКА ВПАЛА  $name"
		printf '%s\n' "$out" | head -12
		continue
	fi

	if out="$("$bin" 2>&1)"; then
		pass=$((pass + 1))
		echo "ok            $name"
	else
		fail=$((fail + 1))
		echo "ПРОВАЛ        $name"
		printf '%s\n' "$out" | tail -20
	fi
done

echo
echo "Тестів пройдено: $pass, провалено: $fail"
[ "$fail" -eq 0 ] || exit 1
exit 0

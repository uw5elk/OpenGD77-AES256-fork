#!/usr/bin/env bash
#
# Швидка СИНТАКСИЧНА перевірка коду прошивки звичайним хостовим gcc (x86), без
# ARM-тулчейну. Запускати з будь-якої теки:
#
#     MDUV380_firmware/tools/syntax_check.sh                 # усі файли application/source
#     MDUV380_firmware/tools/syntax_check.sh path/to/file.c  # лише вказані файли
#
# НАВІЩО ЦЕ ІСНУЄ
# ---------------
# Двічі поспіль (коміти 8aab1f5 і fd1ef9a) збірка CI падала через помилку, яку не
# можна побачити ані оком, ані перевіркою балансу дужок: у коментарі rctl_ua.h був
# текст "RCFG_*/RCTL_*", де послідовність "*/" передчасно ЗАКРИВАЛА блочний коментар,
# і компілятор переставав бачити цілий enum у файлі, що його підключав. Такі речі
# ловить тільки справжній прохід компілятора.
#
# Виявилось, що для цього НЕ потрібен ARM-тулчейн: CMSIS, HAL і FreeRTOS вендеровані
# прямо в цьому репозиторії, тож хостовому gcc бракує рівно одного заголовка з newlib
# (<reent.h>) -- і той підкладається заглушкою з tools/hoststub/. Прохід по всіх ~90
# файлах application/source займає секунди й не потребує нічого, крім gcc.
#
# МЕЖІ ЗАСТОСУВАННЯ -- ЧИТАТИ ОБОВ'ЯЗКОВО
# ---------------------------------------
# Це НЕ заміна справжній збірці. Тут -fsyntax-only і хостовий x86-gcc, тому:
#   * НЕ генерується код, не лінкується, не рахується розмір прошивки;
#   * розміри/вирівнювання типів і поведінка, залежна від цілі, ІНШІ, ніж на Cortex-M4;
#   * помилки лінкування (напр. відсутнє визначення функції) тут не видно.
# Перевіряє: синтаксис, оголошення/типи, помилки в коментарях, аргументи функцій,
# формати printf тощо -- тобто рівно той клас багів, що ламав CI.
# Остаточне слово завжди за збіркою arm-none-eabi-gcc у GitHub Actions.
#
# Прапорці збірки нижче навмисно збігаються з роботою "build (українська,
# ENABLE_AES=1 ENABLE_DMR_DATA=1)" у .github/workflows/build.yml -- саме та
# конфігурація, яку ми прошиваємо, і саме та, що падала.

set -u

FW_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$FW_DIR" || exit 1

CC="${CC:-gcc}"

if ! command -v "$CC" >/dev/null 2>&1; then
	echo "ПОМИЛКА: не знайдено компілятор '$CC'." >&2
	exit 2
fi

INCS=(
	-Itools/hoststub                 # заглушка <reent.h>, див. коментар у тому файлі
	-include tools/hoststub/host_compat.h  # прототипи newlib, яких немає в glibc (itoa)
	-I. -ICore/Inc
	-IDrivers/CMSIS/Device/ST/STM32F4xx/Include -IDrivers/CMSIS/Include
	-IDrivers/STM32F4xx_HAL_Driver/Inc -IDrivers/STM32F4xx_HAL_Driver/Inc/Legacy
	-IMiddlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
	-IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc
	-IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
	-IMiddlewares/Third_Party/FreeRTOS/Source/include
	-IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
	-IUSB_DEVICE/App -IUSB_DEVICE/Target -Iapplication/include
)

DEFS=(
	-DUSE_HAL_DRIVER -DSTM32F405xx
	-DPLATFORM_MDUV380 -DPLATFORM_VARIANT_UV380_PLUS_10W -DNDEBUG
	-DENABLE_AES -DENABLE_DMR_DATA -DLANGUAGE_BUILD_UKRAINIAN
	-DGITVERSION=syntaxcheck
)

# Щоб побачити попередження ВЛАСНОГО коду, запускай: SHOW_WARNINGS=1 ...
# Три класи помилок, які МАЮТЬ валити перевірку.
#
# Навіщо: 2026-09-04 ми прибрали небезпечний codeplugGetOpenGD77CustomData() з розрахунку
# "якщо злиття з upstream поверне виклик, збірка впаде". Негативний тест показав, що НЕ
# впала б: у C неявний виклик невідомої функції -- лише попередження, а -w його ковтав.
# Тобто перевірка мовчки пропускала б рівно той клас помилок, заради якого функцію й
# прибрали.
#
# ВАЖЛИВО: -w перебиває -Werror=... НЕЗАЛЕЖНО ВІД ПОРЯДКУ прапорців (перевірено на gcc
# 13.3: `gcc -w -Werror=implicit-function-declaration` мовчить). Тому в режимі за
# замовчуванням -w більше не використовується взагалі. Шум вендерованих HAL/CMSIS однаково
# не видно: цикл нижче друкує вивід лише коли там є 'error:' або коли SHOW_WARNINGS=1.
#   implicit-function-declaration -- виклик функції, якої компілятор не бачив;
#   implicit-int                  -- оголошення без типу;
#   int-conversion                -- мовчазна конверсія вказівник <-> ціле.
WARN_FLAGS=()
if [ "${SHOW_WARNINGS:-0}" = "1" ]; then
	WARN_FLAGS=(-Wall)
fi
WARN_FLAGS+=(-Werror=implicit-function-declaration -Werror=implicit-int -Werror=int-conversion)

if [ "$#" -gt 0 ]; then
	FILES=("$@")
else
	mapfile -t FILES < <(find application/source -name '*.c' | sort)
fi

failed=0
total=0

for f in "${FILES[@]}"; do
	total=$((total + 1))
	out="$("$CC" -fsyntax-only -std=gnu11 "${WARN_FLAGS[@]}" "${INCS[@]}" "${DEFS[@]}" "$f" 2>&1)"
	if printf '%s' "$out" | grep -q 'error:'; then
		failed=$((failed + 1))
		echo "=== ПОМИЛКА: $f"
		printf '%s\n' "$out" | grep -A 2 'error:' | head -20
	elif [ -n "$out" ] && [ "${SHOW_WARNINGS:-0}" = "1" ]; then
		echo "--- попередження: $f"
		printf '%s\n' "$out" | head -20
	fi
done

echo
echo "Перевірено файлів: $total, з помилками: $failed"

if [ "$failed" -ne 0 ]; then
	echo "СИНТАКСИЧНА ПЕРЕВІРКА НЕ ПРОЙДЕНА -- ці помилки зламають і збірку CI."
	exit 1
fi

echo "Синтаксична перевірка пройдена (нагадування: це не заміна ARM-збірці в CI)."
exit 0

/*
 * Заглушки для розбіжностей МІЖ БІБЛІОТЕКАМИ, а не для коду прошивки.
 *
 * Підключається примусово через -include у tools/syntax_check.sh, тобто раніше за будь-який
 * заголовок файлу, що перевіряється.
 *
 * НАВІЩО. Прошивка збирається arm-none-eabi-gcc із newlib, а перевірка -- хостовим gcc із
 * glibc. Кілька функцій є в newlib і немає в glibc. Доки в syntax_check.sh стояв -w, така
 * розбіжність тонула у мовчазних попередженнях; коли (2026-09-04) неявний виклик підвищили
 * до помилки, вилізли рівно ці випадки. Це НЕ помилки прошивки: на цільовому компіляторі
 * прототипи є.
 *
 * Сюди можна додавати ЛИШЕ прототипи функцій, які реально надає newlib. Якщо якоїсь функції
 * немає й там -- це справжня помилка, і глушити її тут не можна.
 */
#ifndef TOOLS_HOSTSTUB_HOST_COMPAT_H_
#define TOOLS_HOSTSTUB_HOST_COMPAT_H_

/* itoa(): є в newlib (stdlib.h), немає в стандарті C і в glibc.
 * Використовується в application/source: voicePrompts.c, menuContactDetails.c,
 * uiTxTgPcContactOwnId.c, uiUtilities.c. */
char *itoa(int value, char *str, int base);

#endif /* TOOLS_HOSTSTUB_HOST_COMPAT_H_ */

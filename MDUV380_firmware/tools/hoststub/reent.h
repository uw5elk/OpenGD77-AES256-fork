/*
 * Мінімальна заглушка newlib-заголовка <reent.h> -- ЛИШЕ для швидкої синтаксичної
 * перевірки прошивки хостовим (x86) gcc через tools/syntax_check.sh.
 *
 * Навіщо: FreeRTOS.h робить #include <reent.h> під configUSE_NEWLIB_REENTRANT, і цей
 * заголовок є в newlib, який іде з arm-none-eabi-gcc, але його немає в системному glibc
 * на звичайній Linux-машині. Це єдине, чого бракує хостовому gcc, щоб розібрати весь
 * код прошивки -- решта (CMSIS, HAL, FreeRTOS) вендорена прямо в цьому репозиторії.
 *
 * struct _reent тут потрібна лише як тип поля-заглушки в TCB (FreeRTOS.h, xDummy17),
 * тому справжня розкладка полів не має значення: -fsyntax-only не генерує коду і
 * ніколи не покладається на розмір цієї структури.
 *
 * ЦЕЙ ФАЙЛ НЕ БЕРЕ УЧАСТІ У ЗБІРЦІ ПРОШИВКИ. Makefile його не бачить -- справжня
 * збірка йде arm-none-eabi-gcc зі справжнім newlib.
 */
#ifndef _REENT_H_HOST_SYNTAX_STUB_
#define _REENT_H_HOST_SYNTAX_STUB_

struct _reent
{
	long _stub_dummy[64];
};

#endif /* _REENT_H_HOST_SYNTAX_STUB_ */

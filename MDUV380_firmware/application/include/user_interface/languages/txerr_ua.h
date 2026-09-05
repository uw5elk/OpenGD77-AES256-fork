/*
 * txerr_ua.h -- рядки екрана невдалої передачі (uiTxScreen.c) українською.
 *
 * ВАЖЛИВО: файл у кодуванні cp1251, як ukrainian.h/messages_ua.h/rctl_ua.h -- саме так
 * шрифт HX8353E_charset_UA.h трактує коди 192-255. Не зберігати в UTF-8.
 *
 * Чому окремим файлом, а не полями в stringsTable_t: цю таблицю читає ще й CPS, і
 * додавання полів ламає сумісність мовних файлів. Той самий підхід, що вже застосований
 * для SMS, RCTL, ключів і гасел.
 *
 * Ширина: FONT_SIZE_3 -- 8 px на символ, екран 160 px => максимум 20 символів.
 */
#ifndef USER_INTERFACE_LANGUAGES_TXERR_UA_H_
#define USER_INTERFACE_LANGUAGES_TXERR_UA_H_

/* Ретранслятор не відповів на серію запитів пробудження (WAKING_MODE_FAILED).
 * Це НЕ той самий стан, що TIMEOUT таймера передачі, хоч раніше показувався той самий
 * напис. Найчастіша причина -- рація поза зоною дії ретранслятора. */
#define TXERR_RPT_LINE1          "Ретранслятор"
#define TXERR_RPT_LINE2          "не відповідає"
#define TXERR_RPT_HINT           "поза зоною дії?"


/* Рацію дистанційно заблоковано (RCTL Disable/stun). Передача заборонена, доки
 * не прийде Enable по ефіру або зняття кабелем (rctl_capture.py --unlock). */
#define TXERR_INHIBIT_LINE1      "ЗАБЛОКОВАНО"
#define TXERR_INHIBIT_LINE2      "дистанційно"
#define TXERR_INHIBIT_HINT       "Enable або кабель"

#endif /* USER_INTERFACE_LANGUAGES_TXERR_UA_H_ */

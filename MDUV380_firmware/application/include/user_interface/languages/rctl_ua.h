/*
 * rctl_ua.h — рядки екранів "RCTL access" (menuRCTLConfig.c) та "Remote control"
 * (menuRCTLRemote.c) українською.
 *
 * ВАЖЛИВО: цей файл, як і messages_ua.h/ukrainian.h, має кодування cp1251 -- саме
 * так шрифт HX8353E_charset_UA.h трактує коди 192-255. Не зберігати в UTF-8.
 *
 * Той самий підхід перемикання на етапі компіляції, що й messages_ua.h: ці рядки не
 * в stringsTable_t (лише назви двох пунктів меню там є, RCFG_* та RCTL_* -- це вміст
 * самих екранів), щоб не роздувати мовний файл, який читає ще й CPS.
 */
#ifndef USER_INTERFACE_LANGUAGES_RCTL_UA_H_
#define USER_INTERFACE_LANGUAGES_RCTL_UA_H_

/* ---- menuRCTLConfig.c: Options > RCTL access (увімк/вимк приймання команд) ---- */
#define RCFG_TITLE               "Доступ RCTL"
/* Підказка внизу екрана. ДВА рядки, і кожен не довший за 26 символів: FONT_SIZE_1 --
 * це 6 пікселів на символ, екран 160 пікселів, а displayPrintCore обрізає надлишок
 * МОВЧКИ (HX8353E_display.c: sLen = (DISPLAY_SIZE_X - xPos) / charWidthPixels).
 * Попередній однорядковий варіант мав 34 символи, тобто на екрані було видно
 * "L/R:зміна GRN:зберегти RED" -- саме та частина, що пояснює скасування, і зникала. */
#define RCFG_HINT                "L/R:зміна  GRN:зберегти"
#define RCFG_HINT2               "RED:вийти без збереження"

/* Пункти екрана дозволів. Бюджет рядка -- 16 символів разом із ":" і значенням
 * ("On"/"Off"), тож найдовший підпис тут 10 символів. */
#define RCFG_ITEM_ACCESS         "Доступ"
#define RCFG_ITEM_CHECK          "Перевірка"
#define RCFG_ITEM_MONITOR        "Моніторинг"
#define RCFG_ITEM_STUN           "Вимкнення"
#define RCFG_ITEM_REVIVE         "Ввімкнення"

/* ---- menuRCTLRemote.c: "Від. керування" (запит Radio Check іншій рації) ---- */
#define RCTL_TITLE               "Від. керування"
#define RCTL_PICK_CMD_TITLE      "Оберіть команду"
#define RCTL_HINT_PICK_CMD       "U/D  GRN:далі  RED:вихід"
#define RCTL_SENT                "Надіслано"
#define RCTL_ID_FMT              "ID: %s"
#define RCTL_HINT_ID             "0-9:id  L:стерти"
#define RCTL_HINT_CONTACTS       "SK1:контакти"
#define RCTL_HINT_SEND_BACK      "GRN:надіслати RED:назад"
#define RCTL_TITLE_PICK          "Обрати контакт"
#define RCTL_NO_CONTACTS         "(немає контактів)"
#define RCTL_HINT_BACK           "RED:назад"
#define RCTL_WAITING             "Перевірка..."
#define RCTL_HINT_CANCEL         "RED:скасувати"
#define RCTL_ACK_OK              "Рація на зв'язку"
#define RCTL_ACK_AGE_FMT         "ID %lu, %lu с тому"
#define RCTL_TIMEOUT             "Немає відповіді"
#define RCTL_HINT_ANY_KEY        "будь-яка клавіша: назад"
#define RCTL_ERR_GENERIC         "помилка"
#define RCTL_ERR_TX_BUSY         "канал зайнятий"

/* Спливаюче сповіщення в ЗАПИТУВАЧА (на цільовій рації радіоперевірка тиха).
 * Раніше цей текст був написаний прямо в dmr_rctl_tx.c у
 * UTF-8 -- і на екрані виходила каша, бо шрифт рації індексується байтом cp1251.
 * Місце українського тексту -- тут. */
#define RCTL_NOTE_ACK_FMT        "Радіоперевірка: ID %lu"
#define RCTL_ERR_NO_KEY          "немає AES-ключа"
#define RCTL_ERR_FRAME           "збірка кадру не вдалась"
/* 18 символів: FONT_SIZE_3 -- 8 px на символ, екран 160 px, межа 20 символів.
 * Попередній варіант ("Надсилання не вдалось", 21 символ) на рації обрізався до
 * "Надсилання не вдалос". */
#define RCTL_SEND_FAILED         "Помилка надсилання"
#define RCTL_FAIL_FMT            "%s (код %d)"

#endif

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
#define RCFG_STATE_FMT           "Стан: %s"
#define RCFG_HINT                "L/R:зміна GRN:зберегти RED:відміна"

/* ---- menuRCTLRemote.c: "Від. керування" (запит Radio Check іншій рації) ---- */
#define RCTL_TITLE               "Від. керування"
#define RCTL_ID_FMT              "ID: %s"
#define RCTL_HINT_ID             "0-9:id  L:стерти"
#define RCTL_HINT_CONTACTS       "SK1:контакти"
#define RCTL_HINT_SEND_BACK      "GRN:перевірити RED:назад"
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
#define RCTL_ERR_NO_KEY          "немає AES-ключа"
#define RCTL_ERR_FRAME           "збірка кадру не вдалась"
#define RCTL_SEND_FAILED         "Надсилання не вдалось"
#define RCTL_FAIL_FMT            "%s (код %d)"

#endif

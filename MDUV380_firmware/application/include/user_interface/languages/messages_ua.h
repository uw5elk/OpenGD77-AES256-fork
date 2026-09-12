/*
 * Рядки екрана SMS-повідомлень українською.
 *
 * ВАЖЛИВО: цей файл, як і ukrainian.h, має кодування cp1251 -- саме так шрифт
 * HX8353E_charset_UA.h трактує коди 192-255. Не зберігати в UTF-8.
 *
 * Чому окремим файлом, а не полями у stringsTable_t: SMS -- функція саме цього
 * форку, а таблицю рядків читає ще й мовний файл, який пише CPS. Три десятки
 * нових полів зламали б ту сумісність і з'їли б близько кілобайта флеша в КОЖНІЙ
 * збірці, зокрема неукраїнській. Тут перемикання відбувається на етапі компіляції.
 */
#ifndef USER_INTERFACE_LANGUAGES_MESSAGES_UA_H_
#define USER_INTERFACE_LANGUAGES_MESSAGES_UA_H_

#define MSGS_TITLE               "Повідомлення"
#define MSGS_INBOX_FMT           "Вхідні (%d)"
#define MSGS_SENT_FMT            "Надіслані (%d)"
#define MSGS_NEW                 "Нове повідомлення"
#define MSGS_TITLE_SENT          "Надіслані"
#define MSGS_TITLE_INBOX         "Вхідні"
#define MSGS_EMPTY               "(порожньо)"
#define MSGS_HINT_BACK           "RED:назад"
#define MSGS_DELETE_ALL          "[Видалити все]"
#define MSGS_TO                  "Кому"
#define MSGS_FROM                "Від"
#define MSGS_HINT_SENT_ITEM      "SK2+GRN:вид GRN:ще раз"
#define MSGS_HINT_INBOX_ITEM     "SK2+GRN:вид GRN:відпов"
#define MSGS_TITLE_NEW           "Нове повідомлення"
#define MSGS_CHARS_FMT           "%d/%d симв."
#define MSGS_HINT_PRESET         "U:шаблон  L:стерти"
#define MSGS_HINT_TO_CANCEL      "GRN:кому  RED:відміна"
#define MSGS_TITLE_RCPT          "Одержувач"
#define MSGS_TO_FMT              "Кому: %s"
#define MSGS_TYPE_FMT            "Тип: %s"
#define MSGS_GROUP               "Група"
#define MSGS_PRIVATE             "Приватний"
#define MSGS_HINT_ID             "0-9:id  L:стерти"
#define MSGS_HINT_GRP_PRIV       "U/D:група/приват"
#define MSGS_HINT_CONTACTS       "SK1:контакти"
#define MSGS_HINT_SEND_BACK      "GRN:надіслати  RED:назад"
#define MSGS_TITLE_PICK_TG       "Вибір TG контакту"
#define MSGS_TITLE_PICK_PC       "Вибір PC контакту"
#define MSGS_NO_CONTACTS         "(немає контактів)"
#define MSGS_SENT_OK             "Надіслано"
#define MSGS_KEYED_TX            "(вихід в ефір)"
#define MSGS_SEND_FAILED         "Не надіслано"
#define MSGS_ERR_GENERIC         "помилка"
#define MSGS_ERR_TX_BUSY         "TX зайнятий"
#define MSGS_ERR_BAD_TEXT        "хибний текст"
#define MSGS_ERR_TOO_LONG        "задовгий текст"
#define MSGS_FAIL_FMT            "%s (%d)"
#define MSGS_HINT_ANY_KEY        "будь-яка клавіша: назад"
#define MSGS_MONITOR_FMT         "Монітор: %s"
#define MSGS_MON_ON              "увімк"
#define MSGS_MON_OFF             "вимк"
/* Банер вхідного: скільки ще НЕпрочитаних, крім показаного (окремим кольором). */
#define MSGS_MORE_UNREAD_FMT     "ще %d непрочит."
#define MSGS_FOREIGN             " (не вам)"

#endif /* USER_INTERFACE_LANGUAGES_MESSAGES_UA_H_ */

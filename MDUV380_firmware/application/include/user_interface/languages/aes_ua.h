/*
 * Тексти сповіщень AES українською.
 *
 * ВАЖЛИВО: файл у кодуванні cp1251, як ukrainian.h і messages_ua.h -- саме так шрифт
 * трактує коди 192-255. Не зберігати в UTF-8.
 *
 * Окремим файлом, а не полями stringsTable_t: це два рядки суто нашого форку, а таблицю
 * читає ще й мовний файл від CPS (див. той самий підхід у messages_ua.h).
 */
#ifndef USER_INTERFACE_LANGUAGES_AES_UA_H_
#define USER_INTERFACE_LANGUAGES_AES_UA_H_

#define AES_STR_NO_KEY      "Немає ключа %u"   // ключ, названий у виклику, у нас не завантажений
#define AES_STR_WRONG_KEY   "Інший ключ: %u"   // канал налаштований на інший слот

#endif /* USER_INTERFACE_LANGUAGES_AES_UA_H_ */

/* -*- coding: binary; -*- */
/*
 * Copyright (C) 2019-2025 Roger Clark, VK3KYY / G4KYF
 *
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
/*
 * РџРµСЂРµРєР»Р°РґР°С‡С– (Translators): uw5elk (OpenGD77-AES256-fork), Р·Р° РґРѕРїРѕРјРѕРіРѕСЋ Claude (Anthropic)
 *
 * Р”Р¶РµСЂРµР»Рѕ РєРёСЂРёР»РёС†С– (font glyphs): СЂРµР°Р»СЊРЅРёР№, РїРµСЂРµРІС–СЂРµРЅРёР№ РЅР° Р·Р°Р»С–Р·С– С€СЂРёС„С‚ Р· С„РѕСЂРєСѓ tytreversing/OpenGD77-alternative (РґРёРІ. HX8353E_charset_UA.h)
 *
 * Rev: 1
 */
#ifndef USER_INTERFACE_LANGUAGES_UKRAINIAN_H_
#define USER_INTERFACE_LANGUAGES_UKRAINIAN_H_
/********************************************************************
 *
 * VERY IMPORTANT.
 * This file should not be saved with UTF-8 encoding
 * Use Notepad++ on Windows with ANSI encoding
 * or emacs on Linux with binary encoding
 *
 ********************************************************************/
#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
__attribute__((section(".upper_text")))
#endif
const stringsTable_t ukrainianLanguage =
{
.magicNumber                            = { LANGUAGE_TAG_MAGIC_NUMBER, LANGUAGE_TAG_VERSION },
.LANGUAGE_NAME                           = "Українська", // MaxLen: 16
.menu                                    = "Меню", // MaxLen: 16
.credits                                 = "Автори", // MaxLen: 16
.zone                                    = "Зона", // MaxLen: 16
.rssi                                    = "RSSI", // MaxLen: 16
.battery                                 = "Батарея", // MaxLen: 16
.contacts                                = "Контакти", // MaxLen: 16
.last_heard                              = "Останні виклики", // MaxLen: 16
.firmware_info                           = "Про прошивку", // MaxLen: 16
.options                                 = "Налаштування", // MaxLen: 16
.display_options                         = "Дисплей", // MaxLen: 16
.sound_options                           = "Звук", // MaxLen: 16
.channel_details                         = "Канал", // MaxLen: 16
.language                                = "Мова", // MaxLen: 16
.new_contact                             = "Новий контакт", // MaxLen: 16
.dmr_contacts                            = "DMR контакти", // MaxLen: 16
.contact_details                         = "Про контакт", // MaxLen: 16
.hotspot_mode                            = "Hotspot", // MaxLen: 16
.built                                   = "Зібрано", // MaxLen: 16
.zones                                   = "Зони", // MaxLen: 16
.keypad                                  = "Кнопки", // MaxLen: 16
.ptt                                     = "PTT", // MaxLen: 16
.locked                                  = "Заблоковано", // MaxLen: 16
.press_sk2_plus_star                     = "SK2 + *", // MaxLen: 16
.to_unlock                               = "щоб відкрити", // MaxLen: 16
.unlocked                                = "Розблоковано", // MaxLen: 16
.power_off                               = "Вимкнення...", // MaxLen: 16
.error                                   = "ПОМИЛКА", // MaxLen: 16
.rx_only                                 = "Тільки RX", // MaxLen: 16
.out_of_band                             = "ПОЗА СМУГОЮ", // MaxLen: 16
.timeout                                 = "ТАЙМАУТ", // MaxLen: 16
.tg_entry                                = "Ввід TG", // MaxLen: 16
.pc_entry                                = "Ввід PC", // MaxLen: 16
.user_dmr_id                             = "Мій DMR ID", // MaxLen: 16
.contact                                 = "Контакт", // MaxLen: 16
.accept_call                             = "Відповісти", // MaxLen: 16
.private_call                            = "Приватний виклик", // MaxLen: 16
.squelch                                 = "Squelch", // MaxLen: 16
.quick_menu                              = "Швидке меню", // MaxLen: 16
.filter                                  = "Фільтр", // MaxLen: 16
.all_channels                            = "Всі канали", // MaxLen: 16
.gotoChannel                             = "До", // MaxLen: 16
.scan                                    = "Скан", // MaxLen: 16
.channelToVfo                            = "Канал --> VFO", // MaxLen: 16
.vfoToChannel                            = "VFO --> Канал", // MaxLen: 16
.vfoToNewChannel                         = "VFO --> Новий К", // MaxLen: 16
.group                                   = "Група", // MaxLen: 16
.private                                 = "Приватний", // MaxLen: 16
.all                                     = "Всі", // MaxLen: 16
.type                                    = "Тип", // MaxLen: 16
.timeSlot                                = "TS", // MaxLen: 16
.none                                    = "Немає", // MaxLen: 16
.contact_saved                           = "Контакт збереж.", // MaxLen: 16
.duplicate                               = "Дублікат", // MaxLen: 16
.tg                                      = "TG", // MaxLen: 16
.pc                                      = "PC", // MaxLen: 16
.ts                                      = "TS", // MaxLen: 16
.mode                                    = "Режим", // MaxLen: 16
.colour_code                             = "Color Code", // MaxLen: 16
.n_a                                     = "Н/Д", // MaxLen: 16
.bandwidth                               = "Смуга", // MaxLen: 16
.stepFreq                                = "Крок", // MaxLen: 16
.tot                                     = "TOT", // MaxLen: 16
.off                                     = "Off", // MaxLen: 16
.zone_skip                               = "Пропуск зони", // MaxLen: 16
.all_skip                                = "Пропуск всіх", // MaxLen: 16
.yes                                     = "Так", // MaxLen: 16
.no                                      = "Ні", // MaxLen: 16
.tg_list                                 = "Спис. TG", // MaxLen: 16
.on                                      = "On", // MaxLen: 16
.timeout_beep                            = "Тайм-аут", // MaxLen: 16
.list_full                               = "Список повний", // MaxLen: 16
.dmr_cc_scan                             = "Скан CC", // MaxLen: 16
.band_limits                             = "Межі діапазону", // MaxLen: 16
.beep_volume                             = "Гучність", // MaxLen: 16
.dmr_mic_gain                            = "Мікр. DMR", // MaxLen: 16
.fm_mic_gain                             = "Мікр. FM", // MaxLen: 16
.key_long                                = "Довге нат.", // MaxLen: 16
.key_repeat                              = "Повтор", // MaxLen: 16
.dmr_filter_timeout                      = "Час фільтра", // MaxLen: 16
.brightness                              = "Яскравість", // MaxLen: 16
.brightness_off                          = "Мін. яскр.", // MaxLen: 16
.contrast                                = "Контраст", // MaxLen: 16
.screen_invert                           = "Інверсія", // MaxLen: 16
.screen_normal                           = "Звичайний", // MaxLen: 16
.backlight_timeout                       = "Тайм-аут", // MaxLen: 16
.scan_delay                              = "Затр. скан.", // MaxLen: 16
.yes___in_uppercase                      = "ТАК", // MaxLen: 16
.no___in_uppercase                       = "НІ", // MaxLen: 16
.DISMISS                                 = "ЗАКРИТИ", // MaxLen: 16
.scan_mode                               = "Тип скану", // MaxLen: 16
.hold                                    = "Утрим.", // MaxLen: 16
.pause                                   = "Пауза", // MaxLen: 16
.list_empty                              = "Список пустий", // MaxLen: 16
.delete_contact_qm                       = "Видалити конт.?", // MaxLen: 16
.contact_deleted                         = "Контакт видален", // MaxLen: 16
.contact_used                            = "Контакт викор.", // MaxLen: 16
.in_tg_list                              = "у списку TG", // MaxLen: 16
.select_tx                               = "Вибір TX", // MaxLen: 16
.edit_contact                            = "Редаг. контакт", // MaxLen: 16
.delete_contact                          = "Видал. контакт", // MaxLen: 16
.group_call                              = "Груповий викл.", // MaxLen: 16
.all_call                                = "Виклик всіх", // MaxLen: 16
.tone_scan                               = "Скан тонів", // MaxLen: 16
.low_battery                             = "СЛАБКА БАТАРЕЯ", // MaxLen: 16
.Auto                                    = "Авто", // MaxLen: 16
.manual                                  = "Ручний", // MaxLen: 16
.ptt_toggle                              = "Фікс. PTT", // MaxLen: 16
.private_call_handling                   = "Дозвіл PC", // MaxLen: 16
.stop                                    = "Стоп", // MaxLen: 16
.one_line                                = "1 рядок", // MaxLen: 16
.two_lines                               = "2 рядки", // MaxLen: 16
.new_channel                             = "Новий канал", // MaxLen: 16
.priority_order                          = "Порядок", // MaxLen: 16
.dmr_beep                                = "Звук DMR", // MaxLen: 16
.start                                   = "Початок", // MaxLen: 16
.both                                    = "Обидва", // MaxLen: 16
.vox_threshold                           = "Поріг VOX", // MaxLen: 16
.vox_tail                                = "VOX tail", // MaxLen: 16
.audio_prompt                            = "Звуки", // MaxLen: 16
.silent                                  = "Тихо", // MaxLen: 16
.rx_beep                                 = "Звук RX", // MaxLen: 16
.beep                                    = "Сигнал", // MaxLen: 16
.voice_prompt_level_1                    = "Голос", // MaxLen: 16
.transmitTalkerAliasTS1                  = "TA Tx TS1", // MaxLen: 16
.squelch_VHF                             = "VHF Squelch", // MaxLen: 16
.squelch_220                             = "220 Squelch", // MaxLen: 16
.squelch_UHF                             = "UHF Squelch", // MaxLen: 16
.display_screen_invert                   = "Екран", // MaxLen: 16
.openGD77                                = "OpenGD77", // MaxLen: 16
.talkaround                              = "Talkaround", // MaxLen: 16
.APRS                                    = "APRS", // MaxLen: 16
.no_keys                                 = "Без клавіш", // MaxLen: 16
.gitCommit                               = "Git коміт", // MaxLen: 16
.voice_prompt_level_2                    = "Голос 2", // MaxLen: 16
.voice_prompt_level_3                    = "Голос 3", // MaxLen: 16
.dmr_filter                              = "DMR фільтр", // MaxLen: 16
.talker                                  = "Абонент", // MaxLen: 16
.dmr_ts_filter                           = "TS фільтр", // MaxLen: 16
.dtmf_contact_list                       = "FM DTMF контакт", // MaxLen: 16
.channel_power                           = "Потуж.", // MaxLen: 16
.from_master                             = "Загальна", // MaxLen: 16
.set_quickkey                            = "Швидка клав.", // MaxLen: 16
.dual_watch                              = "Dual Watch", // MaxLen: 16
.info                                    = "Інфо", // MaxLen: 16
.pwr                                     = "Потуж.", // MaxLen: 16
.user_power                              = "Своя потуж.", // MaxLen: 16
.temperature                             = "Температура", // MaxLen: 16
.celcius                                 = "°C", // MaxLen: 16
.seconds                                 = "секунд", // MaxLen: 16
.radio_info                              = "Про рацію", // MaxLen: 16
.temperature_calibration                 = "Temp Cal", // MaxLen: 16
.pin_code                                = "PIN-код", // MaxLen: 16
.please_confirm                          = "Підтвердіть", // MaxLen: 16
.vfo_freq_bind_mode                      = "Прив'язка", // MaxLen: 16
.overwrite_qm                            = "Перезаписати?", // MaxLen: 16
.eco_level                               = "Рівень Eco", // MaxLen: 16
.buttons                                 = "Кнопки", // MaxLen: 16
.leds                                    = "Індикатори", // MaxLen: 16
.scan_dwell_time                         = "Утримання", // MaxLen: 16
.battery_calibration                     = "Batt. Cal", // MaxLen: 16
.low                                     = "Низький", // MaxLen: 16
.high                                    = "Високий", // MaxLen: 16
.dmr_id                                  = "DMR ID", // MaxLen: 16
.scan_on_boot                            = "Скан при вкл", // MaxLen: 16
.dtmf_entry                              = "Ввід DTMF", // MaxLen: 16
.name                                    = "Назва", // MaxLen: 16
.carrier                                 = "Несуча", // MaxLen: 16
.zone_empty                              = "Зона пуста", // MaxLen: 16
.time                                    = "Час", // MaxLen: 16
.uptime                                  = "Час роботи", // MaxLen: 16
.hours                                   = "Години", // MaxLen: 16
.minutes                                 = "Хвилини", // MaxLen: 16
.satellite                               = "Супутники", // MaxLen: 16
.alarm_time                              = "Час сигналу", // MaxLen: 16
.location                                = "Локація", // MaxLen: 16
.date                                    = "Дата", // MaxLen: 16
.timeZone                                = "Часовий пояс", // MaxLen: 16
.suspend                                 = "Призупин.", // MaxLen: 16
.pass                                    = "Проліт", // MaxLen: 16
.elevation                               = "El", // MaxLen: 16
.azimuth                                 = "Az", // MaxLen: 16
.inHHMMSS                                = "через", // MaxLen: 16
.predicting                              = "Прогноз", // MaxLen: 16
.maximum                                 = "Макс", // MaxLen: 16
.satellite_short                         = "SAT", // MaxLen: 16
.use_location_short                      = "Локація", // MaxLen: 16
.UTC                                     = "UTC", // MaxLen: 16
.symbols                                 = "NSEW", // MaxLen: 16
.not_set                                 = "НЕ ВСТАН.", // MaxLen: 16
.general_options                         = "Загальні опції", // MaxLen: 16
.radio_options                           = "Опції рації", // MaxLen: 16
.auto_night                              = "Авто ніч", // MaxLen: 16
.dmr_rx_agc                              = "DMR Rx AGC", // MaxLen: 16
.speaker_click_suppress                  = "STE", // MaxLen: 16
.gps                                     = "GPS", // MaxLen: 16
.end_only                                = "Кінець", // MaxLen: 16
.dmr_crc                                 = "DMR CRC", // MaxLen: 16
.eco                                     = "Eco", // MaxLen: 16
.safe_power_on                           = "Безпечне вкл", // MaxLen: 16
.auto_power_off                          = "Автовимк.", // MaxLen: 16
.apo_with_rf                             = "Автовимк+RF", // MaxLen: 16
.brightness_night                        = "Нічна яскр.", // MaxLen: 16
.freq_set_VHF                            = "Частота VHF", // MaxLen: 16
.gps_acquiring                           = "Пошук", // MaxLen: 16
.altitude                                = "Вис", // MaxLen: 16
.calibration                             = "Калібрування", // MaxLen: 16
.freq_set_UHF                            = "Частота UHF", // MaxLen: 16
.cal_frequency                           = "Частота", // MaxLen: 16
.cal_pwr                                 = "Рівень потуж.", // MaxLen: 16
.pwr_set                                 = "Налашт.", // MaxLen: 16
.factory_reset                           = "Скидання завод.", // MaxLen: 16
.rx_tune                                 = "Настройка Rx", // MaxLen: 16
.transmitTalkerAliasTS2                  = "TA Tx TS2", // MaxLen: 16
.ta_text                                 = "Текст", // MaxLen: 16
.daytime_theme_day                       = "Денна тема", // MaxLen: 16
.daytime_theme_night                     = "Нічна тема", // MaxLen: 16
.theme_chooser                           = "Вибір теми", // MaxLen: 16
.theme_options                           = "Опції теми", // MaxLen: 16
.theme_fg_default                        = "Текст типовий", // MaxLen: 16
.theme_bg                                = "Фон", // MaxLen: 16
.theme_fg_decoration                     = "Декор", // MaxLen: 16
.theme_fg_text_input                     = "Ввід тексту", // MaxLen: 16
.theme_fg_splashscreen                   = "Текст завант.", // MaxLen: 16
.theme_bg_splashscreen                   = "Фон завант.", // MaxLen: 16
.theme_fg_notification                   = "Текст сповіщ.", // MaxLen: 16
.theme_fg_warning_notification           = "Попередження", // MaxLen: 16
.theme_fg_error_notification             = "Сповіщ.помилки", // MaxLen: 16
.theme_bg_notification                   = "Фон сповіщ.", // MaxLen: 16
.theme_fg_menu_name                      = "Назва меню", // MaxLen: 16
.theme_bg_menu_name                      = "Фон назви меню", // MaxLen: 16
.theme_fg_menu_item                      = "Пункт меню", // MaxLen: 16
.theme_fg_menu_item_selected             = "Виділ. пункту", // MaxLen: 16
.theme_fg_options_value                  = "Значення опції", // MaxLen: 16
.theme_fg_header_text                    = "Текст заголовк", // MaxLen: 16
.theme_bg_header_text                    = "Фон заголовка", // MaxLen: 16
.theme_fg_rssi_bar                       = "Шкала RSSI", // MaxLen: 16
.theme_fg_rssi_bar_s9p                   = "Шкала RSSI S9+", // MaxLen: 16
.theme_fg_channel_name                   = "Назва каналу", // MaxLen: 16
.theme_fg_channel_contact                = "Контакт", // MaxLen: 16
.theme_fg_channel_contact_info           = "Інфо контакту", // MaxLen: 16
.theme_fg_zone_name                      = "Назва зони", // MaxLen: 16
.theme_fg_rx_freq                        = "Частота RX", // MaxLen: 16
.theme_fg_tx_freq                        = "Частота TX", // MaxLen: 16
.theme_fg_css_sql_values                 = "Значення CSS/SQ", // MaxLen: 16
.theme_fg_tx_counter                     = "Лічильник TX", // MaxLen: 16
.theme_fg_polar_drawing                  = "Полярна", // MaxLen: 16
.theme_fg_satellite_colour               = "Точка супутн.", // MaxLen: 16
.theme_fg_gps_number                     = "Номер GPS", // MaxLen: 16
.theme_fg_gps_colour                     = "Точка GPS", // MaxLen: 16
.theme_fg_bd_colour                      = "Точка BeiDou", // MaxLen: 16
.theme_colour_picker_red                 = "Червоний", // MaxLen: 16
.theme_colour_picker_green               = "Зелений", // MaxLen: 16
.theme_colour_picker_blue                = "Синій", // MaxLen: 16
.volume                                  = "Гучн.", // MaxLen: 16
.roaming                                 = "Roaming", // MaxLen: 16
.show_distance                           = "Показ відст.", // MaxLen: 16
.aprs_options                            = "Опції APRS", // MaxLen: 16
.aprs_smart                              = "Розумний", // MaxLen: 16
.aprs_channel                            = "Канал", // MaxLen: 16
.aprs_decay                              = "Згасання", // MaxLen: 16
.aprs_compress                           = "Стиснення", // MaxLen: 16
.aprs_interval                           = "Інтервал", // MaxLen: 16
.aprs_message_interval                   = "Інтерв. пов.", // MaxLen: 16
.aprs_slow_rate                          = "Повільно", // MaxLen: 16
.aprs_fast_rate                          = "Швидко", // MaxLen: 16
.aprs_low_speed                          = "Low Speed", // MaxLen: 16
.aprs_high_speed                         = "Hi Speed", // MaxLen: 16
.aprs_turn_angle                         = "Кут повороту", // MaxLen: 16
.aprs_turn_slope                         = "Крутизна", // MaxLen: 16
.aprs_turn_time                          = "Час повор.", // MaxLen: 16
.auto_lock                               = "Автоблок.", // MaxLen: 16
.trackball                               = "Trackball", // MaxLen: 16
.dmr_force_dmo                           = "Примус DMO", // MaxLen: 16
.tx_inhibit                              = "Блок. TX", // MaxLen: 16
.latitude_short                          = "Шир", // MaxLen: 16
.longitude_short                         = "Довг", // MaxLen: 16
.text_size                               = "Розмір тексту", // MaxLen: 16
.last_talker                             = "Ост. абонент", // MaxLen: 16
.mute                                    = "Без звуку", // MaxLen: 16
#if defined(ENABLE_AES)
.aes_keys                                = "Ключі AES", // MaxLen: 16
.encrypt_tx                              = "Шифр. TX", // MaxLen: 16
#if defined(ENABLE_DMR_DATA)
.messages                                = "Повідомлення", // MaxLen: 16
#endif
#endif
#if defined(LANGUAGE_BUILD_UKRAINIAN)
.gps_absent                              = "GPS відсутній", // MaxLen: 16
#endif
};
/********************************************************************
 *
 * VERY IMPORTANT.
 * This file should not be saved with UTF-8 encoding
 * Use Notepad++ on Windows with ANSI encoding
 * or emacs on Linux with binary encoding
 *
 ********************************************************************/
#endif /* USER_INTERFACE_LANGUAGES_UKRAINIAN_H_ */

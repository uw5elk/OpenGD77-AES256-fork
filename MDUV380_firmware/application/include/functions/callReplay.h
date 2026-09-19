/*
 * Форк (2026-09-19). "Переслухати" -- кільцевий буфер у CCM з останніми ~30 с
 * ПРИЙНЯТОГО (RX) DMR-голосу на ВІДКРИТИХ каналах, і відтворення того самого
 * запису через штатний кодек-шлях (той самий, що й голосові підказки).
 *
 * Розвідка: docs/recording-feasibility.md (сценарій A). Причина обмеження на
 * відкриті канали -- docs/recording-feasibility.md, розділ 2.5: шифрування в
 * цьому форку XOR-иться на ДЕКОДОВАНИЙ 49-бітний вектор AMBE, ПІСЛЯ FEC-декоду
 * (RX) / ДО FEC-кодування (TX) -- а не на сирі 27 байт, які тут захоплюються.
 * Тобто на AES-каналі сирі 27 байт -- це вже "перемішаний" з ключовим потоком
 * FEC-кодовий блок, а не звичайний шифротекст поверх звичайного AMBE: його не
 * можна ні прочитати як голос, ні безпечно віддати чужому слухачеві. Простіше
 * й безпечніше -- не записувати такі кадри взагалі (RxActive() перевіряється
 * при кожному захопленні).
 *
 * Усе під ENABLE_CALL_REPLAY (типово вимкнено). Типова збірка без цього
 * прапорця не викликає жодної з цих функцій -- дивись виклики під тим самим
 * #if у HR-C6000.c/applicationMain.c/menuSystem.c, а не заглушки нижче: вони
 * лише про всяк випадок (щоб файл, який випадково забув свій #if, не впав на
 * лінкері), а не основний механізм byte-identical збірки.
 */
#ifndef _OPENGD77_CALLREPLAY_H_
#define _OPENGD77_CALLREPLAY_H_

#include <stdint.h>
#include <stdbool.h>

#if defined(ENABLE_CALL_REPLAY)

/* AMBE_AUDIO_LENGTH (HR-C6000.h) -- та сама "група" з 3 кадрів (9 Б кожен) =
 * 60 мс аудіо, у тому самому форматі, що споживає codecDecode(ptr, 3), і в
 * якому голосові підказки вже зберігають записані фрази (voicePrompts.c).
 * Дубльовано числом, а не інклудом HR-C6000.h, щоб цей заголовок лишався
 * легким -- callReplay.c звіряє це static_assert-ом. */
#define CALL_REPLAY_GROUP_BYTES   27U
/* 500 груп x 27 Б = 13500 Б -- 30 с при 450 Б/с (60 мс/група), увесь буфер у
 * .ccmram (docs/notification-buffer.md звільнив під це місце). */
#define CALL_REPLAY_GROUPS        500U

/* Обнулити стан у CCM -- .ccmram НЕ ініціалізується стартапом (той самий
 * застережний коментар, що й біля dmrAesInit()), тож без явного виклику тут
 * були б сміттєві writeIdx/count після холодного старту. Викликати з
 * applicationMain.c одразу після dmrAesInit(). */
void     callReplayInit(void);

/* Те саме, але для стану ВІДТВОРЕННЯ (functions/callReplayPlayback.c) -- окрема
 * функція, бо цей стан живе в ІНШОМУ .c, ніж кільце вище (розбиття заради
 * хостового тесту, дивись великий коментар у callReplayPlayback.c). Викликати
 * з applicationMain.c одразу після callReplayInit(). */
void     callReplayPlaybackInit(void);

/* Захоплення ОДНІЄЇ 27-байтної групи. Викликається ЛИШЕ з hrc6000TaskFunction
 * (задача, не переривання), поруч із живим codecDecode(), для КОЖНОГО реально
 * прийнятого голосового кадру (не для вставленої тиші). aesActive -- поточний
 * стан dmrAesRxActive() виклику (кадри AES-виклику пропускаються цілком,
 * дивись великий коментар вище файлу). nowMs -- ticksGetMillis() виклику;
 * передається аргументом (а не читається всередині), щоб цю ж функцію можна
 * було прогнати хостовим тестом із довільними мітками часу без апаратних
 * залежностей. */
void     callReplayCaptureTick(const uint8_t group[CALL_REPLAY_GROUP_BYTES], bool aesActive, uint32_t nowMs);

/* Скільки груп зараз у кільці (0..CALL_REPLAY_GROUPS). */
uint32_t callReplayGroupCount(void);
bool     callReplayIsEmpty(void);
/* Скільки мілісекунд запису доступно (groupCount * 60). */
uint32_t callReplayAvailableMs(void);

/* Читання i-тої групи в порядку "від найстарішої до найновішої" (0..count-1)
 * ВІДНОСНО ПОТОЧНОГО стану кільця в момент виклику. Чиста функція -- жодних
 * апаратних викликів, саме її й ганяє хостовий тест кільцевої логіки.
 * outIsOverStart -- true, якщо ЦЯ група -- перша в новому "заході" (over):
 * розрив у часі із попереднім захопленим кадром перевищив поріг (нова
 * радіопередача, а не продовження попередньої). Повертає false, якщо
 * playIndex >= поточного count. */
bool     callReplayPlaybackGroup(uint32_t playIndex, const uint8_t **outGroup, bool *outIsOverStart);

/* ВНУТРІШНЄ API для callReplayPlayback.c (окремий .c, апаратний рушій відтворення --
 * НЕ бере участі в хостовому тесті кільцевої логіки). Публічне лише тому, що обидва
 * файли компілюються окремо; ззовні модуля викликати немає сенсу.
 *
 * callReplayPlaybackGroup() вище перераховує "найстарішу" позицію ВІДНОСНО ПОТОЧНОГО
 * (живого) стану кільця при кожному виклику -- коректно для хостового тесту (де немає
 * паралельного запису), але НЕБЕЗПЕЧНО для реального відтворення: захоплення (інша
 * задача) може дописувати нові групи, поки програється стара позиція, і "найстаріша"
 * зсунеться просто під час програвання. Тому рушій відтворення заморожує вікно ОДИН
 * РАЗ на старті (callReplayOldestPhysIndex()) і потім читає лише фізичними індексами
 * (callReplayRawGroupAt()), не перераховуючи його заново. */
uint16_t callReplayOldestPhysIndex(void);
void     callReplayRawGroupAt(uint16_t physIdx, const uint8_t **outGroup, bool *outIsOverStart);

/* Відтворення -- апаратний контекст (main loop, поруч із voicePromptsTick()).
 * Реалізація -- functions/callReplayPlayback.c, окремо від чистого кільця вище
 * (щоб хостовий тест кільцевої логіки не тягнув codec/sound/trx/FreeRTOS). */
void     callReplayTick(void);
/* false -- буфер порожній АБО зараз іде прийом/передача (не стартувало). */
bool     callReplayStart(void);
void     callReplayStop(void);
bool     callReplayIsPlaying(void);
/* Для екрана "Переслухати" -- прогрес поточного відтворення (0, якщо не активне).
 * total -- ЗНІМОК довжини буфера на момент callReplayStart(), не поточний
 * callReplayAvailableMs() (буфер міг дописатись під час відтворення). */
uint32_t callReplayPlayedMs(void);
uint32_t callReplayPlayTotalMs(void);

#else // !ENABLE_CALL_REPLAY -- заглушки на випадок пропущеного #if у виклику;
      // на byte-identical збірку не впливають (у типовій збірці ЖОДЕН виклик
      // нижче не існує в жодному .c -- дивись коментар вище файлу).
static inline void     callReplayInit(void) { }
static inline void     callReplayPlaybackInit(void) { }
static inline void     callReplayCaptureTick(const uint8_t *g, bool a, uint32_t n) { (void)g; (void)a; (void)n; }
static inline uint32_t callReplayGroupCount(void) { return 0U; }
static inline bool     callReplayIsEmpty(void) { return true; }
static inline uint32_t callReplayAvailableMs(void) { return 0U; }
static inline bool     callReplayPlaybackGroup(uint32_t i, const uint8_t **g, bool *b) { (void)i; (void)g; (void)b; return false; }
static inline uint16_t callReplayOldestPhysIndex(void) { return 0U; }
static inline void     callReplayRawGroupAt(uint16_t i, const uint8_t **g, bool *b) { (void)i; (void)g; (void)b; }
static inline void     callReplayTick(void) { }
static inline bool     callReplayStart(void) { return false; }
static inline void     callReplayStop(void) { }
static inline bool     callReplayIsPlaying(void) { return false; }
static inline uint32_t callReplayPlayedMs(void) { return 0U; }
static inline uint32_t callReplayPlayTotalMs(void) { return 0U; }
#endif

#endif /* _OPENGD77_CALLREPLAY_H_ */

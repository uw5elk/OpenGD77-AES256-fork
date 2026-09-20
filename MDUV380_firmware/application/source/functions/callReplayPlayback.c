/*
 * Форк (2026-09-19). "Переслухати" -- апаратний рушій ВІДТВОРЕННЯ. Дивись
 * великий коментар у callReplay.h. Цей файл НЕ бере участі в хостовому тесті
 * кільцевої логіки (tests/test_call_replay.c лінкує лише callReplay.c) --
 * саме тому весь codec/sound/trx/FreeRTOS живе тут, окремо від чистого
 * кільця.
 *
 * Читає кільце ЛИШЕ через callReplayOldestPhysIndex()/callReplayRawGroupAt()
 * (обидві -- callReplay.c) -- ЖОДНОГО прямого доступу до внутрішніх масивів
 * кільця. Вікно відтворення заморожується ОДИН РАЗ на callReplayStart()
 * (startPhys + total) і потім читається за фіксованим фізичним зсувом:
 * захоплення (інша задача, hrc6000TaskFunction) може дописувати нові групи,
 * поки програється стара позиція, тож "найстаріша" позиція могла б зсунутись
 * просто під час відтворення, якби рушій питав про неї живцем на кожному
 * кадрі -- дивись callReplay.h.
 *
 * Увесь файл під ENABLE_CALL_REPLAY (типово вимкнено) -- на byte-identical
 * типову збірку не впливає жодним байтом (порожній translation unit).
 */
#ifdef ENABLE_CALL_REPLAY

#include "functions/callReplay.h"
#include "dmr_codec/codec.h"          /* codecDecode/codecInit, тягне HR-C6000.h (AMBE_AUDIO_LENGTH) */
#include "functions/sound.h"          /* wavbuffer_count, audioAmp*, soundTick*, FreeRTOS task.h */
#include "functions/trx.h"            /* trxTransmissionEnabled/trxIsTransmitting/trxCarrierDetected */
#include "functions/voicePrompts.h"   /* voicePromptsIsPlaying() -- не зривати чужий звук */
#include "functions/rxPowerSaving.h"  /* rxPowerSavingSetState(ECOPHASE_POWERSAVE_INACTIVE) -- фікс хлопків, дивись callReplayStart() */
#include "functions/settings.h"       /* nonVolatileSettings.dmrRxAGC */
#include "functions/ticks.h"          /* ticksGetMillis() -- дросель картки, дивись CALL_REPLAY_NOTIFICATION_REFRESH_MS */
#include "hardware/radioHardwareInterface.h" /* RADIO_DEVICE_PRIMARY, radioSetAudioPath */
#include "functions/codeplug.h"       /* codeplugGetOpenGD77CustomDataBounded/SetOpenGD77CustomData --
                                        * персистентність перемикача "Запис RX", дивись callReplayConfigLoad() */
#include "user_interface/menuSystem.h" /* uiNotificationShow/Hide/IsVisible/GetId + NOTIFICATION_*_CALL_REPLAY --
                                         * картка відтворення (задача 2026-09-20), той самий шлях, що вже
                                         * використовує functions/dmr_sms.c для банера SMS. */
#include <string.h>                   /* memcmp/memcpy/memset для callReplayOnFlashCfg_t нижче */

/* Картка "Переслухати" під час відтворення (задача 2026-09-20, варіант A з макета) --
 * висить ДО КІНЦЯ відтворення (uiNotificationHasTimedOut() має спеціальний випадок для
 * NOTIFICATION_TYPE_CALL_REPLAY у uiNotification.c), тож це значення ніколи фактично не
 * спрацьовує -- лишень "про всяк випадок", як і 60000 для банера SMS (dmr_sms.c). */
#define CALL_REPLAY_NOTIFICATION_TIMEOUT_MS   3600000U
/* Як часто оновлювати час/смугу на картці, поки грає (задача: "приблизно раз на
 * 250-500 мс"; НЕ щотіку -- displayLevelCard() перемальовує ввесь екран). */
#define CALL_REPLAY_NOTIFICATION_REFRESH_MS   300U

/* Повідомлення асерту -- лише для хостового компілятора (діагностика збірки, ніколи не
 * потрапляє на екран рації), тож навмисно англійською/ASCII -- check_string_encoding.py
 * (за задумом) ловить кирилицю САМЕ в .c, бо вона там завжди помилка кодування шрифту.
 * Асерт живе саме тут (не в callReplay.c): цей файл і так тягне codec.h заради
 * codecDecode()/codecInit(), тож HR-C6000.h (AMBE_AUDIO_LENGTH) уже підключений, а
 * callReplay.c свідомо лишається легким (жодних апаратних заголовків). */
_Static_assert(CALL_REPLAY_GROUP_BYTES == AMBE_AUDIO_LENGTH,
		"CALL_REPLAY_GROUP_BYTES must match AMBE_AUDIO_LENGTH (HR-C6000.h)");

/* Пауза між "заходами" при ВІДТВОРЕННІ (не при записі -- у буфері паузи
 * взагалі не зберігаються, лише реальні кадри). 3 групи x 60 мс = 180 мс --
 * досить, щоб на слух розрізнити дві окремі передачі, не настільки довго, щоб
 * відчутно розтягувати відтворення. */
#define CALL_REPLAY_OVER_PAUSE_GROUPS   3U

typedef struct
{
	bool     active;
	uint16_t startPhys;        // фізичний індекс НАЙСТАРІШОЇ групи -- заморожено на старті
	uint32_t index;             // зсув від startPhys, 0..total-1
	uint32_t total;              // знімок count на момент старту (буфер міг дописатись ПІД ЧАС відтворення -- ми в те вікно більше не заглядаємо, дивись коментар у callReplayTick())
	uint32_t startIndex;         // playIndex, з якого ПОЧАЛАСЬ ця сесія відтворення (задача
	                             // 2026-09-20): 0 для "усі ~30 с", callReplayFindLastOverStart()
	                             // для "останній перехід". Потрібен ОКРЕМО від index (яке
	                             // рухається вперед) -- і для правила паузи в callReplayTick()
	                             // ("не перша група ЦІЄЇ сесії", а не буквально "не нульова"),
	                             // і для played/total-ms нижче (прогрес відносно самого
	                             // переходу, а не всього буфера).
	bool     lastTransitionOnly; // режим "Останній виклик" -- лише для callReplayIsLastTransitionMode()
	                             // (картка екрана/сповіщення); на саму логіку відтворення в
	                             // callReplayTick() не впливає -- той факт, що це єдиний захід
	                             // у вікні [startIndex..total), уже гарантовано пошуком у
	                             // callReplayFindLastOverStart() (жодного іншого isOverStart
	                             // немає в цьому діапазоні, тож пауз і так не буде вставлено).
	uint32_t lastNotifRefreshMs; // ticksGetMillis() останнього uiNotificationRefresh() картки
	                             // (задача 2026-09-20) -- дросель на CALL_REPLAY_NOTIFICATION_REFRESH_MS,
	                             // той самий ідіом віднімання, що ticksTimerHasExpired() (ticks.c):
	                             // окреме поле, а не ticksTimer_t (8 Б), заради мінімального
	                             // приросту CCM (лише 4 Б).
	uint16_t pauseRemaining;    // скільки груп тиші лишилось вставити перед наступною
	bool     boundaryHandled;   // пауза для поточної межі "over" вже вставлена
} callReplayPlayState_t;

static __attribute__((section(".ccmram"))) callReplayPlayState_t callReplayPlay;

/* AMBE "тиша" -- та сама, що SILENCE_AUDIO в HR-C6000.c (той масив static, тому
 * копія тут; той самий 27-байтний "comfort noise" кадр, яким прошивка й так
 * заповнює провали в живому прийомі). Використовується ЛИШЕ для паузи між
 * заходами при відтворенні -- у самому записі тиша ніколи не зберігається. */
static const uint8_t callReplaySilenceGroup[CALL_REPLAY_GROUP_BYTES] = {
		0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU, 0xB9U, 0xE8U, 0x81U, 0x52U,
		0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU, 0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU
};

void callReplayPlaybackInit(void)
{
	// Той самий застережний коментар, що й біля callReplayInit()/dmrAesInit():
	// .ccmram не ініціалізується стартапом. Викликати з applicationMain.c одразу
	// після callReplayInit() (окрема функція, а не спільна з нею -- бо стан
	// відтворення живе в ЦЬОМУ файлі, а не в callReplay.c).
	callReplayPlay.active = false;
	callReplayPlay.startPhys = 0U;
	callReplayPlay.index = 0U;
	callReplayPlay.total = 0U;
	callReplayPlay.startIndex = 0U;
	callReplayPlay.lastTransitionOnly = false;
	callReplayPlay.lastNotifRefreshMs = 0U;
	callReplayPlay.pauseRemaining = 0U;
	callReplayPlay.boundaryHandled = false;
}

/* Персистентність перемикача "Запис RX" (задача 2026-09-19, п.5). Малий custom-data
 * блок -- ОБҐРУНТУВАННЯ вибору цього способу (а не біт у nonVolatileSettings, як
 * розглядалось спершу) -- великий коментар біля BIT_UNUSED_1 у settings.h і біля
 * оголошень callReplayConfigLoad()/Save() у callReplay.h: коротко, перемикач має
 * бути записуваний з tools/gui_flasher.py, а CPS-запис EEPROM -- no-op на MDUV380.
 *
 * Формат -- звичайна статична пам'ять (НЕ CCM): цей блок читається/пишеться рідко
 * (старт і перемикання в меню), а не на кожному тіку, тож CCM-бюджет тут не
 * вигравав би нічого -- той самий принцип, що dmrRctlOnFlashCfg_t у dmr_rctl_cfg.c. */
typedef struct
{
	char    magic[4];   // "CRPL"
	uint8_t version;    // 1
	uint8_t enabled;     // 0/1 -- ПРЯМА полярність (на відміну від відкинутого BIT_CALL_REPLAY_DISABLED):
	                     // тут інверсія не потрібна, бо ВІДСУТНІСТЬ блоку (нова/нечіпана рація) сама по
	                     // собі означає "типове значення" -- callReplayConfigLoad() нижче лишає
	                     // callReplayInit()-типове (Увімкнено), не записуючи нічого в цьому випадку.
} callReplayOnFlashCfg_t;

#define CALL_REPLAY_CFG_VERSION  1U

void callReplayConfigLoad(void)
{
	callReplayOnFlashCfg_t cfg;

	memset(&cfg, 0, sizeof(cfg));

	if (codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_CALL_REPLAY_CONFIG,
			(uint8_t *)&cfg, (int)sizeof(cfg)) &&
			(memcmp(cfg.magic, "CRPL", 4) == 0))
	{
		callReplaySetRecordingEnabled(cfg.enabled != 0U);
	}
	// Блоку немає (нова/нечіпана рація) -- типове значення callReplayInit() (Увімкнено,
	// задача п.5) лишається як є, нічого писати не треба.
}

bool callReplayConfigSave(bool enabled)
{
	callReplayOnFlashCfg_t cfg;

	memset(&cfg, 0, sizeof(cfg));

	// Знос флеша: applySettings() (menuSoundOptions.c) викликає це на КОЖНЕ підтвердження
	// екрана Options>Sound, не лише коли саме цей пункт змінили -- той самий принцип, що
	// dmrRctlSetInhibited() у dmr_rctl_cfg.c ("уже в потрібному стані -- зайвий запис не
	// робимо"). Якщо на флеші вже лежить блок з ЦИМ самим enabled -- пропускаємо запис.
	if (codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_CALL_REPLAY_CONFIG,
			(uint8_t *)&cfg, (int)sizeof(cfg)) &&
			(memcmp(cfg.magic, "CRPL", 4) == 0) &&
			(cfg.version == CALL_REPLAY_CFG_VERSION) &&
			(cfg.enabled == (enabled ? 1U : 0U)))
	{
		callReplaySetRecordingEnabled(enabled); // вже узгоджено -- лише живий стан, про всяк випадок
		return true;
	}

	memset(&cfg, 0, sizeof(cfg));
	memcpy(cfg.magic, "CRPL", 4);
	cfg.version = CALL_REPLAY_CFG_VERSION;
	cfg.enabled = (enabled ? 1U : 0U);

	bool ok = codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_CALL_REPLAY_CONFIG, (uint8_t *)&cfg, (int)sizeof(cfg));

	if (ok)
	{
		// Живий стан застосовуємо ЛИШЕ при успішному записі -- викликач (menuSoundOptions.c)
		// не повинен вважати перемикання застосованим, якщо флеш не прийняв запис.
		callReplaySetRecordingEnabled(enabled);
	}

	return ok;
}

bool callReplayIsPlaying(void)
{
	return callReplayPlay.active;
}

bool callReplayIsLastTransitionMode(void)
{
	return (callReplayPlay.active && callReplayPlay.lastTransitionOnly);
}

uint32_t callReplayPlayedMs(void)
{
	// Відносно startIndex ЦІЄЇ сесії (задача 2026-09-20) -- у режимі "останній
	// перехід" прогрес показує сам перехід, а не позицію в усьому буфері;
	// у режимі "усі ~30 с" startIndex==0, тож поведінка не змінюється.
	return (callReplayPlay.active ? ((callReplayPlay.index - callReplayPlay.startIndex) * 60U) : 0U);
}

uint32_t callReplayPlayTotalMs(void)
{
	return (callReplayPlay.active ? ((callReplayPlay.total - callReplayPlay.startIndex) * 60U) : 0U);
}

static bool callReplayBusyOnAir(void)
{
	return (trxTransmissionEnabled || trxIsTransmitting || trxCarrierDetected(RADIO_DEVICE_PRIMARY));
}

/* Спільна частина callReplayStart()/callReplayStartLastTransition() (задача
 * 2026-09-20): УСЕ, що не залежить від того, ЯКИЙ playIndex -- стартовий --
 * перевірки готовності, побудка з еко-режиму, скидання АРУ, підготовка
 * аудіотракту. Різниця між двома режимами -- ЛИШЕ startIndex/index (0 vs.
 * знайдений початок останнього заходу) і lastTransitionOnly-прапорець. */
static bool callReplayStartAt(uint32_t startIndex, bool lastTransitionOnly)
{
	if (callReplayPlay.active)
	{
		return true; // вже відтворюється -- ідемпотентно
	}

	if ((callReplayGroupCount() == 0U) || callReplayBusyOnAir())
	{
		return false; // порожньо, або зараз прийом/передача (п. "Не стартувати...")
	}

	// Фікс "ритмічних хлопків" (польова перевірка 2026-09-19): без цих двох рядків
	// -- вихід рівно той самий, що на початку voicePromptsPlay() -- ефір під час
	// відтворення тихий (ми граємо із запису, живого сигналу немає), і без цього
	// rxPowerSavingTick() (rxPowerSaving.c) за кілька секунд тиші сам почав би
	// цикл ECOPHASE-присипляння приймача/HR-C6000 (кожен цикл цикл вимкнення-
	// увімкнення AT1846S -- чутний хлопок). callReplayIsPlaying() нижче в
	// rxPowerSaving.c не дає ЗАЙТИ в цей цикл ПІД ЧАС відтворення; тут -- та
	// сама негайна побудка, що робить voicePromptsPlay(), про всяк випадок, якщо
	// відтворення стартувало вже ПІД ЧАС активного еко-циклу (RX/HR-C6000 могли
	// бути вимкнені саме в цю мить -- codecInit()/audioAmpEnable() нижче повинні
	// піти з увімкненим трактом).
	rxPowerSavingSetState(ECOPHASE_POWERSAVE_INACTIVE);

	// Той самий порядок, що voicePromptsPlay(): скинути середнє АРУ ДО старту
	// відтворення, а не після -- інакше перші кадри запису йдуть із застарілим
	// (можливо, від попередньої тихої ділянки ефіру) гейном.
	if (nonVolatileSettings.dmrRxAGC != 0)
	{
		soundResetDMRRxAGCGain();
	}

	taskENTER_CRITICAL();

	// Знімок вікна відтворення -- ФІКСУЄМО фізичні межі тут. Захоплення (інша
	// задача, hrc6000TaskFunction) може дописувати нові групи ПІД ЧАС відтворення
	// -- ми в ту частину кільця, що вже вийшла за startPhys+total, більше не
	// заглядаємо, тож нові записи туди нас не зачіпають. Єдиний теоретичний
	// зазор: НОВИЙ виклик встигає дописати кадр у слот playIndex=0 (startPhys)
	// РАНІШЕ, ніж callReplayTick() побачить trxCarrierDetected() і перерве
	// відтворення. На практиці нереально: детект несучої -- майже миттєвий,
	// а до першого РЕАЛЬНОГО аудіокадру нового виклику потрібна повна
	// DMR-синхронізація слота (помітно довше одного тіку). Не доведено
	// математично -- лишаю в TESTING.md на польову перевірку.
	callReplayPlay.total = callReplayGroupCount();
	callReplayPlay.startPhys = callReplayOldestPhysIndex();
	callReplayPlay.startIndex = startIndex;
	callReplayPlay.index = startIndex;
	callReplayPlay.lastTransitionOnly = lastTransitionOnly;
	callReplayPlay.pauseRemaining = 0U;
	callReplayPlay.boundaryHandled = false;
	callReplayPlay.active = true;

	if (soundMelodyIsPlaying())
	{
		soundStopMelody();
	}
	radioSetAudioPath(false);           // HR-C6000 -> підсилювач (той самий шлях, що й підказки)
	audioAmpEnable(AUDIO_AMP_CHANNEL_PROMPT);
	codecInit(true);

	taskEXIT_CRITICAL();

	// Повноекранна картка "Переслухати" (задача 2026-09-20, варіант A) -- ПОЗА
	// критичною секцією (displayLevelCard() штовхає повний кадр на LCD через SPI,
	// довга операція, критичну секцію не тримаємо довше за потрібне). immediateRender=
	// true -- картка з'являється одразу на старті, з обох джерел виклику (гаряча
	// клавіша SK1 і меню "Переслухати", ОБИДВІ йдуть через цей спільний хелпер, тож
	// малювати картку окремо в кожному з них не треба). lastNotifRefreshMs -- від
	// МОМЕНТУ цього показу (він і є "перше" оновлення), щоб наступний періодичний
	// рефреш у callReplayTick() не спрацював одразу ж повторно.
	callReplayPlay.lastNotifRefreshMs = ticksGetMillis();
	uiNotificationShow(NOTIFICATION_TYPE_CALL_REPLAY, NOTIFICATION_ID_CALL_REPLAY,
			CALL_REPLAY_NOTIFICATION_TIMEOUT_MS, NULL, true);

	return true;
}

bool callReplayStart(void)
{
	return callReplayStartAt(0U, false);
}

bool callReplayStartLastTransition(void)
{
	// callReplayFindLastOverStart() -- чиста логіка в callReplay.c (хостовий тест
	// покриває саме її); тут лише передаємо результат у спільний старт-хелпер.
	// Порожній буфер: callReplayStartAt() сама поверне false нижче за
	// callReplayGroupCount()==0U ще ДО використання цього значення, тож
	// викликати callReplayFindLastOverStart() тут безпечно навіть на порожньому
	// кільці (вона й сама повертає 0 у цьому випадку, дивись callReplay.c).
	return callReplayStartAt(callReplayFindLastOverStart(), true);
}

static void callReplayStopInternal(void)
{
	if (!callReplayPlay.active)
	{
		return;
	}

	taskENTER_CRITICAL();
	callReplayPlay.active = false;
	audioAmpDisable(AUDIO_AMP_CHANNEL_PROMPT);
	soundTerminateSound();
	codecInit(true);
	taskEXIT_CRITICAL();

	// Закрити картку "Переслухати" (задача 2026-09-20) -- ЄДИНЕ місце для ВСІХ причин
	// зупинки (явний стоп SK1/RED, кінець буфера, переривання PTT/вхідним викликом --
	// усі три шляхи callReplayStop()/callReplayTick() ведуть саме сюди), тож окремо
	// ховати картку в кожному з них не треба. Перевірка ID -- про всяк випадок: якщо
	// картку вже перекрила ІНША (наприклад, вхідне SMS), не чіпаємо її -- закриваємо
	// ЛИШЕ якщо на екрані досі саме наша. immediateRender=true -- екран (VFO/канал/
	// меню "Переслухати") коректно відновлюється одразу, без чекання на випадковий
	// наступний перемальов.
	if (uiNotificationIsVisible() && (uiNotificationGetId() == NOTIFICATION_ID_CALL_REPLAY))
	{
		uiNotificationHide(true);
	}
}

void callReplayStop(void)
{
	callReplayStopInternal();
}

void callReplayTick(void)
{
	uint16_t phys;
	bool isOverStart;
	const uint8_t *group;

	if (!callReplayPlay.active)
	{
		return;
	}

	if (callReplayBusyOnAir())
	{
		// PTT або вхідний виклик -- перериваємо негайно (не чекаємо кінця буфера).
		callReplayStopInternal();
		return;
	}

	if (callReplayPlay.index >= callReplayPlay.total)
	{
		// Усе відтворено -- чекаємо, поки звукобуфер спорожніє, і завершуємо
		// (той самий патерн, що voicePromptsTick() наприкінці послідовності).
		if (wavbuffer_count == 0)
		{
			callReplayStopInternal();
		}
		return;
	}

	// Картка "Переслухати" -- оновлюємо час/смугу приблизно раз на
	// CALL_REPLAY_NOTIFICATION_REFRESH_MS (задача 2026-09-20), а НЕ щотіку:
	// displayLevelCard() перемальовує ввесь екран через SPI, і робити це на кожен
	// виклик callReplayTick() (набагато частіше за 60 мс/групу -- цей тік викликається
	// з головного циклу, не з темпу самого відтворення) було б і зайвим навантаженням,
	// і тим самим "повним перемальовуванням щотіку", якого задача явно просить уникати.
	// Той самий ідіом віднімання, що ticksTimerHasExpired() (ticks.c) -- коректний і при
	// перегортанні ticksGetMillis(). uiNotificationIsVisible()+GetId() -- захист про
	// всяк випадок: якщо картку вже перекрила інша (наприклад, вхідне SMS), не малюємо
	// поверх чужої -- callReplayStopInternal() однаково закриє ЛИШЕ свою, коли настане час.
	if ((ticksGetMillis() - callReplayPlay.lastNotifRefreshMs) >= CALL_REPLAY_NOTIFICATION_REFRESH_MS)
	{
		callReplayPlay.lastNotifRefreshMs = ticksGetMillis();

		if (uiNotificationIsVisible() && (uiNotificationGetId() == NOTIFICATION_ID_CALL_REPLAY))
		{
			uiNotificationRefresh();
		}
	}

	if (voicePromptsIsPlaying() || soundMelodyIsPlaying())
	{
		return; // не зривати чужий звук -- почекаємо наступного тіку
	}

	taskENTER_CRITICAL();
	if (wavbuffer_count <= WAV_BUFFER_AMBE_PREBUFFERING_COUNT)
	{
		if (callReplayPlay.pauseRemaining > 0U)
		{
			codecDecode((uint8_t *)callReplaySilenceGroup, 3);
			callReplayPlay.pauseRemaining--;
		}
		else
		{
			phys = (uint16_t)((callReplayPlay.startPhys + callReplayPlay.index) % CALL_REPLAY_GROUPS);
			callReplayRawGroupAt(phys, &group, &isOverStart);

			if (isOverStart && (callReplayPlay.index > callReplayPlay.startIndex) && (callReplayPlay.boundaryHandled == false))
			{
				// Початок нового "заходу" (не першого в ЦІЙ сесії відтворення --
				// callReplayPlay.startIndex, а не буквально 0: задача 2026-09-20,
				// інакше "останній перехід" (startIndex != 0) хибно отримав би
				// зайву паузу перед самою першою зіграною групою) -- спершу
				// коротка пауза, саму групу зіграємо наступного тіку.
				callReplayPlay.pauseRemaining = CALL_REPLAY_OVER_PAUSE_GROUPS;
				callReplayPlay.boundaryHandled = true;
			}
			else
			{
				codecDecode((uint8_t *)group, 3);
				callReplayPlay.index++;
				callReplayPlay.boundaryHandled = false;
			}
		}
	}
	soundTickRXBuffer();
	taskEXIT_CRITICAL();
}

#endif // ENABLE_CALL_REPLAY

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
#include "hardware/radioHardwareInterface.h" /* RADIO_DEVICE_PRIMARY, radioSetAudioPath */

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
	callReplayPlay.pauseRemaining = 0U;
	callReplayPlay.boundaryHandled = false;
}

bool callReplayIsPlaying(void)
{
	return callReplayPlay.active;
}

uint32_t callReplayPlayedMs(void)
{
	return (callReplayPlay.active ? (callReplayPlay.index * 60U) : 0U);
}

uint32_t callReplayPlayTotalMs(void)
{
	return (callReplayPlay.active ? (callReplayPlay.total * 60U) : 0U);
}

static bool callReplayBusyOnAir(void)
{
	return (trxTransmissionEnabled || trxIsTransmitting || trxCarrierDetected(RADIO_DEVICE_PRIMARY));
}

bool callReplayStart(void)
{
	if (callReplayPlay.active)
	{
		return true; // вже відтворюється -- ідемпотентно
	}

	if ((callReplayGroupCount() == 0U) || callReplayBusyOnAir())
	{
		return false; // порожньо, або зараз прийом/передача (п. "Не стартувати...")
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
	callReplayPlay.index = 0U;
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
	return true;
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

			if (isOverStart && (callReplayPlay.index > 0U) && (callReplayPlay.boundaryHandled == false))
			{
				// Початок нового "заходу" (не першого в цьому відтворенні) --
				// спершу коротка пауза, саму групу зіграємо наступного тіку.
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

/*
 * Форк (2026-09-19). Реалізація "Переслухати" -- дивись великий коментар у
 * callReplay.h. Тут лише ЧИСТА кільцева логіка -- захоплення, обгортання,
 * межі "заходів" (over) -- БЕЗ жодної апаратної залежності (codec/sound/trx/
 * FreeRTOS), щоб хостовий тест (tests/test_call_replay.c) міг лінкувати рівно
 * цей файл без заглушок апаратури. Апаратний рушій відтворення -- окремий
 * functions/callReplayPlayback.c, дивись великий коментар у callReplay.h
 * біля callReplayOldestPhysIndex()/callReplayRawGroupAt() -- це єдиний шов
 * між двома файлами.
 *
 * Увесь файл під ENABLE_CALL_REPLAY (типово вимкнено), як і
 * crypto/dmr_aes_hook.c під ENABLE_AES -- на byte-identical типову збірку не
 * впливає жодним байтом (порожній translation unit).
 */
#ifdef ENABLE_CALL_REPLAY

#include <string.h>
#include "functions/callReplay.h"

/* Розрив між захопленими групами довший за це -- нова радіопередача (over), а
 * не продовження попередньої. Евристика: звичайний темп -- 60 мс/група; жоден
 * реальний DMR-кадр не запізниться на 500 мс сам собою, а пауза між двома
 * різними натисканнями PTT на практиці помітно довша за це. НЕ перевірено на
 * залізі -- якщо в полі виявиться, що короткі паузи всередині ОДНІЄЇ розмови
 * (наприклад, late entry) хибно рвуть її на "різні" заходи, поріг варто
 * підняти; дивись TESTING.md, блок I. */
#define CALL_REPLAY_OVER_GAP_MS         500U

typedef struct
{
	uint16_t writeIdx;      // куди писати НАСТУПНУ групу (фізичний індекс кільця)
	uint16_t count;         // скільки груп зараз дійсні (0..CALL_REPLAY_GROUPS)
	uint32_t lastCaptureMs; // ticksGetMillis() останнього ДІЙСНО записаного кадру
	bool     hasCaptured;   // false до першого кадру після callReplayInit()
} callReplayCaptureState_t;

/* Усе -- у CCM (той самий буфер, що звільнила a2211ac/docs/notification-buffer.md):
 * основної RAM вільно лише ~212 Б (docs/recording-feasibility.md), туди не влазить
 * навіть службовий стан, не кажучи вже про сам кільцевий буфер. */
static __attribute__((section(".ccmram"))) uint8_t callReplayRing[CALL_REPLAY_GROUPS][CALL_REPLAY_GROUP_BYTES];
/* Один біт на групу: чи ця група -- перша в новому "заході". (CALL_REPLAY_GROUPS+7)/8 = 63 Б. */
static __attribute__((section(".ccmram"))) uint8_t callReplayOverBit[(CALL_REPLAY_GROUPS + 7U) / 8U];
static __attribute__((section(".ccmram"))) callReplayCaptureState_t callReplayCap;

static inline void callReplayBitSet(uint8_t *bits, uint16_t idx)
{
	bits[idx >> 3] = (uint8_t)(bits[idx >> 3] | (uint8_t)(1U << (idx & 7U)));
}

static inline void callReplayBitClear(uint8_t *bits, uint16_t idx)
{
	bits[idx >> 3] = (uint8_t)(bits[idx >> 3] & (uint8_t)~(1U << (idx & 7U)));
}

static inline bool callReplayBitGet(const uint8_t *bits, uint16_t idx)
{
	return ((bits[idx >> 3] & (uint8_t)(1U << (idx & 7U))) != 0U);
}

void callReplayInit(void)
{
	callReplayCap.writeIdx = 0U;
	callReplayCap.count = 0U;
	callReplayCap.lastCaptureMs = 0U;
	callReplayCap.hasCaptured = false;
	memset(callReplayOverBit, 0, sizeof(callReplayOverBit));
}

void callReplayCaptureTick(const uint8_t group[CALL_REPLAY_GROUP_BYTES], bool aesActive, uint32_t nowMs)
{
	uint16_t idx;
	bool isNewOver;

	if (aesActive)
	{
		// Активне дешифрування ЦЬОГО виклику -- у буфер не потрапляє нічого: ні
		// шифротекст, ні розшифрований голос (обґрунтування -- callReplay.h).
		// lastCaptureMs свідомо НЕ чіпаємо: якщо після AES-виклику піде звичайний,
		// розрив у часі (>CALL_REPLAY_OVER_GAP_MS) сам позначить його як новий захід.
		return;
	}

	isNewOver = ((callReplayCap.hasCaptured == false) ||
			((nowMs - callReplayCap.lastCaptureMs) > CALL_REPLAY_OVER_GAP_MS));

	idx = callReplayCap.writeIdx;
	memcpy(callReplayRing[idx], group, CALL_REPLAY_GROUP_BYTES);

	if (isNewOver)
	{
		callReplayBitSet(callReplayOverBit, idx);
	}
	else
	{
		callReplayBitClear(callReplayOverBit, idx);
	}

	callReplayCap.writeIdx = (uint16_t)((idx + 1U) % CALL_REPLAY_GROUPS);
	if (callReplayCap.count < CALL_REPLAY_GROUPS)
	{
		callReplayCap.count++;
	}
	callReplayCap.lastCaptureMs = nowMs;
	callReplayCap.hasCaptured = true;
}

uint32_t callReplayGroupCount(void)
{
	return callReplayCap.count;
}

bool callReplayIsEmpty(void)
{
	return (callReplayCap.count == 0U);
}

uint32_t callReplayAvailableMs(void)
{
	return (callReplayCap.count * 60U);
}

bool callReplayPlaybackGroup(uint32_t playIndex, const uint8_t **outGroup, bool *outIsOverStart)
{
	uint16_t start;
	uint16_t phys;

	if (playIndex >= callReplayCap.count)
	{
		return false;
	}

	// Найстаріша ЖИВА група -- завжди (writeIdx - count) за модулем розміру кільця,
	// і для випадку "кільце ще не заповнилось" (count < GROUPS, writeIdx == count)
	// це коректно дає 0 -- запис справді почався з фізичного нуля.
	start = (uint16_t)((callReplayCap.writeIdx + CALL_REPLAY_GROUPS - callReplayCap.count) % CALL_REPLAY_GROUPS);
	phys = (uint16_t)((start + playIndex) % CALL_REPLAY_GROUPS);

	*outGroup = callReplayRing[phys];
	*outIsOverStart = callReplayBitGet(callReplayOverBit, phys);
	return true;
}

uint16_t callReplayOldestPhysIndex(void)
{
	// Той самий вираз, що "start" вище -- окрема функція лише тому, що рушію
	// відтворення (callReplayPlayback.c) потрібно взяти це значення ОДИН РАЗ на
	// старті, а не перераховувати відносно живого стану на кожному кадрі
	// (дивись великий коментар у callReplay.h біля цієї функції).
	return (uint16_t)((callReplayCap.writeIdx + CALL_REPLAY_GROUPS - callReplayCap.count) % CALL_REPLAY_GROUPS);
}

void callReplayRawGroupAt(uint16_t physIdx, const uint8_t **outGroup, bool *outIsOverStart)
{
	uint16_t phys = (uint16_t)(physIdx % CALL_REPLAY_GROUPS);

	*outGroup = callReplayRing[phys];
	*outIsOverStart = callReplayBitGet(callReplayOverBit, phys);
}

#endif // ENABLE_CALL_REPLAY

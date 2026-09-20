/*
 * test_call_replay.c -- хостовий тест ЧИСТОЇ кільцевої логіки "Переслухати"
 * (functions/callReplay.c), без жодної апаратної залежності. Лінкує РЕАЛЬНИЙ
 * callReplay.c (не копію) -- дивись SRCS у run_tests.sh; той самий файл, що
 * компілюється й у прошивку.
 *
 * Що перевіряємо (за задачею):
 *   1) порожній буфер -- count/AvailableMs/IsEmpty/PlaybackGroup поводяться коректно;
 *   2) звичайне захоплення підряд -- перша група позначена як початок "заходу" (over),
 *      наступні -- ні;
 *   3) розрив у часі (> порогу) -- нова група ПОЗНАЧЕНА як початок нового заходу;
 *   4) AES-активний кадр -- НЕ потрапляє в кільце взагалі (count не росте, вміст не
 *      змінюється), і НЕ зсуває lastCaptureMs (наступний реальний кадр рахує розрив
 *      від ОСТАННЬОГО РЕАЛЬНОГО, а не від пропущеного AES-кадру);
 *   5) переповнення/обгортання -- запис більш ніж CALL_REPLAY_GROUPS груп: count не
 *      росте понад ліміт, найстаріші групи витісняються, playbackGroup(0) завжди
 *      повертає справді найстарішу ЖИВУ групу;
 *   6) callReplayOldestPhysIndex()/callReplayRawGroupAt() (внутрішній шов до
 *      callReplayPlayback.c) дають ТІ САМІ дані, що й callReplayPlaybackGroup(0..).
 *   7) перемикач "Запис RX" (задача 2026-09-19, п.5): типово увімкнено одразу після
 *      callReplayInit(); вимкнення ОДРАЗУ спорожнює кільце (не лишає старий запис);
 *      захоплення -- НІЧОГО не пише, поки вимкнено; повторне увімкнення відновлює
 *      звичайне захоплення "з чистого аркуша".
 *   8) callReplayFindLastOverStart() (задача 2026-09-20, режим "Останній перехід"):
 *      порожній буфер -> 0; один перехід -> playIndex 0; кілька переходів -> playIndex
 *      САМЕ останнього (не першого/середнього); фізичне обгортання кільця ПОСЕРЕД
 *      останнього переходу -- пошук за ЛОГІЧНИМ playIndex не плутається з фізичним
 *      wrap-around; останній перехід ДОВШИЙ за саму ємність кільця (справжній початок
 *      уже витіснено) -> fallback 0 ("грати все, що є, від найстарішого").
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "functions/callReplay.h"

static int fails = 0;

static void expect(const char *what, int cond)
{
	if (cond)
	{
		printf("  ok   %s\n", what);
	}
	else
	{
		fails++;
		printf("  FAIL %s\n", what);
	}
}

/* Група з упізнаваним вмістом: перші 4 байти -- 32-бітний номер (big-endian), решта -- 0.
 * Дозволяє після обгортання перевірити, що playbackGroup(i) повертає РІВНО той запис,
 * що очікується, а не сусідній фізичний слот. */
static void makeGroup(uint8_t out[CALL_REPLAY_GROUP_BYTES], uint32_t seq)
{
	memset(out, 0, CALL_REPLAY_GROUP_BYTES);
	out[0] = (uint8_t)(seq >> 24);
	out[1] = (uint8_t)(seq >> 16);
	out[2] = (uint8_t)(seq >> 8);
	out[3] = (uint8_t)(seq);
}

static uint32_t groupSeq(const uint8_t *g)
{
	return (((uint32_t)g[0] << 24) | ((uint32_t)g[1] << 16) | ((uint32_t)g[2] << 8) | (uint32_t)g[3]);
}

static void test_empty(void)
{
	printf("1) порожній буфер:\n");
	callReplayInit();

	expect("IsEmpty() == true", callReplayIsEmpty());
	expect("GroupCount() == 0", callReplayGroupCount() == 0U);
	expect("AvailableMs() == 0", callReplayAvailableMs() == 0U);

	const uint8_t *g;
	bool overStart;
	expect("PlaybackGroup(0) -> false на порожньому", callReplayPlaybackGroup(0, &g, &overStart) == false);
	expect("PlaybackGroup(будь-який) -> false на порожньому", callReplayPlaybackGroup(41, &g, &overStart) == false);
}

static void test_basic_capture_and_over_marking(void)
{
	printf("2) звичайне захоплення -- межі заходів:\n");
	callReplayInit();

	uint8_t buf[CALL_REPLAY_GROUP_BYTES];
	uint32_t t = 1000U;

	/* 5 груп поспіль, крок 60 мс (звичайний темп) -- один "захід". */
	for (uint32_t i = 0; i < 5U; i++)
	{
		makeGroup(buf, 100U + i);
		callReplayCaptureTick(buf, false, t);
		t += 60U;
	}
	expect("GroupCount() == 5", callReplayGroupCount() == 5U);
	expect("AvailableMs() == 300", callReplayAvailableMs() == 300U);
	expect("IsEmpty() == false", callReplayIsEmpty() == false);

	const uint8_t *g;
	bool overStart;
	expect("playbackGroup(0) існує", callReplayPlaybackGroup(0, &g, &overStart));
	expect("playbackGroup(0).seq == 100", groupSeq(g) == 100U);
	expect("playbackGroup(0) -- початок заходу (перший кадр)", overStart == true);

	for (uint32_t i = 1; i < 5U; i++)
	{
		expect("playbackGroup(i) існує", callReplayPlaybackGroup(i, &g, &overStart));
		expect("playbackGroup(i).seq коректна", groupSeq(g) == (100U + i));
		expect("playbackGroup(i), i>0, у тому самому заході -- НЕ початок", overStart == false);
	}

	expect("playbackGroup(5) -- за межею (count==5)", callReplayPlaybackGroup(5, &g, &overStart) == false);
}

static void test_over_gap_boundary(void)
{
	printf("3) розрив у часі -- новий захід:\n");
	callReplayInit();

	uint8_t buf[CALL_REPLAY_GROUP_BYTES];
	makeGroup(buf, 1U);
	callReplayCaptureTick(buf, false, 0U);       // перший кадр -- завжди початок заходу
	makeGroup(buf, 2U);
	callReplayCaptureTick(buf, false, 400U);     // розрив 400 мс (<=500) -- той самий захід
	makeGroup(buf, 3U);
	callReplayCaptureTick(buf, false, 400U + 501U); // розрив 501 мс (>500) -- НОВИЙ захід

	expect("GroupCount() == 3", callReplayGroupCount() == 3U);

	const uint8_t *g;
	bool overStart;
	callReplayPlaybackGroup(0, &g, &overStart);
	expect("група 0 -- початок заходу", overStart == true);
	callReplayPlaybackGroup(1, &g, &overStart);
	expect("група 1 (розрив 400мс) -- НЕ новий захід", overStart == false);
	callReplayPlaybackGroup(2, &g, &overStart);
	expect("група 2 (розрив 501мс) -- новий захід", overStart == true);
}

static void test_aes_skip(void)
{
	printf("4) AES-активний кадр не записується:\n");
	callReplayInit();

	uint8_t buf[CALL_REPLAY_GROUP_BYTES];
	makeGroup(buf, 1U);
	callReplayCaptureTick(buf, false, 0U);
	expect("count == 1 після 1 реального кадру", callReplayGroupCount() == 1U);

	/* Кадр AES-виклику -- має бути повністю проігнорований (не зʼявитись у кільці). */
	makeGroup(buf, 999U);
	callReplayCaptureTick(buf, true, 100U);
	expect("count не змінився після AES-кадру", callReplayGroupCount() == 1U);

	const uint8_t *g;
	bool overStart;
	callReplayPlaybackGroup(0, &g, &overStart);
	expect("вміст кільця -- досі перший (не AES) кадр", groupSeq(g) == 1U);

	/* lastCaptureMs НЕ зсунувся AES-кадром (callReplay.c, коментар у callReplayCaptureTick):
	 * наступний РЕАЛЬНИЙ кадр рахує розрив від t=0 (останній справжній), а не від t=100
	 * (пропущений AES). Розрив від t=0 до t=600 -- 600мс (>500) -- новий захід. Якби
	 * lastCaptureMs зсунувся AES-кадром, розрив рахувався б від t=100 (теж >500 у цьому
	 * прикладі) -- тож підберемо межове значення, що розрізняє обидві поведінки: розрив
	 * РІВНО 550 мс від t=0 (>500 -> новий захід), але лише 450 мс від t=100 (<=500 -> той
	 * самий захід). Якщо lastCaptureMs помилково зсунеться AES-кадром, цей кадр НЕ буде
	 * позначений як новий захід -- тест це спіймає. */
	makeGroup(buf, 2U);
	callReplayCaptureTick(buf, false, 550U);
	expect("count == 2 після другого реального кадру", callReplayGroupCount() == 2U);
	callReplayPlaybackGroup(1, &g, &overStart);
	expect("розрив рахується від ОСТАННЬОГО РЕАЛЬНОГО кадру (t=0), не від AES-кадру (t=100)",
			overStart == true);
}

static void test_overflow_wraparound(void)
{
	printf("5) переповнення/обгортання кільця:\n");
	callReplayInit();

	uint8_t buf[CALL_REPLAY_GROUP_BYTES];
	uint32_t t = 0U;
	const uint32_t extra = 137U; /* скільки груп понад ємність кільця запишемо */
	const uint32_t total = CALL_REPLAY_GROUPS + extra;

	for (uint32_t i = 0; i < total; i++)
	{
		makeGroup(buf, i);
		callReplayCaptureTick(buf, false, t);
		t += 60U; /* крок менший за поріг заходу -- усе один довгий "виклик" */
	}

	expect("GroupCount() не перевищує ємність", callReplayGroupCount() == CALL_REPLAY_GROUPS);
	expect("AvailableMs() відповідає повному кільцю", callReplayAvailableMs() == (CALL_REPLAY_GROUPS * 60U));

	const uint8_t *g;
	bool overStart;

	/* Найстаріша ЖИВА група -- це seq == extra (перші `extra` витіснені). */
	expect("playbackGroup(0) існує після переповнення", callReplayPlaybackGroup(0, &g, &overStart));
	expect("playbackGroup(0) -- саме найстаріша ЖИВА (витіснені перші extra)", groupSeq(g) == extra);

	/* Найновіша -- останній записаний. */
	expect("playbackGroup(count-1) існує", callReplayPlaybackGroup(CALL_REPLAY_GROUPS - 1U, &g, &overStart));
	expect("playbackGroup(count-1) -- остання записана", groupSeq(g) == (total - 1U));

	/* За межею -- як і до переповнення. */
	expect("playbackGroup(count) -- за межею", callReplayPlaybackGroup(CALL_REPLAY_GROUPS, &g, &overStart) == false);

	/* Внутрішній шов до callReplayPlayback.c: OldestPhysIndex()/RawGroupAt() мають
	 * повертати РІВНО те саме, що PlaybackGroup(0) -- саме на цій рівності тримається
	 * заморожене вікно відтворення (callReplay.h, великий коментар). */
	uint16_t oldestPhys = callReplayOldestPhysIndex();
	const uint8_t *rawG;
	bool rawOverStart;
	callReplayRawGroupAt(oldestPhys, &rawG, &rawOverStart);
	const uint8_t *pbG;
	bool pbOverStart;
	callReplayPlaybackGroup(0, &pbG, &pbOverStart);
	expect("RawGroupAt(OldestPhysIndex()) вміст == PlaybackGroup(0)", groupSeq(rawG) == groupSeq(pbG));
	expect("RawGroupAt(OldestPhysIndex()) overStart == PlaybackGroup(0)", rawOverStart == pbOverStart);
}

static void test_recording_enable_toggle(void)
{
	printf("7) перемикач \"Запис RX\":\n");
	callReplayInit();

	expect("IsRecordingEnabled() == true одразу після Init()", callReplayIsRecordingEnabled());

	uint8_t buf[CALL_REPLAY_GROUP_BYTES];
	makeGroup(buf, 1U);
	callReplayCaptureTick(buf, false, 0U);
	makeGroup(buf, 2U);
	callReplayCaptureTick(buf, false, 60U);
	expect("захоплення працює, поки увімкнено", callReplayGroupCount() == 2U);

	callReplaySetRecordingEnabled(false);
	expect("IsRecordingEnabled() == false одразу після вимкнення", callReplayIsRecordingEnabled() == false);
	expect("буфер ОДРАЗУ спорожнів після вимкнення (не лишає старий запис)", callReplayIsEmpty());
	expect("GroupCount() == 0 одразу після вимкнення", callReplayGroupCount() == 0U);

	/* Поки вимкнено -- жодна спроба захоплення нічого не пише. */
	makeGroup(buf, 3U);
	callReplayCaptureTick(buf, false, 120U);
	makeGroup(buf, 4U);
	callReplayCaptureTick(buf, false, 180U);
	expect("захоплення -- НІЧОГО не пише, поки вимкнено (спроба 1)", callReplayGroupCount() == 0U);
	expect("буфер лишається порожнім, поки вимкнено (спроба 2)", callReplayIsEmpty());

	/* Повторне увімкнення -- звичайне захоплення відновлюється з чистого аркуша. */
	callReplaySetRecordingEnabled(true);
	expect("IsRecordingEnabled() == true після повторного увімкнення", callReplayIsRecordingEnabled());
	expect("буфер досі порожній одразу після увімкнення (нічого не з'явилось саме собою)", callReplayIsEmpty());

	makeGroup(buf, 5U);
	callReplayCaptureTick(buf, false, 1000U);
	expect("захоплення відновилось після повторного увімкнення", callReplayGroupCount() == 1U);

	const uint8_t *g;
	bool overStart;
	callReplayPlaybackGroup(0, &g, &overStart);
	expect("новий запис -- новий захід (перший кадр з чистого аркуша)", overStart == true);
	expect("вміст -- саме кадр, записаний ПІСЛЯ повторного увімкнення", groupSeq(g) == 5U);
}

static void test_find_last_over_start(void)
{
	printf("8) пошук початку останнього переходу (callReplayFindLastOverStart, задача 2026-09-20):\n");

	/* 8a) порожній буфер -- 0, як і задокументовано (викликач зобов'язаний сам
	 * перевірити callReplayIsEmpty() перед стартом, ця функція лише не падає). */
	callReplayInit();
	expect("порожній буфер -> 0", callReplayFindLastOverStart() == 0U);

	/* 8b) один-єдиний перехід -- початок останнього переходу == початок ЄДИНОГО
	 * (playIndex 0). */
	{
		uint8_t buf[CALL_REPLAY_GROUP_BYTES];
		uint32_t t = 0U;

		callReplayInit();
		for (uint32_t i = 0; i < 5U; i++)
		{
			makeGroup(buf, i);
			callReplayCaptureTick(buf, false, t);
			t += 60U;
		}
		expect("один перехід -> playIndex 0", callReplayFindLastOverStart() == 0U);
	}

	/* 8c) кілька переходів -- має повернути playIndex САМЕ ОСТАННЬОГО (найновішого),
	 * а не першого чи середнього. */
	{
		uint8_t buf[CALL_REPLAY_GROUP_BYTES];
		uint32_t t = 0U;

		callReplayInit();
		/* Перехід 1: playIndex 0..2 (3 групи). */
		for (uint32_t i = 0; i < 3U; i++)
		{
			makeGroup(buf, i);
			callReplayCaptureTick(buf, false, t);
			t += 60U;
		}
		t += 600U; /* розрив > порогу -- новий перехід */
		/* Перехід 2: playIndex 3..6 (4 групи). */
		for (uint32_t i = 0; i < 4U; i++)
		{
			makeGroup(buf, 100U + i);
			callReplayCaptureTick(buf, false, t);
			t += 60U;
		}
		t += 600U;
		/* Перехід 3 (останній): playIndex 7..9 (3 групи). */
		for (uint32_t i = 0; i < 3U; i++)
		{
			makeGroup(buf, 200U + i);
			callReplayCaptureTick(buf, false, t);
			t += 60U;
		}

		expect("GroupCount() == 10 (3+4+3)", callReplayGroupCount() == 10U);

		uint32_t lastStart = callReplayFindLastOverStart();
		expect("кілька переходів -> playIndex саме ОСТАННЬОГО заходу (== 7)", lastStart == 7U);

		const uint8_t *g;
		bool overStart;
		callReplayPlaybackGroup(lastStart, &g, &overStart);
		expect("знайдений playIndex дійсно позначений як початок заходу", overStart == true);
		expect("знайдений playIndex -- перша група ТРЕТЬОГО переходу (seq 200)", groupSeq(g) == 200U);
	}

	/* 8d) обгортання кільця ПОСЕРЕД останнього переходу -- заповнюємо кільце
	 * "розігрівом" (окремий перехід, який ЦІЛКОМ витісниться), тоді короткий
	 * перехід, тоді довгий ОСТАННІЙ перехід, що обгортає кільце (пише за
	 * фізичний кінець масиву назад на початок), лишаючи в живому вікні лише
	 * "хвіст" розігріву + короткий перехід + весь останній. Перевіряємо, що
	 * пошук не плутається з фізичним wrap-around, бо ходить по ЛОГІЧНОМУ
	 * playIndex (0..count-1), а не по фізичних індексах. */
	{
		uint8_t buf[CALL_REPLAY_GROUP_BYTES];
		uint32_t t = 0U;
		uint32_t seq = 0U;

		callReplayInit();

		/* "Розігрів" -- один довгий перехід трохи більший за половину кільця,
		 * щоб після другого такого ж переходу фізичний writeIdx устиг кілька
		 * разів пройти через кінець масиву (реальне обгортання), перш ніж
		 * почнеться короткий і останній переходи нижче. */
		for (uint32_t i = 0; i < (CALL_REPLAY_GROUPS + (CALL_REPLAY_GROUPS / 2U)); i++)
		{
			makeGroup(buf, seq++);
			callReplayCaptureTick(buf, false, t);
			t += 60U;
		}
		t += 600U; /* новий перехід -- короткий, 5 груп */
		for (uint32_t i = 0; i < 5U; i++)
		{
			makeGroup(buf, seq++);
			callReplayCaptureTick(buf, false, t);
			t += 60U;
		}
		t += 600U; /* ОСТАННІЙ перехід -- довший за половину кільця, тож ГАРАНТОВАНО
		            * обгортає фізичний масив хоч раз під час запису САМЕ ЦЬОГО переходу. */
		uint32_t lastTransitionLen = (CALL_REPLAY_GROUPS / 2U) + 10U;
		uint32_t lastTransitionFirstSeq = seq;
		for (uint32_t i = 0; i < lastTransitionLen; i++)
		{
			makeGroup(buf, seq++);
			callReplayCaptureTick(buf, false, t);
			t += 60U;
		}

		expect("кільце заповнене (переповнення відбулось)", callReplayGroupCount() == CALL_REPLAY_GROUPS);

		uint32_t lastStart = callReplayFindLastOverStart();
		uint32_t expectedStart = (CALL_REPLAY_GROUPS - lastTransitionLen);

		expect("обгортання ПОСЕРЕД останнього переходу -- playIndex все одно коректний",
				lastStart == expectedStart);

		const uint8_t *g;
		bool overStart;
		callReplayPlaybackGroup(lastStart, &g, &overStart);
		expect("знайдена група -- дійсно позначена як початок заходу", overStart == true);
		expect("знайдена група -- саме ПЕРША група останнього переходу", groupSeq(g) == lastTransitionFirstSeq);

		/* Останній playIndex (найновіша група) -- останній записаний кадр. */
		callReplayPlaybackGroup(CALL_REPLAY_GROUPS - 1U, &g, &overStart);
		expect("найновіша група в живому вікні -- справді останній записаний кадр",
				groupSeq(g) == (seq - 1U));
	}

	/* 8e) останній перехід ДОВШИЙ за саму ємність кільця -- справжній початок
	 * заходу давно витіснено, isOverStart==true в живому вікні не знайти
	 * ВЗАГАЛІ (перша жива група -- це вже СЕРЕДИНА того самого нескінченного
	 * заходу, а не його початок) -- очікуємо fallback 0 ("грати все, що є"). */
	{
		uint8_t buf[CALL_REPLAY_GROUP_BYTES];
		uint32_t t = 0U;
		const uint32_t total = CALL_REPLAY_GROUPS + 50U; /* один довжелезний захід, без розривів */

		callReplayInit();
		for (uint32_t i = 0; i < total; i++)
		{
			makeGroup(buf, i);
			callReplayCaptureTick(buf, false, t);
			t += 60U; /* крок менший за поріг -- усе ОДИН захід */
		}

		expect("кільце заповнене", callReplayGroupCount() == CALL_REPLAY_GROUPS);
		expect("перехід довший за буфер -> fallback 0 (грати все, що є)",
				callReplayFindLastOverStart() == 0U);

		const uint8_t *g;
		bool overStart;
		callReplayPlaybackGroup(0, &g, &overStart);
		expect("playIndex 0 у цьому випадку -- НЕ початок заходу (справжній початок витіснено)",
				overStart == false);
	}
}

int main(void)
{
	printf("test_call_replay:\n");

	test_empty();
	test_basic_capture_and_over_marking();
	test_over_gap_boundary();
	test_aes_skip();
	test_overflow_wraparound();
	test_recording_enable_toggle();
	test_find_last_over_start();

	printf(fails ? "ПРОВАЛ (%d)\n" : "ПРОЙДЕНО\n", fails);
	return fails ? 1 : 0;
}

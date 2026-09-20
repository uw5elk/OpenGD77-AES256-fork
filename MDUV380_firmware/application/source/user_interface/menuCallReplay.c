/*
 * Форк (2026-09-19). Екран "Переслухати" -- один простий screen (той самий
 * патерн, що uiPowerOff.c: нема списку, лише ЗЕЛЕНА старт/стоп і ЧЕРВОНА вихід),
 * а не список-меню (menuDisplayMenuList) -- тут нема чого гортати. Уся логіка
 * запису/відтворення -- у functions/callReplay.c; цей файл лише малює й
 * реагує на кнопки. Увесь файл під ENABLE_CALL_REPLAY -- на byte-identical
 * типову збірку не впливає (порожній translation unit без прапорця).
 */
#include "user_interface/uiGlobals.h"

#if defined(ENABLE_CALL_REPLAY)

#include "user_interface/menuSystem.h"
#include "user_interface/uiUtilities.h"
#include "user_interface/uiLocalisation.h"
#include "functions/callReplay.h"

static menuStatus_t menuCallReplayExitCode = MENU_STATUS_SUCCESS;
static void updateScreen(bool forceRedraw);
static void handleEvent(uiEvent_t *ev);

/* Вибір режиму для НАСТУПНОГО старту з ЦЬОГО екрана (задача 2026-09-20, п.2).
 * Файлова статична (не локальна в menuCallReplay()) -- потрібна і handleEvent()
 * (перемикання LEFT/RIGHT, читання в GREEN), і updateScreen() (підпис на
 * картці), а не лише самій menuCallReplay(), на відміну від m/wasPlaying нижче.
 * НЕ скидається при вході в екран (isFirstRun) -- вибір лишається, поки рація
 * ввімкнена, перепитувати щоразу незручно. Типове значення false ("Останні
 * ~30 с") -- та сама поведінка, що була ЄДИНОЮ до цієї задачі, тож перший вхід
 * у меню нічого не змінює для тих, хто ще не торкався LEFT/RIGHT.
 *
 * ЧОМУ LEFT/RIGHT, а не перетворення екрана на список (menuDisplayMenuList):
 * той самий принцип, що й для вимикача "Запис RX" (він пішов у
 * menuSoundOptions.c, а не сюди) -- тут усього ДВА взаємовиключні стани, а не
 * список пунктів для гортання; список-меню додав би окремий екран заради
 * одного bool і зайву навігацію (Зелена, щоб увійти в пункт, стрілки, щоб
 * вибрати, Зелена ще раз, щоб підтвердити) там, де досить одного натискання.
 * LEFT/RIGHT -- стандартний OpenGD77-жест "перемкнути значення на місці",
 * яким уже користуються числові пункти меню налаштувань (та сама клавіатурна
 * розкладка тут вільна: KEY_RED/KEY_GREEN зайняті під вихід/старт-стоп,
 * дивись handleEvent() нижче, а KEY_LEFT/KEY_RIGHT цим екраном раніше не
 * використовувались узагалі). */
static bool selectedLastTransitionMode = false;

menuStatus_t menuCallReplay(uiEvent_t *ev, bool isFirstRun)
{
	static uint32_t m = 0;
	static bool wasPlaying = false;

	if (isFirstRun)
	{
		menuDataGlobal.numItems = 0;
		wasPlaying = false;
		updateScreen(true);
	}
	else
	{
		menuCallReplayExitCode = MENU_STATUS_SUCCESS;

		if ((ev->events & FUNCTION_EVENT) && (ev->function == FUNC_REDRAW))
		{
			updateScreen(true);
			return menuCallReplayExitCode;
		}

		if (ev->hasEvent)
		{
			handleEvent(ev);
		}

		if (callReplayIsPlaying())
		{
			// Поки триває відтворення, оновлюємо прогрес -- той самий темп, що й
			// RSSI-екран (RSSI_UPDATE_COUNTER_RELOAD, ~200 мс).
			if ((ev->time - m) > RSSI_UPDATE_COUNTER_RELOAD)
			{
				m = ev->time;
				updateScreen(false);
			}
			wasPlaying = true;
		}
		else if (wasPlaying)
		{
			// Відтворення щойно САМОСТІЙНО зупинилось (кінець буфера, вхідний виклик,
			// PTT -- callReplay.c), без натискання нашої кнопки -- підхопити це на
			// екрані одразу, а не чекати на подію.
			wasPlaying = false;
			updateScreen(true);
		}
	}

	return menuCallReplayExitCode;
}

static void updateScreen(bool forceRedraw)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];

	if (forceRedraw)
	{
		displayClearBuf();
		menuDisplayTitle(currentLanguage->call_replay);
	}
	else
	{
		// Лише тіло, не чіпаючи заголовок/шапку.
		displayFillRect(0, 24, DISPLAY_SIZE_X, (DISPLAY_SIZE_Y - 24), true);
	}

	if (callReplayIsRecordingEnabled() == false)
	{
		// Перевірити ПЕРШИМ, до callReplayIsEmpty(): вимкнений запис завжди означає й
		// порожній буфер (callReplaySetRecordingEnabled(false) спорожняє його одразу,
		// дивись callReplay.c), але користувачу треба бачити ПРИЧИНУ ("вимкнено"), а
		// не загальне "немає запису" -- інакше виглядає як апаратна проблема (задача,
		// п.5).
		displayPrintCentered(52, (char *)currentLanguage->call_replay_disabled, FONT_SIZE_3);
	}
	else if (callReplayIsEmpty())
	{
		displayPrintCentered(52, (char *)currentLanguage->call_replay_empty, FONT_SIZE_3);
	}
	else if (callReplayIsPlaying())
	{
		uint32_t playedS = (callReplayPlayedMs() / 1000U);
		uint32_t totalS = ((callReplayPlayTotalMs() + 999U) / 1000U); // округлення вгору, як і нижче

		// Режим АКТИВНОЇ сесії (callReplayIsLastTransitionMode()), а не поточний
		// вибір на екрані (selectedLastTransitionMode могли встигнути перемкнути
		// вже ПІД ЧАС відтворення -- LEFT/RIGHT нижче це ігнорує, але про всяк
		// випадок читаємо саме те, що РЕАЛЬНО грає, задача п.2 "Картка на старті
		// показує, який режим грає").
		displayPrintCentered(40, (char *)(callReplayIsLastTransitionMode() ?
				currentLanguage->call_replay_mode_last : currentLanguage->call_replay_mode_all), FONT_SIZE_3);
		// Навмисно ASCII ("/", "s"), не кирилиця -- check_string_encoding.py дозволяє
		// кирилицю лише в languages/*_ua.h, а не в .c (обґрунтування -- сам скрипт).
		snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%lu/%lus", (unsigned long)playedS, (unsigned long)totalS);
		displayPrintCentered(64, buffer, FONT_SIZE_4);
	}
	else
	{
		// Округлення вгору -- "0s" для непорожнього буфера виглядало б як помилка.
		uint32_t availS = ((callReplayAvailableMs() + 999U) / 1000U);

		// Обраний (LEFT/RIGHT) режим для НАСТУПНОГО старту -- окремим рядком над
		// довжиною запису (задача, п.2: два пункти меню/вибір режиму, і видно,
		// що саме буде грати при натисканні GREEN/коротке SK1/довге SK1). Той
		// самий y=40/FONT_SIZE_3 + y=64/FONT_SIZE_4 макет, що й у гілці
		// "відтворюється" вище (FONT_SIZE_3_HEIGHT=16 -> рядок на y=40 займає
		// 40..56, FONT_SIZE_4_HEIGHT=32 -> рядок на y=64 займає 64..96, розрив
		// 8px; старий y=52 для самого лише "Xs" тут більше не підходить --
		// перекрився б із новим рядком режиму).
		displayPrintCentered(40, (char *)(selectedLastTransitionMode ?
				currentLanguage->call_replay_mode_last : currentLanguage->call_replay_mode_all), FONT_SIZE_3);
		snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%lus", (unsigned long)availS);
		displayPrintCentered(64, buffer, FONT_SIZE_4);
	}

	displayRender();
}

static void handleEvent(uiEvent_t *ev)
{
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		// ЧЕРВОНА -- і перериває відтворення (якщо триває), і виходить з екрана
		// одним натисканням (п. "Переривати відтворення: ... будь-яка кнопка виходу").
		callReplayStop();
		menuSystemPopPreviousMenu();
		return;
	}

	// LEFT/RIGHT -- перемкнути обраний режим для наступного старту (задача
	// 2026-09-20, п.2). Ігнорується ПІД ЧАС відтворення -- міняти вибір, поки
	// щось уже грає, лише плутало б (те, що грає, і так завжди показує СВІЙ
	// РЕАЛЬНИЙ режим окремо, callReplayIsLastTransitionMode() в updateScreen()
	// вище, незалежно від цього перемикача).
	if ((KEYCHECK_SHORTUP(ev->keys, KEY_LEFT) || KEYCHECK_SHORTUP(ev->keys, KEY_RIGHT)) &&
			(callReplayIsPlaying() == false))
	{
		selectedLastTransitionMode = !selectedLastTransitionMode;
		updateScreen(true);
		return;
	}

	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		if (callReplayIsPlaying())
		{
			callReplayStop();
		}
		else if (callReplayIsRecordingEnabled() == false)
		{
			// Той самий пріоритет, що в updateScreen() вище -- сказати ЧОМУ порожньо.
			uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 2000,
					currentLanguage->call_replay_disabled, true);
		}
		else if (callReplayIsEmpty())
		{
			uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 2000,
					currentLanguage->call_replay_empty, true);
		}
		else if (selectedLastTransitionMode)
		{
			(void)callReplayStartLastTransition(); // false лише якщо зараз прийом/передача
		}
		else
		{
			(void)callReplayStart(); // false лише якщо зараз прийом/передача -- тоді просто нічого не станеться
		}

		updateScreen(true);
	}
}

#endif // ENABLE_CALL_REPLAY

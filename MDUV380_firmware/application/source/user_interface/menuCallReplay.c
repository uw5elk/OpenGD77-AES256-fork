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

	if (callReplayIsEmpty())
	{
		displayPrintCentered(52, (char *)currentLanguage->call_replay_empty, FONT_SIZE_3);
	}
	else if (callReplayIsPlaying())
	{
		uint32_t playedS = (callReplayPlayedMs() / 1000U);
		uint32_t totalS = ((callReplayPlayTotalMs() + 999U) / 1000U); // округлення вгору, як і нижче

		displayPrintCentered(40, (char *)currentLanguage->call_replay_playing, FONT_SIZE_3);
		// Навмисно ASCII ("/", "s"), не кирилиця -- check_string_encoding.py дозволяє
		// кирилицю лише в languages/*_ua.h, а не в .c (обґрунтування -- сам скрипт).
		snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%lu/%lus", (unsigned long)playedS, (unsigned long)totalS);
		displayPrintCentered(64, buffer, FONT_SIZE_4);
	}
	else
	{
		// Округлення вгору -- "0s" для непорожнього буфера виглядало б як помилка.
		uint32_t availS = ((callReplayAvailableMs() + 999U) / 1000U);

		snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%lus", (unsigned long)availS);
		displayPrintCentered(52, buffer, FONT_SIZE_4);
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

	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		if (callReplayIsPlaying())
		{
			callReplayStop();
		}
		else if (callReplayIsEmpty())
		{
			uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 2000,
					currentLanguage->call_replay_empty, true);
		}
		else
		{
			(void)callReplayStart(); // false лише якщо зараз прийом/передача -- тоді просто нічого не станеться
		}

		updateScreen(true);
	}
}

#endif // ENABLE_CALL_REPLAY

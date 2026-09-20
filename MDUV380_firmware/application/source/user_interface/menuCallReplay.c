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
static bool handleEvent(uiEvent_t *ev);

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
			// Форк (2026-09-20, картка "Переслухати"): handleEvent() повертає true,
			// коли КРАСНА вже вивела нас з цього екрана (callReplayStop() +
			// menuSystemPopPreviousMenu() -- останній усередині сам малює й штовхає
			// НОВИЙ поточний екран, VFO/канал/меню, через menuSystemPushMenuFirstRun()).
			// Без цієї перевірки блок wasPlaying нижче однаково спрацював би на ЦЬОМУ
			// ж тіку (callReplayIsPlaying() уже false після стопу) і своїм
			// updateScreen(true) переписав би щойно правильно намальований НОВИЙ
			// екран назад на "Переслухати" -- реальний баг, не повʼязаний із самою
			// карткою, але вперше видимий саме зараз, коли стоп синхронно тягне за
			// собою uiNotificationHide(true) (теж перемальовує поточний екран).
			if (handleEvent(ev))
			{
				return menuCallReplayExitCode;
			}
		}

		if (callReplayIsPlaying())
		{
			// Поки триває відтворення, екран належить повноекранній картці
			// NOTIFICATION_TYPE_CALL_REPLAY (uiNotification.c) -- вона сама оновлюється
			// з callReplayTick(). updateScreen(false) тут лишень тримає m/wasPlaying
			// узгодженими; сама функція нічого не малює, поки грає (див. нижче).
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
			// екрані одразу, а не чекати на подію. (Стоп через RED сюди вже не
			// доходить -- обробляється вище через ранній return.)
			wasPlaying = false;
			updateScreen(true);
		}
	}

	return menuCallReplayExitCode;
}

static void updateScreen(bool forceRedraw)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];

	if (callReplayIsPlaying())
	{
		// Форк (2026-09-20, картка "Переслухати", варіант A): під час відтворення
		// екран повністю належить повноекранній картці NOTIFICATION_TYPE_CALL_REPLAY
		// (displayLevelCard(), uiNotification.c) -- вона сама раз на ~300 мс
		// оновлюється з callReplayTick(). Малювати тут щось СВОЄ (як робив старий
		// рядок "режим" + "зіграно/усього") означало б дублювати ту саму інформацію
		// другим шляхом малювання, а displayRender() нижче все одно був би
		// no-op'ом, поки картка видима (HX8353E_display.c) -- лише зайва робота
		// щотіку. Нічого малювати не треба.
		return;
	}

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

/* Повертає true, якщо цей виклик УЖЕ вивів нас з екрана "Переслухати"
 * (menuSystemPopPreviousMenu() всередині KEY_RED -- новий поточний екран,
 * VFO/канал/меню, уже намальований і штовхнутий на LCD, дивись
 * menuSystemPushMenuFirstRun()). Викликач (menuCallReplay()) у цьому випадку
 * має одразу вийти, не чіпаючи екран далі -- задача 2026-09-20. */
static bool handleEvent(uiEvent_t *ev)
{
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		// ЧЕРВОНА -- і перериває відтворення (якщо триває), і виходить з екрана
		// одним натисканням (п. "Переривати відтворення: ... будь-яка кнопка виходу").
		callReplayStop();
		menuSystemPopPreviousMenu();
		return true;
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
		return false;
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

		// Форк: якщо цей GREEN щойно СТАРТУВАВ відтворення -- картка
		// (uiNotificationShow(..., true) усередині callReplayStartAt()) уже
		// показана й уже владіє екраном, тож updateScreen(true) тут одразу
		// повертається без малювання (callReplayIsPlaying()==true, дивись
		// updateScreen() вище) -- не зайвий виклик, а дешевий no-op. Якщо
		// натомість ЗУПИНИВ (перша гілка вище) -- callReplayStop() уже встиг
		// через uiNotificationHide(true) перемалювати цей самий екран
		// (ми й далі на "Переслухати", RED сюди не доходить), тож цей виклик
		// повторює те саме малювання -- не баг, лише один зайвий кадр раз на
		// натискання, не щотіку.
		updateScreen(true);
	}

	return false;
}

#endif // ENABLE_CALL_REPLAY

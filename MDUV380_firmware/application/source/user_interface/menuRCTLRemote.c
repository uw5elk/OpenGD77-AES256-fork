/*
 * menuRCTLRemote.c — "Від. керування" / "Remote control": надіслати команду керування
 * іншій рації У СТОКОВОМУ ФОРМАТІ TYT (Motorola CSBK), щоб заводська рація нас розуміла.
 *
 * Потік (модельовано на menuMessages.c):
 *   PICK_CMD      : обрати команду (Перевірка / Моніторинг / Ввімкнення / Вимкнення);
 *                   індекс пункту == dmr_rctl_stock_cmd_t
 *   ENTRY         : ввести/обрати DMR ID цілі, GRN надсилає обрану команду
 *   PICK_CONTACT  : список приватних контактів кодплагу замість ручного набору ID
 *                   (RCTL — лише індивідуальний виклик, групового режиму немає)
 *   RESULT        : "Надіслано" або помилка надсилання (TX зайнятий / не зібралось)
 *
 * ACK від цілі тут поки НЕ очікується: формат стокової ACK-відповіді ще не реалізовано
 * в RX -- спершу треба захопити його з ефіру (крок 2.5 RCTL_COMPAT.md). Тому екран
 * показує лише факт надсилання; "рація на зв'язку" повернеться, коли з'явиться парсинг ACK.
 *
 * Компілюється в ніщо без -DENABLE_AES -DENABLE_DMR_DATA (та сама умова, що й
 * functions/dmr_rctl_tx.c/menuMessages.c).
 *
 * LEGAL: керування іншою рацією без її відома може порушувати місцеве законодавство
 * поза службовим/аматорським використанням із взаємною згодою -- ця функція призначена
 * для власного флоту станцій, де RCTL свідомо увімкнено власником кожної рації
 * (Options > RCTL access, menuRCTLConfig.c).
 */
#include "user_interface/uiGlobals.h"
#include "user_interface/menuSystem.h"
#include "user_interface/uiLocalisation.h"
#include "user_interface/uiUtilities.h"
#include "functions/dmr_rctl_tx.h"
#include "crypto/dmr_rctl_stock.h"   // dmr_rctl_stock_cmd_t + кількість команд
#include "functions/ticks.h"   // ticksTimer_t: реальний час замість лічби тіків
#include "functions/codeplug.h"
#include "crypto/dmr_aes.h"
#include <string.h>
#include <stdlib.h>

#if defined(LANGUAGE_BUILD_UKRAINIAN)
#include "user_interface/languages/rctl_ua.h"
#else
#define RCTL_TITLE               "Remote control"
#define RCTL_PICK_CMD_TITLE      "Pick command"
#define RCTL_HINT_PICK_CMD       "U/D  GRN:next  RED:exit"
#define RCTL_ID_FMT              "ID: %s"
#define RCTL_HINT_ID             "0-9:id  L:del"
#define RCTL_HINT_CONTACTS       "SK1:contacts"
#define RCTL_HINT_SEND_BACK      "GRN:send  RED:back"
#define RCTL_TITLE_PICK          "Pick contact"
#define RCTL_NO_CONTACTS         "(no contacts)"
#define RCTL_HINT_BACK           "RED:back"
#define RCTL_SENT                "Sent"
#define RCTL_HINT_ANY_KEY        "any key: back"
#define RCTL_ERR_GENERIC         "error"
#define RCTL_ERR_TX_BUSY         "TX busy"
#define RCTL_ERR_FRAME           "build failed"
#define RCTL_SEND_FAILED         "Send failed"
#define RCTL_FAIL_FMT            "%s (ret %d)"
#define RCFG_ITEM_CHECK          "Radio check"
#define RCFG_ITEM_MONITOR        "Monitor"
#define RCFG_ITEM_STUN           "Disable"
#define RCFG_ITEM_REVIVE         "Enable"
#endif

#if defined(ENABLE_AES) && defined(ENABLE_DMR_DATA)

enum { RCTL_PICK_CMD = 0, RCTL_ENTRY, RCTL_PICK_CONTACT, RCTL_WAIT, RCTL_RESULT };

// ЧАС У МІЛІСЕКУНДАХ, а не в тіках.
//
// БУЛО (виправлено 2026-09-05): тут рахувались тіки меню з припущенням "~20 тіків/с".
// Насправді головний цикл крутиться раз на МІЛІСЕКУНДУ (applicationMain.c: "ensure this
// Task runs at 1ms intervals"), а menuSystemCallCurrentMenuTick() викликається щоразу
// без жодного гейта. Тобто частота тіків ~1000/с, у 50 разів більша за оцінку:
//   160 тіків -> 0,16 с замість 8 с очікування ACK;
//   400 тіків -> 0,4 с замість 20 с показу результату.
// З таймаутом 160 мс радіоперевірка НЕ МОГЛА працювати в принципі: стільки не встигає
// пройти навіть один бік дата-виклику DMR, тож відповідь завжди спізнювалась і на екрані
// був "Немає відповіді". Це, найпевніше, і чекало нас на першій же перевірці на залізі.
//
// Тепер час рахується ticksTimer_t -- реальними мілісекундами, як у решті прошивки.
// Такий код не залежить від частоти головного циклу й не зламається, якщо її змінять.
#define RCTL_RESULT_HOLD_MS     20000U

// Скільки чекати квитанцію після Radio Check. Наш власний запит займає ефір ~0.7 с
// (16 преамбул + команда по 30 мс, плюс ~100 мс на keying), і лише ПІСЛЯ цього ціль
// відповідає (стокова -- за ~37 мс). 4 с -- з великим запасом; командир-стокова, для
// порівняння, повторює запит раз на ~1.3 с.
#define RCTL_WAIT_ACK_MS         4000U

// Персистентно між тіками, в CCM -- той самий idiom, що й menuMessages.c/menuAESKeys.c.
static struct
{
	uint8_t  view;
	uint8_t  cmd;           // обрана команда: dmr_rctl_stock_cmd_t (Check/Monitor/Enable/Disable)
	char     rcpt[10];      // ID цілі, що вводиться/обирається
	int8_t   sendResult;    // повернене dmrRctlStockSend(): 0 = надіслано, <0 = помилка TX
	uint8_t  waitedAck;     // 1 = це був Radio Check, і ми чекали квитанцію
	uint8_t  gotAck;        // 1 = квитанція прийшла (інакше -- тайм-аут)
	uint32_t ackGenAtSend;  // покоління ACK на момент надсилання (щоб не взяти стару відповідь)
	uint32_t ackFrom;       // хто відповів
	uint32_t ackAgeSecs;    // скільки секунд тому
	ticksTimer_t holdTimer; // показ результату / очікування -- реальний час, не тіки
} s_rc DMR_AES_CCM;

// Пункти екрана вибору команди -- у ПОРЯДКУ dmr_rctl_stock_cmd_t (Check,Monitor,Enable,Disable),
// щоб індекс пункту == коду команди. Мітки спільні з екраном дозволів (rctl_ua.h).
static const char *cmdName(int i)
{
	switch (i)
	{
		case DMR_RCTL_STOCK_CHECK:   return RCFG_ITEM_CHECK;    // Перевірка
		case DMR_RCTL_STOCK_MONITOR: return RCFG_ITEM_MONITOR;  // Моніторинг
		case DMR_RCTL_STOCK_ENABLE:  return RCFG_ITEM_REVIVE;   // Ввімкнення
		case DMR_RCTL_STOCK_DISABLE: return RCFG_ITEM_STUN;     // Вимкнення
		default:                     return "?";
	}
}

static void updatePickCmd(void);
static void updateEntry(void);
static void updatePickContact(void);
static void updateWait(void);
static void updateResult(void);
static void pickCmdEvent(uiEvent_t *ev);
static void entryEvent(uiEvent_t *ev);
static void pickContactEvent(uiEvent_t *ev);

static void gotoPickCmd(void)
{
	s_rc.view = RCTL_PICK_CMD;
	menuDataGlobal.currentItemIndex = s_rc.cmd;
	menuDataGlobal.numItems = DMR_RCTL_STOCK_NUM_CMDS;
	updatePickCmd();
}

static void gotoEntry(void)
{
	s_rc.view = RCTL_ENTRY;
	updateEntry();
}

menuStatus_t menuRCTLRemote(uiEvent_t *ev, bool isFirstRun)
{
	if (isFirstRun)
	{
		memset(&s_rc, 0, sizeof s_rc);
		gotoPickCmd();
		return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
	}

	menuStatus_t exitCode = MENU_STATUS_SUCCESS;

	if (s_rc.view == RCTL_WAIT)
	{
		// Чекаємо квитанцію на Radio Check. Ознака "прийшла НОВА" -- зросле покоління
		// ACK (а не просто наявність старого стану), тож повторна перевірка тієї ж цілі
		// не показує торішню відповідь.
		if (dmrRctlAckGeneration() != s_rc.ackGenAtSend)
		{
			uint32_t from = 0, ageMs = 0;
			if (dmrRctlLastCheckAck(&from, &ageMs))
			{
				s_rc.gotAck = 1;
				s_rc.ackFrom = from;
				s_rc.ackAgeSecs = ageMs / 1000U;
			}
			ticksTimerStart(&s_rc.holdTimer, RCTL_RESULT_HOLD_MS);
			s_rc.view = RCTL_RESULT;
			updateResult();
			return exitCode;
		}
		if (ev->hasEvent && (ev->events & KEY_EVENT))
		{
			gotoPickCmd();   // RED/будь-яка клавіша -- скасувати очікування
			return exitCode;
		}
		if (ticksTimerHasExpired(&s_rc.holdTimer))
		{
			s_rc.gotAck = 0;   // тайм-аут -> "Немає відповіді"
			ticksTimerStart(&s_rc.holdTimer, RCTL_RESULT_HOLD_MS);
			s_rc.view = RCTL_RESULT;
			updateResult();
		}
		return exitCode;
	}

	if (s_rc.view == RCTL_RESULT)
	{
		// Результат показано; будь-яка клавіша або тайм-аут -> назад до вибору команди.
		if (ev->hasEvent && (ev->events & KEY_EVENT))
		{
			gotoPickCmd();
			return exitCode;
		}
		if (ticksTimerHasExpired(&s_rc.holdTimer))
		{
			gotoPickCmd();
		}
		return exitCode;
	}

	if (ev->hasEvent)
	{
		switch (s_rc.view)
		{
			case RCTL_PICK_CMD:     pickCmdEvent(ev);      break;
			case RCTL_ENTRY:        entryEvent(ev);        break;
			case RCTL_PICK_CONTACT: pickContactEvent(ev);  break;
		}
	}
	return exitCode;
}

/* ============================ PICK COMMAND =============================== */
static void updatePickCmd(void)
{
	char buf[SCREEN_LINE_BUFFER_SIZE];

	displayClearBuf();
	menuDisplayTitle(RCTL_PICK_CMD_TITLE);

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(DMR_RCTL_STOCK_NUM_CMDS, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY) { continue; }
		if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)   { break; }

		snprintf(buf, sizeof buf, "%s", cmdName(mNum));
		menuDisplayEntry(i, mNum, buf, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}
	displayPrintCentered(112, RCTL_HINT_PICK_CMD, FONT_SIZE_1);
	displayRender();
}

static void pickCmdEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		menuSystemPopPreviousMenu();
		return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, DMR_RCTL_STOCK_NUM_CMDS);
		updatePickCmd();
		return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, DMR_RCTL_STOCK_NUM_CMDS);
		updatePickCmd();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		s_rc.cmd = (uint8_t)menuDataGlobal.currentItemIndex;
		gotoEntry();
		return;
	}
}

/* ============================ ENTRY ===================================== */
static void updateEntry(void)
{
	char buf[24];

	displayClearBuf();
	menuDisplayTitle(RCTL_TITLE);

	// показуємо, яку команду шлемо, щоб ціль не набирали для не тієї дії
	displayPrintCentered(18, cmdName(s_rc.cmd), FONT_SIZE_1);

	snprintf(buf, sizeof buf, RCTL_ID_FMT, s_rc.rcpt);
	displayPrintAt(2, 30, buf, FONT_SIZE_3);

	displayPrintCentered(92,  RCTL_HINT_ID, FONT_SIZE_1);
	displayPrintCentered(102, RCTL_HINT_CONTACTS, FONT_SIZE_1);
	displayPrintCentered(112, RCTL_HINT_SEND_BACK, FONT_SIZE_1);
	displayRender();
}

static void doSend(void)
{
	uint32_t dst = (uint32_t)strtoul(s_rc.rcpt, NULL, 10);
	if (dst == 0) { return; }   // порожній/нульовий ID -- нічого не робимо, лишаємось на ENTRY

	// Шлемо у СТОКОВОМУ форматі (сумісність із заводською TYT).
	s_rc.sendResult = (int8_t)dmrRctlStockSend((int)s_rc.cmd, dst);
	s_rc.waitedAck = 0;
	s_rc.gotAck = 0;

	// Radio Check -- єдина команда, на яку ціль відповідає квитанцією (формат знято з
	// ефіру, RCTL_COMPAT.md §5a). Запам'ятовуємо ПОКОЛІННЯ ACK до очікування, щоб стара
	// відповідь не зійшла за нову, і чекаємо на екрані "Перевірка...".
	if ((s_rc.sendResult == 0) && (s_rc.cmd == DMR_RCTL_STOCK_CHECK))
	{
		s_rc.waitedAck = 1;
		s_rc.ackGenAtSend = dmrRctlAckGeneration();
		ticksTimerStart(&s_rc.holdTimer, RCTL_WAIT_ACK_MS);
		s_rc.view = RCTL_WAIT;
		updateWait();
		return;
	}

	ticksTimerStart(&s_rc.holdTimer, RCTL_RESULT_HOLD_MS);
	s_rc.view = RCTL_RESULT;
	updateResult();
}

static void entryEvent(uiEvent_t *ev)
{
	// SK1 -- звичайне натискання кнопки, не KEY_EVENT -- перевіряємо ДО early return
	// нижче (той самий idiom, що й recipientEvent() у menuMessages.c).
	if (ev->events & BUTTON_EVENT)
	{
		if (BUTTONCHECK_SHORTUP(ev, BUTTON_SK1))
		{
			s_rc.view = RCTL_PICK_CONTACT;
			menuDataGlobal.currentItemIndex = 0;
			menuDataGlobal.numItems = codeplugContactsGetCount(CONTACT_CALLTYPE_PC);
			updatePickContact();
			return;
		}
	}

	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		gotoPickCmd();   // назад до вибору команди (а не вихід із меню)
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		doSend();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_LEFT))
	{
		int n = (int)strlen(s_rc.rcpt);
		if (n > 0) { s_rc.rcpt[n - 1] = 0; }
		updateEntry();
		return;
	}
	if (KEYCHECK_SHORTUP_NUMBER(ev->keys))
	{
		int n = (int)strlen(s_rc.rcpt);
		if (n < (int)sizeof s_rc.rcpt - 1)
		{
			s_rc.rcpt[n] = (char)ev->keys.key;
			s_rc.rcpt[n + 1] = 0;
		}
		updateEntry();
		return;
	}
}

/* ============================ PICK CONTACT ================================ */
/* Лише приватні (PC) контакти -- RCTL завжди індивідуальний виклик, на відміну від
 * SMS тут немає перемикача Group/Private (dmr_rctl_tx.c адресує group=0 завжди). */
static void updatePickContact(void)
{
	int count = codeplugContactsGetCount(CONTACT_CALLTYPE_PC);
	char buf[24];

	displayClearBuf();
	menuDisplayTitle(RCTL_TITLE_PICK);

	if (count == 0)
	{
		displayPrintCentered(56, RCTL_NO_CONTACTS, FONT_SIZE_2);
		displayPrintCentered(112, RCTL_HINT_BACK, FONT_SIZE_1);
		displayRender();
		return;
	}

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(count, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY) { continue; }
		if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)   { break; }

		CodeplugContact_t c;
		// 1-індексований API кодплагу: mNum йде з 0, тому +1 (той самий idiom, що й
		// pickContactUpdate() у menuMessages.c).
		if (codeplugContactGetDataForNumberInType(mNum + 1, CONTACT_CALLTYPE_PC, &c))
		{
			char name[17];
			codeplugUtilConvertBufToString(c.name, name, 16);
			snprintf(buf, sizeof buf, "%s", name);
		}
		else
		{
			snprintf(buf, sizeof buf, "?");
		}
		menuDisplayEntry(i, mNum, buf, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}
	displayRender();
}

static void pickContactEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	int count = codeplugContactsGetCount(CONTACT_CALLTYPE_PC);

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		s_rc.view = RCTL_ENTRY;
		updateEntry();
		return;
	}

	if (count == 0) { return; }   // порожній список -- лише RED працює

	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, count); updatePickContact(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, count); updatePickContact(); return;
	}

	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		CodeplugContact_t c;
		if (codeplugContactGetDataForNumberInType(menuDataGlobal.currentItemIndex + 1, CONTACT_CALLTYPE_PC, &c))
		{
			snprintf(s_rc.rcpt, sizeof s_rc.rcpt, "%lu", (unsigned long)c.tgNumber);
		}
		s_rc.view = RCTL_ENTRY;
		updateEntry();
		return;
	}
}

/* ===================== Великі символи результату ========================= */
/* Галочка/хрест замість напису: результат перевірки читається з одного погляду,
 * не вимагає читання й не залежить від довжини рядка на 160-піксельному екрані. */

/* Товстий штрих: кілька ліній зі зсувом у межах квадрата -- на діагоналях дає рівну
 * товщину, на відміну від зсуву лише по X. Малюється один раз на екран, тож
 * дешевизна тут не критична. */
static void thickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int8_t t)
{
	for (int8_t dx = -t; dx <= t; dx++)
	{
		for (int8_t dy = -t; dy <= t; dy++)
		{
			displayDrawLine((int16_t)(x0 + dx), (int16_t)(y0 + dy),
			                (int16_t)(x1 + dx), (int16_t)(y1 + dy), false);
		}
	}
}

static void setResultColour(uint32_t rgb)
{
#if defined(HAS_COLOURS)
	uint16_t fg = 0, bg = 0;
	displayGetForegroundAndBackgroundColours(&fg, &bg);   /* тло лишаємо тим, що в теми */
	displaySetForegroundAndBackgroundColours(
			PLATFORM_COLOUR_FORMAT_SWAP_BYTES(RGB888_TO_PLATFORM_COLOUR_FORMAT(rgb)), bg);
#else
	(void)rgb;   /* монохромні платформи -- лишаємо поточний колір теми */
#endif
}

/* Зелена галочка: короткий штрих униз-праворуч + довгий угору-праворуч. */
static void drawTick(void)
{
	setResultColour(0x22DD22);
	thickLine(58, 62, 73, 78, 2);
	thickLine(73, 78, 104, 38, 2);
}

/* Червоний хрест: дві діагоналі. */
static void drawCross(void)
{
	setResultColour(0xFF3B30);
	thickLine(60, 40, 100, 80, 2);
	thickLine(100, 40, 60, 80, 2);
}

/* ============================= WAIT ACK ================================= */
static void updateWait(void)
{
	displayClearBuf();
	menuDisplayTitle(RCTL_TITLE);
	displayPrintCentered(40, cmdName(s_rc.cmd), FONT_SIZE_2);
	displayPrintCentered(64, RCTL_WAITING, FONT_SIZE_3);
	displayPrintCentered(112, RCTL_HINT_CANCEL, FONT_SIZE_1);
	displayRender();
}

/* ============================ RESULT ==================================== */
static void updateResult(void)
{
	displayClearBuf();
	menuDisplayTitle(RCTL_TITLE);

	if (s_rc.sendResult != 0)
	{
		char buf[24];
		const char *why = RCTL_ERR_GENERIC;
		switch (s_rc.sendResult)
		{
			case -2: why = RCTL_ERR_TX_BUSY; break;
			case -4: why = RCTL_ERR_FRAME;   break;
		}
		displayPrintCentered(44, RCTL_SEND_FAILED, FONT_SIZE_3);
		snprintf(buf, sizeof buf, RCTL_FAIL_FMT, why, s_rc.sendResult);
		displayPrintCentered(72, buf, FONT_SIZE_2);
	}
	else if (s_rc.waitedAck)
	{
		// Radio Check: результат -- великим символом (галочка/хрест), щоб читався з
		// одного погляду. Підпис лишаємо дрібним: символ несе головне.
		displayPrintCentered(20, cmdName(s_rc.cmd), FONT_SIZE_2);
		if (s_rc.gotAck)
		{
			char buf[24];
			drawTick();
			displayThemeResetToDefault();   // далі текст -- звичайним кольором теми
			snprintf(buf, sizeof buf, RCTL_ACK_AGE_FMT,
			         (unsigned long)s_rc.ackFrom, (unsigned long)s_rc.ackAgeSecs);
			// FONT_SIZE_1 (6 px/символ): у FONT_SIZE_2 рядок з 8-значним ID і двозначними
			// секундами виходить за 160 px і обрізається на рації.
			displayPrintCentered(94, buf, FONT_SIZE_1);
		}
		else
		{
			drawCross();
			displayThemeResetToDefault();
		}
	}
	else
	{
		// Решта команд квитанції не мають -- показуємо лише факт відправки.
		displayPrintCentered(40, cmdName(s_rc.cmd), FONT_SIZE_2);
		displayPrintCentered(64, RCTL_SENT, FONT_SIZE_3);
	}
	displayPrintCentered(112, RCTL_HINT_ANY_KEY, FONT_SIZE_1);

	displayRender();
}

#endif // ENABLE_AES && ENABLE_DMR_DATA

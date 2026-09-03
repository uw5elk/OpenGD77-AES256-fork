/*
 * menuRCTLRemote.c — "Від. керування" / "Remote control": надіслати запит Radio Check
 * (DMR_RCTL_CMD_CHECK_REQ) іншій рації й показати, чи відповіла.
 *
 * Модельовано на menuMessages.c (RECIPIENT + PICK_CONTACT), спрощено під один-єдиний
 * запит без набору тексту:
 *   ENTRY         : ввести/обрати DMR ID цілі, GRN надсилає запит
 *   PICK_CONTACT  : список приватних контактів кодплагу замість ручного набору ID
 *                   (RCTL — лише індивідуальний виклик, групового режиму немає за
 *                   дизайном, тож тут на відміну від SMS немає перемикача Group/Private)
 *   RESULT        : або одразу помилка надсилання (TX зайнятий/немає ключа/...), або
 *                   "Перевірка..." з очікуванням CHECK_ACK, потім результат (відповіла /
 *                   не відповіла за тайм-аут)
 *
 * "Нову" відповідь від "застарілої" (що лишилась від попереднього, не пов'язаного
 * запиту) відрізняємо через dmrRctlAckGeneration() — лічильник, що зростає при
 * кожному прийнятому CHECK_ACK (functions/dmr_rctl_tx.c) — а не лише за віком.
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
#include "functions/codeplug.h"
#include "crypto/dmr_aes.h"
#include <string.h>
#include <stdlib.h>

#if defined(LANGUAGE_BUILD_UKRAINIAN)
#include "user_interface/languages/rctl_ua.h"
#else
#define RCTL_TITLE               "Remote control"
#define RCTL_ID_FMT              "ID: %s"
#define RCTL_HINT_ID             "0-9:id  L:del"
#define RCTL_HINT_CONTACTS       "SK1:contacts"
#define RCTL_HINT_SEND_BACK      "GRN:check  RED:back"
#define RCTL_TITLE_PICK          "Pick contact"
#define RCTL_NO_CONTACTS         "(no contacts)"
#define RCTL_HINT_BACK           "RED:back"
#define RCTL_WAITING             "Checking..."
#define RCTL_HINT_CANCEL         "RED:cancel"
#define RCTL_ACK_OK              "Radio is on air"
#define RCTL_ACK_AGE_FMT         "ID %lu, %lus ago"
#define RCTL_TIMEOUT             "No response"
#define RCTL_HINT_ANY_KEY        "any key: back"
#define RCTL_ERR_GENERIC         "error"
#define RCTL_ERR_TX_BUSY         "TX busy"
#define RCTL_ERR_NO_KEY          "no AES key"
#define RCTL_ERR_FRAME           "build failed"
#define RCTL_SEND_FAILED         "Send failed"
#define RCTL_FAIL_FMT            "%s (ret %d)"
#endif

#if defined(ENABLE_AES) && defined(ENABLE_DMR_DATA)

enum { RCTL_ENTRY = 0, RCTL_PICK_CONTACT, RCTL_RESULT };

// ~8 с очікування ACK і ~20 с показу фінального результату, при тій самій оцінці
// частоти тіків (~20/с), що й MSG_RESULT у menuMessages.c (600 тіків ~= 30 с).
#define RCTL_WAIT_TIMEOUT_TICKS   160
#define RCTL_RESULT_HOLD_TICKS    400

// Персистентно між тіками, в CCM -- той самий idiom, що й menuMessages.c/menuAESKeys.c.
static struct
{
	uint8_t  view;
	char     rcpt[10];      // ID цілі, що вводиться/обирається

	int8_t   sendResult;    // повернене dmrRctlRequestCheck(): 0 = надіслано, <0 = помилка TX
	uint8_t  waiting;       // 1 = чекаємо CHECK_ACK від цілі
	uint8_t  acked;         // 1 = дочекались відповіді саме від цілі
	uint32_t waitTargetId;
	uint32_t ackGenAtSend;  // знімок dmrRctlAckGeneration() у момент надсилання
	uint32_t ackAgeMsShown;
	uint16_t ticksLeft;
} s_rc DMR_AES_CCM;

static void updateEntry(void);
static void updatePickContact(void);
static void updateResult(void);
static void entryEvent(uiEvent_t *ev);
static void pickContactEvent(uiEvent_t *ev);

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
		gotoEntry();
		return MENU_STATUS_SUCCESS;
	}

	menuStatus_t exitCode = MENU_STATUS_SUCCESS;

	if (s_rc.view == RCTL_RESULT)
	{
		// Поки чекаємо -- щотік перевіряємо, чи не прийшла НОВА (за generation) відповідь
		// саме від цілі, і рахуємо тайм-аут; нічого з цього не залежить від key-подій.
		if (s_rc.waiting)
		{
			uint32_t fromId, ageMs;
			if ((dmrRctlAckGeneration() != s_rc.ackGenAtSend) &&
					dmrRctlLastCheckAck(&fromId, &ageMs) && (fromId == s_rc.waitTargetId))
			{
				s_rc.acked = 1;
				s_rc.waiting = 0;
				s_rc.ackAgeMsShown = ageMs;
				s_rc.ticksLeft = RCTL_RESULT_HOLD_TICKS;
				updateResult();
			}
			else if (s_rc.ticksLeft > 0)
			{
				s_rc.ticksLeft--;
				if (s_rc.ticksLeft == 0)
				{
					// Тайм-аут: відповіді не було. НЕ лишаємо ticksLeft на 0 тут -- інакше
					// перевірка "автозакриття" нижче в цьому ж виклику одразу зітре щойно
					// показаний RCTL_TIMEOUT, і користувач його просто не встигне побачити.
					s_rc.waiting = 0;
					s_rc.ticksLeft = RCTL_RESULT_HOLD_TICKS;
					updateResult();
				}
			}
		}
		else if (s_rc.ticksLeft > 0)
		{
			s_rc.ticksLeft--;
		}

		if (ev->hasEvent && (ev->events & KEY_EVENT))
		{
			if (s_rc.waiting)
			{
				// Під час очікування діє лише RED (скасувати); решта клавіш ігноруються,
				// щоб випадковий дотик не "з'їдав" подію GREEN, якою напросився запит.
				if (KEYCHECK_SHORTUP(ev->keys, KEY_RED)) { gotoEntry(); }
			}
			else
			{
				// Результат (успіх/помилка/тайм-аут) уже відомий -- будь-яка клавіша
				// повертає на ввід ID, як MSGS_HINT_ANY_KEY у menuMessages.c.
				gotoEntry();
			}
			return exitCode;
		}

		if ((!s_rc.waiting) && (s_rc.ticksLeft == 0))
		{
			gotoEntry();
		}
		return exitCode;
	}

	if (ev->hasEvent)
	{
		switch (s_rc.view)
		{
			case RCTL_ENTRY:        entryEvent(ev);        break;
			case RCTL_PICK_CONTACT: pickContactEvent(ev);  break;
		}
	}
	return exitCode;
}

/* ============================ ENTRY ===================================== */
static void updateEntry(void)
{
	char buf[24];

	displayClearBuf();
	menuDisplayTitle(RCTL_TITLE);

	snprintf(buf, sizeof buf, RCTL_ID_FMT, s_rc.rcpt);
	displayPrintAt(2, 28, buf, FONT_SIZE_3);

	displayPrintCentered(92,  RCTL_HINT_ID, FONT_SIZE_1);
	displayPrintCentered(102, RCTL_HINT_CONTACTS, FONT_SIZE_1);
	displayPrintCentered(112, RCTL_HINT_SEND_BACK, FONT_SIZE_1);
	displayRender();
}

static void doCheck(void)
{
	uint32_t dst = (uint32_t)strtoul(s_rc.rcpt, NULL, 10);
	if (dst == 0) { return; }   // порожній/нульовий ID -- нічого не робимо, лишаємось на ENTRY

	s_rc.waitTargetId = dst;
	s_rc.ackGenAtSend = dmrRctlAckGeneration();
	s_rc.sendResult = (int8_t)dmrRctlRequestCheck(dst);
	s_rc.acked = 0;
	s_rc.waiting = (s_rc.sendResult == 0) ? 1 : 0;
	s_rc.ticksLeft = s_rc.waiting ? RCTL_WAIT_TIMEOUT_TICKS : RCTL_RESULT_HOLD_TICKS;
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
		menuSystemPopPreviousMenu();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		doCheck();
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
			case -3: why = RCTL_ERR_NO_KEY;  break;
			case -4: why = RCTL_ERR_FRAME;   break;
		}
		displayPrintCentered(44, RCTL_SEND_FAILED, FONT_SIZE_3);
		snprintf(buf, sizeof buf, RCTL_FAIL_FMT, why, s_rc.sendResult);
		displayPrintCentered(72, buf, FONT_SIZE_2);
		displayPrintCentered(112, RCTL_HINT_ANY_KEY, FONT_SIZE_1);
	}
	else if (s_rc.acked)
	{
		char buf[24];
		displayPrintCentered(44, RCTL_ACK_OK, FONT_SIZE_2);
		snprintf(buf, sizeof buf, RCTL_ACK_AGE_FMT, (unsigned long)s_rc.waitTargetId, (unsigned long)(s_rc.ackAgeMsShown / 1000));
		displayPrintCentered(72, buf, FONT_SIZE_2);
		displayPrintCentered(112, RCTL_HINT_ANY_KEY, FONT_SIZE_1);
	}
	else if (s_rc.waiting)
	{
		displayPrintCentered(56, RCTL_WAITING, FONT_SIZE_3);
		displayPrintCentered(112, RCTL_HINT_CANCEL, FONT_SIZE_1);
	}
	else
	{
		displayPrintCentered(44, RCTL_TIMEOUT, FONT_SIZE_3);
		displayPrintCentered(112, RCTL_HINT_ANY_KEY, FONT_SIZE_1);
	}

	displayRender();
}

#endif // ENABLE_AES && ENABLE_DMR_DATA

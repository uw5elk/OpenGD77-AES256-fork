/*
 * menuMessages.c — on-radio encrypted DMR SMS UX (Inbox / Sent / New Message).
 *
 * Modelled on menuAESKeys.c. A small screen state-machine:
 *   HOME         : Inbox (n) / Sent (n) / New Message
 *   LIST         : the messages of a folder + a "[Delete all]" row
 *   READ         : full text of one message
 *   COMPOSE      : keypad text entry (multi-tap, like the contact-name editor)
 *   RECIPIENT    : destination DMR ID / talkgroup + Group/Private + send
 *   PICK_CONTACT : browse the existing codeplug Contact List instead of typing an ID
 *
 * Key combos (2026-08-31 pass — moved into the main menu, added Reply/Resend, a
 * contacts-based recipient picker, and "mark all read", on top of what already existed):
 *   HOME/LIST : GREEN opens/selects, SK2+GREEN deletes (single, or "[Delete all]" row),
 *               SK2+RED marks every Inbox message read in one go.
 *   READ      : SK2+GREEN deletes; plain GREEN = Reply (Inbox: blank text, recipient
 *               pre-set to the sender) or Resend (Sent: text AND recipient both
 *               pre-filled, lands straight on RECIPIENT); RED goes back.
 *   RECIPIENT : SK1 opens PICK_CONTACT, browsing the Contact List filtered by the
 *               current Group/Private toggle (U/D), so the entry you land on already
 *               matches what you're about to pick.
 *
 * Sending and the store live in functions/dmr_sms.c (the on-air-validated AES-256-ECB
 * scheme a stock TYT decrypts). Compiles to nothing unless -DENABLE_AES -DENABLE_DMR_DATA.
 *
 * LEGAL: encrypted traffic is illegal on amateur bands in most jurisdictions. PMR/commercial.
 */
#include "user_interface/uiGlobals.h"
#include "user_interface/menuSystem.h"
#include "user_interface/uiLocalisation.h"
#include "user_interface/uiUtilities.h"
#include "functions/trx.h"
#include "functions/dmr_sms.h"
#include "functions/codeplug.h"   /* Contact List browsing for the recipient picker */
#include "crypto/dmr_aes.h"
#include "io/keyboard.h"

/*
 * Рядки цього екрана перемикаються на етапі компіляції, а не через stringsTable_t:
 * SMS -- функція саме цього форку, а таблицю рядків читає ще й мовний файл від CPS,
 * тож три десятки нових полів зламали б ту сумісність і з'їли б ~1 КБ флеша в кожній
 * збірці. Українські значення лежать у languages/messages_ua.h (кодування cp1251).
 */
#if defined(LANGUAGE_BUILD_UKRAINIAN)
#include "user_interface/languages/messages_ua.h"
#else
#define MSGS_TITLE               "Messages"
#define MSGS_INBOX_FMT           "Inbox (%d)"
#define MSGS_SENT_FMT            "Sent (%d)"
#define MSGS_NEW                 "New Message"
#define MSGS_TITLE_SENT          "Sent"
#define MSGS_TITLE_INBOX         "Inbox"
#define MSGS_EMPTY               "(empty)"
#define MSGS_HINT_BACK           "RED:back"
#define MSGS_DELETE_ALL          "[Delete all]"
#define MSGS_TO                  "To"
#define MSGS_FROM                "From"
#define MSGS_HINT_SENT_ITEM      "SK2+GRN:del GRN:resend"
#define MSGS_HINT_INBOX_ITEM     "SK2+GRN:del GRN:reply"
#define MSGS_TITLE_NEW           "New Message"
#define MSGS_CHARS_FMT           "%d/%d chars"
#define MSGS_HINT_PRESET         "U:preset  L:del"
#define MSGS_HINT_TO_CANCEL      "GRN:to  RED:cancel"
#define MSGS_TITLE_RCPT          "Recipient"
#define MSGS_TO_FMT              "To: %s"
#define MSGS_TYPE_FMT            "Type: %s"
#define MSGS_GROUP               "Group"
#define MSGS_PRIVATE             "Private"
#define MSGS_HINT_ID             "0-9:id  L:del"
#define MSGS_HINT_GRP_PRIV       "U/D:Group/Private"
#define MSGS_HINT_CONTACTS       "SK1:contacts"
#define MSGS_HINT_SEND_BACK      "GRN:send  RED:back"
#define MSGS_TITLE_PICK_TG       "Pick TG contact"
#define MSGS_TITLE_PICK_PC       "Pick PC contact"
#define MSGS_NO_CONTACTS         "(no contacts)"
#define MSGS_SENT_OK             "Message sent"
#define MSGS_KEYED_TX            "(keyed TX)"
#define MSGS_SEND_FAILED         "Send failed"
#define MSGS_ERR_GENERIC         "error"
#define MSGS_ERR_TX_BUSY         "TX busy"
#define MSGS_ERR_BAD_TEXT        "bad text"
#define MSGS_ERR_TOO_LONG        "too long"
#define MSGS_FAIL_FMT            "%s (ret %d)"
#define MSGS_HINT_ANY_KEY        "any key: back"
#endif
#include <string.h>
#include <stdlib.h>

#if defined(ENABLE_AES) && defined(ENABLE_DMR_DATA)

enum { MSG_HOME = 0, MSG_LIST, MSG_READ, MSG_COMPOSE, MSG_RECIPIENT, MSG_PICK_CONTACT, MSG_RESULT };

static struct
{
	uint8_t  view;
	uint8_t  folder;        // 0 = Inbox, 1 = Sent
	int16_t  readIdx;       // message being read
	char     compose[DMR_SMS_TEXT_MAX + 1];
	int16_t  composePos;
	char     rcpt[10];      // numeric recipient entry
	int16_t  rcptPos;
	uint8_t  rcptGroup;     // 1 = talkgroup, 0 = private call
	int8_t   presetIdx;     // last quick-text preset cycled in (-1 = none)
	uint8_t  rcptPreset;    // 1 = rcpt/rcptGroup already set by Reply/Resend; composeEvent's
	                        // GREEN must not clobber it with the usual default-recipient prefill
	int8_t   result;        // dmrSmsSend() return for the result screen
	uint16_t resultTicks;
} s_msg DMR_AES_CCM;

static void homeUpdate(void);
static void listUpdate(void);
static void readUpdate(void);
static void composeUpdate(void);
static void recipientUpdate(void);
static void pickContactUpdate(void);
static void resultUpdate(void);
static void homeEvent(uiEvent_t *ev, menuStatus_t *ec);
static void listEvent(uiEvent_t *ev);
static void readEvent(uiEvent_t *ev);
static void composeEvent(uiEvent_t *ev);
static void recipientEvent(uiEvent_t *ev);
static void pickContactEvent(uiEvent_t *ev);

static void gotoHome(void)
{
	keypadAlphaEnable = false;
	s_msg.view = MSG_HOME;
	menuDataGlobal.currentItemIndex = 0;
	menuDataGlobal.numItems = 3;
	homeUpdate();
}

menuStatus_t menuMessages(uiEvent_t *ev, bool isFirstRun)
{
	if (isFirstRun)
	{
		memset(&s_msg, 0, sizeof s_msg);
		dmrSmsInit();
		gotoHome();
		return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
	}

	menuStatus_t exitCode = MENU_STATUS_SUCCESS;

	if (s_msg.view == MSG_RESULT)
	{
		if (s_msg.resultTicks) { s_msg.resultTicks--; }
		if ((s_msg.resultTicks == 0) || (ev->hasEvent && (ev->events & KEY_EVENT)))
		{
			gotoHome();
			return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
		}
		return exitCode;
	}

	if (ev->hasEvent)
	{
		switch (s_msg.view)
		{
			case MSG_HOME:         homeEvent(ev, &exitCode); break;
			case MSG_LIST:         listEvent(ev);            break;
			case MSG_READ:         readEvent(ev);            break;
			case MSG_COMPOSE:      composeEvent(ev);         break;
			case MSG_RECIPIENT:    recipientEvent(ev);       break;
			case MSG_PICK_CONTACT: pickContactEvent(ev);     break;
		}
	}
	return exitCode;
}

/* ============================ HOME ===================================== */
static void homeUpdate(void)
{
	char buf[24];
	displayClearBuf();
	menuDisplayTitle(MSGS_TITLE);

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(3, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY) { continue; }
		if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)   { break; }

		switch (mNum)
		{
			case 0:  snprintf(buf, sizeof buf, MSGS_INBOX_FMT, dmrSmsCount(0)); break;
			case 1:  snprintf(buf, sizeof buf, MSGS_SENT_FMT,  dmrSmsCount(1)); break;
			default: snprintf(buf, sizeof buf, MSGS_NEW);                break;
		}
		menuDisplayEntry(i, mNum, buf, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}

	displayRender();
}

static void openFolder(int folder)
{
	s_msg.folder = (uint8_t)folder;
	s_msg.view = MSG_LIST;
	menuDataGlobal.currentItemIndex = 0;
	menuDataGlobal.numItems = dmrSmsCount(folder) + 1;   // +1 for "[Delete all]"
	listUpdate();
}

static void startCompose(void)
{
	memset(s_msg.compose, 0, sizeof s_msg.compose);
	s_msg.composePos = 0;
	s_msg.presetIdx = -1;
	// A plain "New Message" always falls back to the default-recipient prefill, never a
	// stale Reply/Resend target left over from an earlier, possibly cancelled, attempt.
	// startReplyOrResend() re-sets this to 1 itself AFTER calling this function.
	s_msg.rcptPreset = 0;
	s_msg.view = MSG_COMPOSE;
	keypadAlphaEnable = true;
	composeUpdate();
}

/* Cycle the next non-empty quick-text preset into the compose buffer. */
static void cyclePreset(void)
{
	int total = dmrSmsPresetCount();
	if (total == 0) { return; }
	for (int tries = 0; tries < DMR_SMS_NUM_PRESETS; tries++)
	{
		s_msg.presetIdx = (int8_t)((s_msg.presetIdx + 1) % DMR_SMS_NUM_PRESETS);
		const char *p = dmrSmsPresetGet(s_msg.presetIdx);
		if (p != NULL)
		{
			int cap = dmrSmsMaxLen();
			strncpy(s_msg.compose, p, cap);
			s_msg.compose[cap] = 0;
			s_msg.composePos = (int16_t)strlen(s_msg.compose);
			if (s_msg.composePos > cap - 1) { s_msg.composePos = cap - 1; }
			composeUpdate();
			return;
		}
	}
}

static void homeEvent(uiEvent_t *ev, menuStatus_t *ec)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, 3);
		*ec |= MENU_STATUS_LIST_TYPE; homeUpdate(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, 3);
		*ec |= MENU_STATUS_LIST_TYPE; homeUpdate(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		menuSystemPopPreviousMenu(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		switch (menuDataGlobal.currentItemIndex)
		{
			case 0: openFolder(0);  break;
			case 1: openFolder(1);  break;
			case 2: startCompose(); break;
		}
		return;
	}
}

/* ============================ LIST ===================================== */
static void listUpdate(void)
{
	char buf[DMR_SMS_TEXT_MAX + 24];
	int count = dmrSmsCount(s_msg.folder);
	displayClearBuf();
	menuDisplayTitle(s_msg.folder ? MSGS_TITLE_SENT : MSGS_TITLE_INBOX);

	if (count == 0)
	{
		displayPrintCentered(56, MSGS_EMPTY, FONT_SIZE_3);
		displayPrintCentered(112, MSGS_HINT_BACK, FONT_SIZE_1);
		displayRender();
		return;
	}

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(count + 1, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY) { continue; }
		if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)   { break; }

		if (mNum == count)
		{
			snprintf(buf, sizeof buf, MSGS_DELETE_ALL);
		}
		else
		{
			const dmrSmsMessage_t *m = dmrSmsGet(s_msg.folder, mNum);
			if (m == NULL) { continue; }
			char txt[DMR_SMS_TEXT_MAX + 1];
			int n = (m->textLen < (int)sizeof txt - 1) ? m->textLen : (int)sizeof txt - 1;
			memcpy(txt, m->text, n); txt[n] = 0;
			char mark = (m->flags & DMR_SMS_FLAG_UNREAD) ? '*' : ' ';
			snprintf(buf, sizeof buf, "%c%lu:%s", mark, (unsigned long)m->peerId, txt);
		}
		menuDisplayEntry(i, mNum, buf, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}
	displayRender();
}

static void listEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }
	int count = dmrSmsCount(s_msg.folder);

	// SK2+RED marks every Inbox message read in one go (dmrSmsMarkAllRead() only ever
	// touches unread Inbox entries, so calling it from the Sent view is a harmless no-op).
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED) && BUTTONCHECK_DOWN(ev, BUTTON_SK2))
	{
		dmrSmsMarkAllRead();
		listUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED)) { gotoHome(); return; }

	if (count == 0) { return; }

	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, count + 1); listUpdate(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, count + 1); listUpdate(); return;
	}

	int idx = menuDataGlobal.currentItemIndex;

	// SK2+GREEN deletes the selected message (deliberate combo).
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN) && BUTTONCHECK_DOWN(ev, BUTTON_SK2))
	{
		if (idx == count) { dmrSmsDeleteAll(s_msg.folder); }   // delete-all row + SK2
		else              { dmrSmsDelete(s_msg.folder, idx); }
		int newCount = dmrSmsCount(s_msg.folder);
		menuDataGlobal.numItems = newCount + 1;
		if (menuDataGlobal.currentItemIndex >= menuDataGlobal.numItems)
		{
			menuDataGlobal.currentItemIndex = menuDataGlobal.numItems - 1;
		}
		listUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		if (idx == count) { return; }   // need SK2 to confirm delete-all
		s_msg.readIdx = (int16_t)idx;
		dmrSmsMarkRead(s_msg.folder, idx);
		s_msg.view = MSG_READ;
		readUpdate();
		return;
	}
}

/* ============================ READ ===================================== */
static void readUpdate(void)
{
	const dmrSmsMessage_t *m = dmrSmsGet(s_msg.folder, s_msg.readIdx);
	displayClearBuf();
	menuDisplayTitle(s_msg.folder ? MSGS_TITLE_SENT : MSGS_TITLE_INBOX);

	if (m == NULL) { displayRender(); return; }

	char hdr[24];
	snprintf(hdr, sizeof hdr, "%s %lu", (m->flags & DMR_SMS_FLAG_OUTGOING) ? MSGS_TO : MSGS_FROM,
			(unsigned long)m->peerId);
	displayPrintAt(2, 16, hdr, FONT_SIZE_1);

	// word-free char wrap into ~21-char lines
	char txt[DMR_SMS_TEXT_MAX + 1];
	int n = (m->textLen < (int)sizeof txt - 1) ? m->textLen : (int)sizeof txt - 1;
	memcpy(txt, m->text, n); txt[n] = 0;

	const int perLine = 21;
	char line[perLine + 1];
	int y = 30;
	for (int i = 0; i < n && y < 104; i += perLine, y += 12)
	{
		int len = ((n - i) < perLine) ? (n - i) : perLine;
		memcpy(line, txt + i, len); line[len] = 0;
		displayPrintAt(2, y, line, FONT_SIZE_2);
	}

	displayPrintCentered(112, s_msg.folder ? MSGS_HINT_SENT_ITEM : MSGS_HINT_INBOX_ITEM, FONT_SIZE_1);
	displayRender();
}

/* Reply (Inbox: blank text, recipient pre-set to the sender) or Resend (Sent: text AND
 * recipient both pre-filled, straight to RECIPIENT — one GREEN press away from re-sending
 * unedited, or RED to go back into COMPOSE and change the wording first). */
static void startReplyOrResend(void)
{
	const dmrSmsMessage_t *m = dmrSmsGet(s_msg.folder, s_msg.readIdx);
	if (m == NULL) { return; }

	uint32_t peer = m->peerId;
	uint8_t  group = (m->flags & DMR_SMS_FLAG_GROUP) ? 1 : 0;

	if (s_msg.folder == 1)   // Sent -> Resend
	{
		int n = (m->textLen < (int)sizeof s_msg.compose - 1) ? m->textLen : (int)sizeof s_msg.compose - 1;
		memset(s_msg.compose, 0, sizeof s_msg.compose);
		memcpy(s_msg.compose, m->text, n);
		s_msg.composePos = (int16_t)n;
		s_msg.presetIdx = -1;
		snprintf(s_msg.rcpt, sizeof s_msg.rcpt, "%lu", (unsigned long)peer);
		s_msg.rcptPos = (int16_t)strlen(s_msg.rcpt);
		s_msg.rcptGroup = group;
		s_msg.view = MSG_RECIPIENT;
		keypadAlphaEnable = false;
		recipientUpdate();
	}
	else                     // Inbox -> Reply
	{
		startCompose();   // resets compose text/pos/presetIdx AND rcptPreset - must run first
		snprintf(s_msg.rcpt, sizeof s_msg.rcpt, "%lu", (unsigned long)peer);
		s_msg.rcptPos = (int16_t)strlen(s_msg.rcpt);
		s_msg.rcptGroup = group;
		s_msg.rcptPreset = 1;   // composeEvent's GREEN must skip the default-recipient prefill
	}
}

static void readEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN) && BUTTONCHECK_DOWN(ev, BUTTON_SK2))
	{
		dmrSmsDelete(s_msg.folder, s_msg.readIdx);
		openFolder(s_msg.folder);
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		startReplyOrResend();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		openFolder(s_msg.folder);
		return;
	}
}

/* ============================ COMPOSE ================================== */
static void composeUpdate(void)
{
	displayClearBuf();
	menuDisplayTitle(MSGS_TITLE_NEW);

	// show the text wrapped
	const int perLine = 21;
	int n = (int)strlen(s_msg.compose);
	char line[perLine + 1];
	int y = 22;
	if (n == 0)
	{
		displayPrintAt(2, y, "_", FONT_SIZE_2);
	}
	for (int i = 0; i < n && y < 92; i += perLine, y += 14)
	{
		int len = ((n - i) < perLine) ? (n - i) : perLine;
		memcpy(line, s_msg.compose + i, len); line[len] = 0;
		displayPrintAt(2, y, line, FONT_SIZE_2);
	}

	char cnt[20];
	snprintf(cnt, sizeof cnt, MSGS_CHARS_FMT, n, dmrSmsMaxLen());
	displayPrintCentered(96, cnt, FONT_SIZE_1);
	if (dmrSmsPresetCount() > 0)
	{
		displayPrintCentered(104, MSGS_HINT_PRESET, FONT_SIZE_1);
	}
	displayPrintCentered(116, MSGS_HINT_TO_CANCEL, FONT_SIZE_1);
	displayRender();
}

static void prefillRecipient(void)
{
	uint32_t defDst = 0;
	int defGroup = 1;
	dmrSmsDefaultRecipient(&defDst, &defGroup);

	uint32_t id;
	if (defDst != 0)
	{
		id = defDst;                 // CHIRP-configured default recipient
		s_msg.rcptGroup = defGroup ? 1 : 0;
	}
	else
	{
		id = trxTalkGroupOrPcId & 0x00FFFFFF;   // fall back to the current channel
		s_msg.rcptGroup = ((trxTalkGroupOrPcId >> 24) == PC_CALL_FLAG) ? 0 : 1;
	}
	snprintf(s_msg.rcpt, sizeof s_msg.rcpt, "%lu", (unsigned long)id);
	s_msg.rcptPos = (int16_t)strlen(s_msg.rcpt);
}

static void composeEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		gotoHome(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		if (strlen(s_msg.compose) == 0) { return; }
		keypadAlphaEnable = false;
		if (s_msg.rcptPreset) { s_msg.rcptPreset = 0; }   // Reply: rcpt/rcptGroup already set
		else                  { prefillRecipient(); }
		s_msg.view = MSG_RECIPIENT;
		recipientUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_LEFT))
	{
		int n = (int)strlen(s_msg.compose);
		if (n > 0) { s_msg.compose[n - 1] = 0; if (s_msg.composePos > 0) s_msg.composePos--; }
		composeUpdate();
		return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))   // insert a CHIRP-configured quick-text preset
	{
		cyclePreset();
		return;
	}
	// multi-tap alpha entry (keypadAlphaEnable handles cycling; same idiom as contact name)
	if (s_msg.composePos < dmrSmsMaxLen())
	{
		if (ev->keys.event == KEY_MOD_PREVIEW)
		{
			s_msg.compose[s_msg.composePos] = ev->keys.key;
			composeUpdate();
			return;
		}
		if (ev->keys.event == KEY_MOD_PRESS)
		{
			s_msg.compose[s_msg.composePos] = ev->keys.key;
			if (s_msg.composePos < (int)strlen(s_msg.compose) && s_msg.composePos < dmrSmsMaxLen() - 1)
			{
				s_msg.composePos++;
			}
			composeUpdate();
			return;
		}
	}
}

/* ============================ RECIPIENT =============================== */
static void recipientUpdate(void)
{
	char buf[24];
	displayClearBuf();
	menuDisplayTitle(MSGS_TITLE_RCPT);

	snprintf(buf, sizeof buf, MSGS_TO_FMT, s_msg.rcpt);
	displayPrintAt(2, 28, buf, FONT_SIZE_3);
	snprintf(buf, sizeof buf, MSGS_TYPE_FMT, s_msg.rcptGroup ? MSGS_GROUP : MSGS_PRIVATE);
	displayPrintAt(2, 52, buf, FONT_SIZE_2);

	displayPrintCentered(82,  MSGS_HINT_ID, FONT_SIZE_1);
	displayPrintCentered(92,  MSGS_HINT_GRP_PRIV, FONT_SIZE_1);
	displayPrintCentered(102, MSGS_HINT_CONTACTS, FONT_SIZE_1);
	displayPrintCentered(112, MSGS_HINT_SEND_BACK, FONT_SIZE_1);
	displayRender();
}

static void doSend(void)
{
	uint32_t dst = (uint32_t)strtoul(s_msg.rcpt, NULL, 10);
	if (dst == 0) { return; }
	s_msg.result = (int8_t)dmrSmsSend(s_msg.compose, dst, s_msg.rcptGroup ? 1 : 0, 0);
	s_msg.view = MSG_RESULT;
	s_msg.resultTicks = 600;    // persist (~30 s) so the result/code is readable; any key dismisses
	resultUpdate();
}

static void recipientEvent(uiEvent_t *ev)
{
	// SK1 is a plain button press, not a KEY_EVENT - must be checked before the
	// KEY_EVENT-only early return below (same idiom as menuChannelDetails.c).
	if (ev->events & BUTTON_EVENT)
	{
		if (BUTTONCHECK_SHORTUP(ev, BUTTON_SK1))
		{
			s_msg.view = MSG_PICK_CONTACT;
			menuDataGlobal.currentItemIndex = 0;
			// count тут той самий callType, який побачить pickContactUpdate()/pickContactEvent()
			// нижче (rcptGroup визначає TG чи PC) - інакше UP/DOWN гортатиме за старим numItems.
			menuDataGlobal.numItems = codeplugContactsGetCount(s_msg.rcptGroup ? CONTACT_CALLTYPE_TG : CONTACT_CALLTYPE_PC);
			pickContactUpdate();
			return;
		}
	}

	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		s_msg.view = MSG_COMPOSE;
		keypadAlphaEnable = true;
		composeUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		doSend(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP) || KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		s_msg.rcptGroup = s_msg.rcptGroup ? 0 : 1; recipientUpdate(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_LEFT))
	{
		int n = (int)strlen(s_msg.rcpt);
		if (n > 0) { s_msg.rcpt[n - 1] = 0; }
		recipientUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP_NUMBER(ev->keys))
	{
		int n = (int)strlen(s_msg.rcpt);
		if (n < (int)sizeof s_msg.rcpt - 1)
		{
			s_msg.rcpt[n] = (char)ev->keys.key;
			s_msg.rcpt[n + 1] = 0;
		}
		recipientUpdate();
		return;
	}
}

/* ============================ PICK CONTACT ============================= */
/* Список контактів кодплагу (TG чи PC, залежно від s_msg.rcptGroup) для швидкого вибору
 * адресата замість ручного набору ID цифрами. Вхід - SK1 з екрану RECIPIENT, вихід - назад
 * туди ж (GREEN підставляє обраний контакт, RED повертається без змін). */
static void pickContactUpdate(void)
{
	uint32_t callType = s_msg.rcptGroup ? CONTACT_CALLTYPE_TG : CONTACT_CALLTYPE_PC;
	int count = codeplugContactsGetCount(callType);
	char buf[24];

	displayClearBuf();
	menuDisplayTitle(s_msg.rcptGroup ? MSGS_TITLE_PICK_TG : MSGS_TITLE_PICK_PC);

	if (count == 0)
	{
		displayPrintCentered(56, MSGS_NO_CONTACTS, FONT_SIZE_2);
		displayPrintCentered(112, MSGS_HINT_BACK, FONT_SIZE_1);
		displayRender();
		return;
	}

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(count, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY) { continue; }
		if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)   { break; }

		CodeplugContact_t c;
		// 1-індексований API кодплагу: mNum йде з 0, тому +1.
		if (codeplugContactGetDataForNumberInType(mNum + 1, callType, &c))
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

	uint32_t callType = s_msg.rcptGroup ? CONTACT_CALLTYPE_TG : CONTACT_CALLTYPE_PC;
	int count = codeplugContactsGetCount(callType);

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		// Відміна - повертаємось на RECIPIENT, s_msg.rcpt/rcptGroup не чіпаємо.
		s_msg.view = MSG_RECIPIENT;
		recipientUpdate();
		return;
	}

	if (count == 0) { return; }   // порожній список - тільки RED працює

	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, count); pickContactUpdate(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, count); pickContactUpdate(); return;
	}

	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		CodeplugContact_t c;
		if (codeplugContactGetDataForNumberInType(menuDataGlobal.currentItemIndex + 1, callType, &c))
		{
			snprintf(s_msg.rcpt, sizeof s_msg.rcpt, "%lu", (unsigned long)c.tgNumber);
			s_msg.rcptPos = (int16_t)strlen(s_msg.rcpt);
			// s_msg.rcptGroup вже дорівнює тому, за яким типом (TG/PC) гортали список -
			// саме він визначив callType вище, тому явно його підтверджувати не потрібно.
		}
		s_msg.view = MSG_RECIPIENT;
		recipientUpdate();
		return;
	}
}

/* ============================ RESULT ================================== */
static void resultUpdate(void)
{
	displayClearBuf();
	menuDisplayTitle(MSGS_TITLE);
	if (s_msg.result == 0)
	{
		displayPrintCentered(44, MSGS_SENT_OK, FONT_SIZE_3);
		displayPrintCentered(72, MSGS_KEYED_TX, FONT_SIZE_2);
	}
	else
	{
		char buf[24];
		const char *why = MSGS_ERR_GENERIC;
		switch (s_msg.result)
		{
			case -2: why = MSGS_ERR_TX_BUSY;   break;
			case -1: why = MSGS_ERR_BAD_TEXT;  break;
			case -4: case -5: why = MSGS_ERR_TOO_LONG; break;
		}
		displayPrintCentered(44, MSGS_SEND_FAILED, FONT_SIZE_3);
		snprintf(buf, sizeof buf, MSGS_FAIL_FMT, why, s_msg.result);
		displayPrintCentered(72, buf, FONT_SIZE_2);
	}
	displayPrintCentered(112, MSGS_HINT_ANY_KEY, FONT_SIZE_1);
	displayRender();
}

#endif // ENABLE_AES && ENABLE_DMR_DATA

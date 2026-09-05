/*
 * menuRCTLConfig.c — Options > "Доступ RCTL": що саме дозволено робити з цією рацією
 * дистанційно (functions/dmr_rctl_cfg.c).
 *
 * МОДЕЛЬ (2026-09-05, за прямою вказівкою користувача, за зразком Motorola/Hytera):
 * доступ має будь-яка станція в мережі, і на ВІДКРИТОМУ каналі теж; вирішує не наявність
 * ключа, а сама цільова рація -- окремим дозволом на КОЖНУ команду:
 *
 *     Доступ      -- головний перемикач; вимкнено = не приймати нічого ні від кого
 *     Перевірка   -- відповідати на радіоперевірку
 *     Моніторинг  -- віддалене прослуховування     (команда ще НЕ реалізована)
 *     Вимкнення   -- блокування рації (stun)       (команда ще НЕ реалізована)
 *     Ввімкнення  -- розблокування рації (revive)  (команда ще НЕ реалізована)
 *
 * Три останні пункти вже є в меню навмисно: дозвіл -- це рішення власника рації, і воно
 * має бути прийняте ДО того, як команда з'явиться в ефірі, а не разом із нею. Поки команда
 * не реалізована, увімкнений дозвіл просто ні на що не впливає: dmrRctlTick() відкидає
 * невідомі команди в default-гілці.
 *
 * ЧОМУ ЦЕ ВАЖЛИВО САМЕ НА ВІДКРИТИХ КАНАЛАХ. Там немає ключа, отже немає й підтвердження,
 * що команда від свого: її може скласти будь-хто в зоні чутності. Ці перемикачі -- єдина
 * межа, тому за замовчуванням не ввімкнено нічого.
 *
 * Редагується РОБОЧА копія: LEFT/RIGHT перемикає виділений пункт, GRN зберігає й виходить,
 * RED виходить БЕЗ збереження -- щоб випадковий дотик по дорозі до іншого пункту меню не міг
 * тихо роздати права.
 *
 * Компілюється в ніщо без -DENABLE_AES -DENABLE_DMR_DATA.
 */
#include "user_interface/uiGlobals.h"
#include "user_interface/menuSystem.h"
#include "user_interface/uiLocalisation.h"
#include "user_interface/uiUtilities.h"
#include "functions/dmr_rctl_cfg.h"
#include "crypto/dmr_aes.h"
#include "crypto/dmr_aes_hook.h"
#include <string.h>

#if defined(LANGUAGE_BUILD_UKRAINIAN)
#include "user_interface/languages/rctl_ua.h"
#else
#define RCFG_TITLE          "RCTL access"
#define RCFG_HINT           "L/R:toggle  GRN:save"
#define RCFG_HINT2          "RED:exit without saving"
#define RCFG_ITEM_ACCESS    "Access"
#define RCFG_ITEM_CHECK     "Radio check"
#define RCFG_ITEM_MONITOR   "Monitor"
#define RCFG_ITEM_STUN      "Disable"
#define RCFG_ITEM_REVIVE    "Enable"
#endif

#if defined(ENABLE_AES) && defined(ENABLE_DMR_DATA)

enum { RCFG_ACCESS = 0, RCFG_CHECK, RCFG_MONITOR, RCFG_STUN, RCFG_REVIVE, RCFG_NUM_ITEMS };

// Персистентно між тіками, в CCM -- той самий idiom, що й menuAESKeys.c/menuMessages.c
// (CCM НЕ обнуляється при старті, тож усе ініціалізується на isFirstRun перед читанням;
// AMBE-кодек чутливий до зсуву .bss в основній RAM, тому нового статику там не додаємо).
static struct
{
	uint8_t enabled;   // робоча копія головного перемикача
	uint8_t allow;     // робоча копія маски дозволів
	bool    dirty;     // відрізняється від того, що зараз на флеші
} s_rcfg DMR_AES_CCM;

static void updateScreen(void);

/* Біт маски для пункту меню; для головного перемикача -- 0. */
static uint8_t itemBit(int item)
{
	switch (item)
	{
		case RCFG_CHECK:   return DMR_RCTL_ALLOW_CHECK;
		case RCFG_MONITOR: return DMR_RCTL_ALLOW_MONITOR;
		case RCFG_STUN:    return DMR_RCTL_ALLOW_STUN;
		case RCFG_REVIVE:  return DMR_RCTL_ALLOW_REVIVE;
		default:           return 0;
	}
}

menuStatus_t menuRCTLConfig(uiEvent_t *ev, bool isFirstRun)
{
	if (isFirstRun)
	{
		s_rcfg.enabled = (uint8_t)dmrRctlConfigEnabled();
		// dmrRctlAllowMask() повертає 0 при вимкненому доступі, тож для РЕДАГУВАННЯ маску
		// читаємо незалежно: інакше, вимкнувши доступ, користувач втрачав би виставлені
		// дозволи, щойно зайшов у це меню.
		s_rcfg.allow = dmrRctlConfigAllowRaw();
		s_rcfg.dirty = false;
		menuDataGlobal.currentItemIndex = 0;
		menuDataGlobal.numItems = RCFG_NUM_ITEMS;
		updateScreen();
		return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
	}

	menuStatus_t exitCode = MENU_STATUS_SUCCESS;

	if (ev->hasEvent && ((ev->events & KEY_EVENT) != 0))
	{
		if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
		{
			menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, RCFG_NUM_ITEMS);
			updateScreen();
			return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
		}
		if (KEYCHECK_PRESS(ev->keys, KEY_UP))
		{
			menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, RCFG_NUM_ITEMS);
			updateScreen();
			return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
		}
		if (KEYCHECK_PRESS(ev->keys, KEY_LEFT) || KEYCHECK_PRESS(ev->keys, KEY_RIGHT))
		{
			uint8_t bit = itemBit(menuDataGlobal.currentItemIndex);

			if (bit == 0) { s_rcfg.enabled = s_rcfg.enabled ? 0 : 1; }
			else          { s_rcfg.allow ^= bit; }

			s_rcfg.dirty = true;
			updateScreen();
			return exitCode;
		}
		if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
		{
			if (s_rcfg.dirty)
			{
				// Регіон custom-data міг ще не мати "OpenGD77"-магії (нова/порожня рація) --
				// та сама, спільна для всього регіону операція, що й перед записом
				// AES-ключів/тем. Безпечно викликати й повторно.
				dmrAesEnsureCustomDataRegion();
				dmrRctlConfigSetAllow(s_rcfg.allow);
				dmrRctlConfigSetEnabled(s_rcfg.enabled);   // пише блок останнім -> обидва поля у флеші
				s_rcfg.dirty = false;
			}
			menuSystemPopPreviousMenu();
			return exitCode;
		}
		if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
		{
			menuSystemPopPreviousMenu();   // без збереження
			return exitCode;
		}
	}
	return exitCode;
}

static void updateScreen(void)
{
	static const char *names[RCFG_NUM_ITEMS] = {
		RCFG_ITEM_ACCESS, RCFG_ITEM_CHECK, RCFG_ITEM_MONITOR, RCFG_ITEM_STUN, RCFG_ITEM_REVIVE
	};
	char buf[SCREEN_LINE_BUFFER_SIZE];

	displayClearBuf();
	menuDisplayTitle(RCFG_TITLE);

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(RCFG_NUM_ITEMS, i);

		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY) { continue; }
		if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)   { break; }

		uint8_t bit = itemBit(mNum);
		bool on = (bit == 0) ? (s_rcfg.enabled != 0) : ((s_rcfg.allow & bit) != 0);

		snprintf(buf, sizeof buf, "%s:%s", names[mNum], (on ? currentLanguage->on : currentLanguage->off));
		menuDisplayEntry(i, mNum, buf, (int32_t)(strlen(names[mNum]) + 1),
				THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}

	// П'ять пунктів у семи слотах списку займають y=32..112 (menuGetMenuOffset віддає
	// loopOffset -2..+2, MENU_ENTRY_HEIGHT=16, база 64), тож два рядки підказки нижче
	// нічого не перекривають: 112..120 і 120..128 при висоті екрана 128.
	displayPrintCentered(112, RCFG_HINT, FONT_SIZE_1);
	displayPrintCentered(120, RCFG_HINT2, FONT_SIZE_1);
	displayRender();
}

#endif // ENABLE_AES && ENABLE_DMR_DATA

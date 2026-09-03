/*
 * menuRCTLConfig.c — Options > "RCTL access": локальний перемикач "приймати команди
 * віддаленого керування" (functions/dmr_rctl_cfg.c).
 *
 * Модель довіри (ВИПРАВЛЕНО 2026-09-03, за прямою вказівкою користувача, за зразком
 * Motorola/Hytera): жодного списку довірених ID тут немає -- єдине поле цього екрана
 * це enabled. Увімкнено = приймати команди від БУДЬ-КОГО з правильним AES-ключем
 * каналу (тим самим, що й голос/SMS); вимкнено (за замовчуванням) = ні від кого.
 *
 * Мінімальний однопунктовий екран за зразком глобального TX-ключа в menuAESKeys.c:
 * LEFT/RIGHT перемикає РОБОЧУ копію в пам'яті, GRN зберігає й виходить, RED виходить
 * БЕЗ збереження -- щоб випадковий дотик до LEFT/RIGHT по дорозі до іншого пункту меню
 * не міг тихо ввімкнути/вимкнути прийом команд.
 *
 * До цього кроку (2026-09-03) єдиним способом увімкнути RCTL був ПК-інструмент
 * tools/rctl_config.py; тепер це можна зробити прямо з рації, без ПК, так само як
 * AES-ключі (menuAESKeys.c) чи SMS (menuMessages.c).
 *
 * Компілюється в ніщо без -DENABLE_AES -DENABLE_DMR_DATA (та сама умова, що й
 * functions/dmr_rctl_cfg.c/dmr_rctl_tx.c).
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
#define RCFG_TITLE      "RCTL access"
#define RCFG_STATE_FMT  "State: %s"
#define RCFG_HINT       "L/R:toggle GRN:save RED:cancel"
#endif

#if defined(ENABLE_AES) && defined(ENABLE_DMR_DATA)

// Персистентно між тіками, в CCM -- той самий idiom, що й menuAESKeys.c/menuMessages.c
// (CCM НЕ обнуляється при старті, тож усе ініціалізується на isFirstRun перед читанням;
// AMBE-кодек чутливий до зсуву .bss в основній RAM, тому нового статику там не додаємо).
static struct
{
	uint8_t enabled;   // робоча копія, що редагується на екрані (ще не збережена)
	bool    dirty;     // enabled відрізняється від того, що зараз на флеші
} s_rcfg DMR_AES_CCM;

static void updateScreen(void);

menuStatus_t menuRCTLConfig(uiEvent_t *ev, bool isFirstRun)
{
	if (isFirstRun)
	{
		s_rcfg.enabled = (uint8_t)dmrRctlConfigEnabled();
		s_rcfg.dirty = false;
		updateScreen();
		return MENU_STATUS_SUCCESS;
	}

	menuStatus_t exitCode = MENU_STATUS_SUCCESS;

	if (ev->hasEvent && ((ev->events & KEY_EVENT) != 0))
	{
		if (KEYCHECK_PRESS(ev->keys, KEY_LEFT) || KEYCHECK_PRESS(ev->keys, KEY_RIGHT))
		{
			s_rcfg.enabled = s_rcfg.enabled ? 0 : 1;
			s_rcfg.dirty = true;
			updateScreen();
			return exitCode;
		}
		if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
		{
			if (s_rcfg.dirty)
			{
				// Регіон custom-data міг ще не мати "OpenGD77"-магії (нова/порожня
				// рація) -- та сама, спільна для всього регіону операція, що й перед
				// записом AES-ключів/тем. Безпечно викликати й повторно.
				dmrAesEnsureCustomDataRegion();
				dmrRctlConfigSetEnabled(s_rcfg.enabled);
				s_rcfg.dirty = false;
			}
			menuSystemPopPreviousMenu();
			return exitCode;
		}
		if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
		{
			menuSystemPopPreviousMenu();   // без збереження -- s_rcfg.dirty просто лишається як є
			return exitCode;
		}
	}
	return exitCode;
}

static void updateScreen(void)
{
	char buf[40];

	displayClearBuf();
	menuDisplayTitle(RCFG_TITLE);

	snprintf(buf, sizeof buf, RCFG_STATE_FMT, s_rcfg.enabled ? currentLanguage->on : currentLanguage->off);
	displayPrintCentered(48, buf, FONT_SIZE_3);

	displayPrintCentered(112, RCFG_HINT, FONT_SIZE_1);
	displayRender();
}

#endif // ENABLE_AES && ENABLE_DMR_DATA

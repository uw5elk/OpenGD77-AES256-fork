/*
 * Copyright (C) 2019-2025 Roger Clark, VK3KYY / G4KYF
 *                         Daniel Caujolle-Bert, F1RMB
 *
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include "user_interface/uiGlobals.h"
#include "user_interface/menuSystem.h"
#include "user_interface/uiLocalisation.h"
#include "user_interface/uiUtilities.h"
#include "utils.h"


#if defined(PLATFORM_RD5R)
#define YCENTER         ((DISPLAY_SIZE_Y / 2) + 3)
#define INNERBOX_H      (FONT_SIZE_3_HEIGHT + 7)
#define YBAR            (YCENTER - ((FONT_SIZE_3_HEIGHT / 2) + 2))
#define YTEXT           (YBAR + 1)
#else
#define YCENTER         (DISPLAY_SIZE_Y / 2)
#define INNERBOX_H      (FONT_SIZE_3_HEIGHT + 6)
#define YBAR            (YCENTER - (FONT_SIZE_3_HEIGHT / 2))
#define YTEXT           (YBAR - 2)
#endif
#define XBAR            69
#define YBOX            (YCENTER - (INNERBOX_H / 2))


#if defined(CPU_MK22FN512VLL12) || defined(PLATFORM_MD9600)
#if defined(PLATFORM_RD5R)
#define BEARING_RADIUS              18
#define BEARING_CIRCLE_X_OFFSET     4
#define BEARING_BOX_H             ((BEARING_RADIUS + 5) * 2)
#else
#define BEARING_RADIUS              18
#define BEARING_CIRCLE_X_OFFSET     4
#define BEARING_BOX_H             ((BEARING_RADIUS + 5) * 2)
#endif
#else // defined(CPU_MK22FN512VLL12) || defined(PLATFORM_MD9600)
#define BEARING_RADIUS              24
#define BEARING_CIRCLE_X_OFFSET     13
#define BEARING_BOX_H             ((BEARING_RADIUS + 10) * 2)
#endif
#define BEARING_CIRCLE_X_POS       (BEARING_RADIUS + BEARING_CIRCLE_X_OFFSET)
#define BEARING_BOX_W              (DISPLAY_SIZE_X - 4)
#define BEARING_BOX_Y             ((DISPLAY_SIZE_Y - BEARING_BOX_H) / 2)
#define TOP_ARROW_OFFSET           (BEARING_RADIUS - 8)
#define BASE_ARROW_OFFSET         -(BEARING_RADIUS - 8)
#define BEARING_AND_DIST_X_CENTER (((DISPLAY_SIZE_X - ((BEARING_RADIUS * 2) + BEARING_CIRCLE_X_OFFSET)) / 2) + ((BEARING_RADIUS * 2) + BEARING_CIRCLE_X_OFFSET))


#if defined(PLATFORM_MD9600) || defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
static __attribute__((section(".data.$RAM2"))) uint8_t screenNotificationBufData[((DISPLAY_SIZE_X * DISPLAY_SIZE_Y) >> 3)];
#else
static  __attribute__((section(".ccmram"))) uint16_t screenNotificationBufData[DISPLAY_SIZE_X * DISPLAY_SIZE_Y];
#endif

typedef struct
{
	bool                         visible;
	ticksTimer_t                 hideTimer;
	uiNotificationType_t         type;
	uiNotificationID_t           id;
	char                         message[NOTIFICATION_MESSAGE_LEN_MAX];
	uiNotificationBearingInfo_t  bearingInfo;
} uiNotificationData_t;

static uiNotificationData_t notificationData =
{
		.visible = false,
		.hideTimer = { 0, 0 },
		.type = NOTIFICATION_TYPE_MAX,
		.id = NOTIFICATION_ID_NONE,
		.message = { 0 },
		.bearingInfo = { .bearing = 0.0, .distanceX10 = -1 }
};

#if defined(ENABLE_AES) && defined(ENABLE_DMR_DATA)
#include "functions/dmr_sms.h"        /* dmrSmsUnreadCount(): скільки ще непрочитаних */
#if defined(LANGUAGE_BUILD_UKRAINIAN)
#include "user_interface/languages/messages_ua.h"
#else
#define MSGS_MORE_UNREAD_FMT     "+%d unread"
#endif
#endif

static void displayMessage(void);
static void displaySmsFullScreen(void);
static void displayLevelCard(const char *title, const char *valueText, int16_t pct);

void uiNotificationShow(uiNotificationType_t type, uiNotificationID_t id, uint32_t msTimeout, const char *message, bool immediateRender)
{
	bool valid = true;

	if (notificationData.visible)
	{
		if (notificationData.id != id)
		{
			notificationData.visible = false;
			displayRender();
		}
	}

	memset(notificationData.message, 0, sizeof(notificationData.message));

	notificationData.type = type;
	notificationData.id = id;

	switch (type)
	{
		case NOTIFICATION_TYPE_SQUELCH:
#if defined(HAS_SOFT_VOLUME)
		case NOTIFICATION_TYPE_VOLUME:
#endif
			break;

		case NOTIFICATION_TYPE_POWER:
			break;

		case NOTIFICATION_TYPE_MESSAGE:
		case NOTIFICATION_TYPE_SMS:
			if (message)
			{
				snprintf(&notificationData.message[0], sizeof(notificationData.message), "%s", message);
			}
			break;

		case NOTIFICATION_TYPE_BEARING:
			break;

		default:
			valid = false;
			break;
	}

	if (valid)
	{
		notificationData.visible = true;
		if (immediateRender)
		{
			uiNotificationRefresh();
		}
		ticksTimerStart(&notificationData.hideTimer, msTimeout);
	}
}

void uiNotificationBearingShow(uiNotificationBearingInfo_t *bearing, uint32_t msTimeout, bool immediateRender)
{
	memcpy(&(notificationData.bearingInfo), bearing, sizeof(uiNotificationBearingInfo_t));
	uiNotificationShow(NOTIFICATION_TYPE_BEARING, NOTIFICATION_ID_BEARING, msTimeout, NULL, immediateRender);
}

void uiNotificationRefresh(void)
{
	if (notificationData.visible)
	{
		// copy the primary screen content
		memcpy(screenNotificationBufData, displayGetPrimaryScreenBuffer(), sizeof(screenNotificationBufData));

#if defined(PLATFORM_MD9600) || defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
		displayOverrideScreenBuffer(screenNotificationBufData);
#endif

		// Draw whatever
		switch (notificationData.type)
		{
			case NOTIFICATION_TYPE_SQUELCH:
#if defined(HAS_SOFT_VOLUME)
			case NOTIFICATION_TYPE_VOLUME:
#endif
			{
				// Форк: повноекранне вікно регулювання гучності / шумоподавлення (баг #5).
				// Саму картку малює спільна displayLevelCard() -- та сама, що й потужність.
				char valueText[SCREEN_LINE_BUFFER_SIZE];
				int16_t pct;
				const char *title;

#if defined(HAS_SOFT_VOLUME)
				if (notificationData.type == NOTIFICATION_TYPE_VOLUME)
				{
					uint16_t volValue = CLAMP((lastVolume + 32), 0, 62);

					pct = (int16_t)((volValue * 100) / 62);
					title = currentLanguage->volume;
				}
				else
#endif
				{
					pct = (int16_t)(((currentChannelData->sql - CODEPLUG_MIN_VARIABLE_SQUELCH) * 100) /
						(CODEPLUG_MAX_VARIABLE_SQUELCH - CODEPLUG_MIN_VARIABLE_SQUELCH));
					title = currentLanguage->squelch;
				}

				pct = CLAMP(pct, 0, 100);
				snprintf(valueText, SCREEN_LINE_BUFFER_SIZE, "%d%%", pct);
				displayLevelCard(title, valueText, pct);
			}
			break;

			case NOTIFICATION_TYPE_POWER:
			{
				// Форк: та сама повноекранна картка, що й гучність/шумоподавлення (раніше
				// була вузька рамка по центру, що перекривала S-метр і обрізалась праворуч
				// -- виглядала інакше й гірше читалась у польових умовах). Формат великого
				// значення лишився той самий -- "500mW"/"1W"/"5W" з getPowerLevel()+Unit().
				// Смуга -- позиція рівня потужності серед УСІХ можливих (0..MAX_POWER_SETTING_NUM),
				// а не відсоток від безперервного діапазону, як у гучності/шумоподавлення.
				char valueText[SCREEN_LINE_BUFFER_SIZE];
				uint8_t powerLevel = trxGetPowerLevel();
				int16_t pct = (int16_t)(((int32_t)powerLevel * 100) / MAX_POWER_SETTING_NUM);

				snprintf(valueText, SCREEN_LINE_BUFFER_SIZE, "%s%s", getPowerLevel(powerLevel), getPowerLevelUnit(powerLevel));
				displayLevelCard(currentLanguage->power, valueText, pct);
			}
			break;

			case NOTIFICATION_TYPE_MESSAGE:
				displayMessage();
				break;

			case NOTIFICATION_TYPE_SMS:
				displaySmsFullScreen();
				break;

			case NOTIFICATION_TYPE_BEARING:
			{
				char buffer[SCREEN_LINE_BUFFER_SIZE];
				int16_t x1, x2, x3, y1, y2, y3;

				displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
				displayDrawRoundRectWithDropShadow(1, BEARING_BOX_Y, (DISPLAY_SIZE_X - 4), BEARING_BOX_H, 3, true);
				displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);

				// Circle
				for (int16_t i = 0; i < 360; i += 15)
				{
					x1 = (int16_t)(BEARING_CIRCLE_X_POS + (BEARING_RADIUS) * cos((double)(i * DEG_TO_RAD)));
					y1 = (int16_t)(((DISPLAY_SIZE_Y >> 1) - 3) + (BEARING_RADIUS) * sin((double)(i * DEG_TO_RAD)));

					x2 = (int16_t)(BEARING_CIRCLE_X_POS + (BEARING_RADIUS - ((i % 45) ? 2 : 4)) * cos((double)(i * DEG_TO_RAD)));
					y2 = (int16_t)(((DISPLAY_SIZE_Y >> 1) - 3) + (BEARING_RADIUS - ((i % 45) ? 2 : 4)) * sin((double)(i * DEG_TO_RAD)));

					if (i == 270)
					{
						displayThemeApply(THEME_ITEM_FG_GPS_COLOUR, THEME_ITEM_BG_NOTIFICATION);
						displayDrawLine((x1 - 1), y1, (x2 - 1), y2, true);
					}

					displayDrawLine(x1, y1, x2, y2, true);

					if (i == 270)
					{
						displayDrawLine((x1 + 1), y1, (x2 + 1), y2, true);
						displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
					}
				}


				displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
				displayDrawCircle(BEARING_CIRCLE_X_POS, ((DISPLAY_SIZE_Y >> 1) - 3), (BEARING_RADIUS + 1), true);
				displayThemeApply(THEME_ITEM_FG_GPS_COLOUR, THEME_ITEM_BG_NOTIFICATION);

				// Needle
				double needle = (notificationData.bearingInfo.bearing - 90.0);

				if (needle > 360.0)
				{
					needle -= 360.0;
				}

				needle *= DEG_TO_RAD;

				x1 = (int16_t)(BEARING_CIRCLE_X_POS + TOP_ARROW_OFFSET * cos(needle));
				y1 = (int16_t)(((DISPLAY_SIZE_Y >> 1) - 3) + TOP_ARROW_OFFSET * sin(needle));

				x2 = (int16_t)(BEARING_CIRCLE_X_POS + BASE_ARROW_OFFSET * cos(needle + (30 * DEG_TO_RAD)));
				y2 = (int16_t)(((DISPLAY_SIZE_Y >> 1) - 3) + BASE_ARROW_OFFSET * sin(needle + (30 * DEG_TO_RAD)));

				x3 = (int16_t)(BEARING_CIRCLE_X_POS + BASE_ARROW_OFFSET * cos(needle - (30 * DEG_TO_RAD)));
				y3 = (int16_t)(((DISPLAY_SIZE_Y >> 1) - 3) + BASE_ARROW_OFFSET * sin(needle - (30 * DEG_TO_RAD)));

				displayFillTriangle(x1, y1, x2, y2, x3, y3, true);

				// Bearing and Distance values
				displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);

				snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%u%c", ((uint32_t)notificationData.bearingInfo.bearing), 176);
				displayPrintAt((BEARING_AND_DIST_X_CENTER - ((strlen(buffer) * 8) >> 1)),
						(((DISPLAY_SIZE_Y >> 1) - 3) - FONT_SIZE_3_HEIGHT), buffer, FONT_SIZE_3);

				if (notificationData.bearingInfo.distanceX10 >= 0)
				{
					uint32_t intPart = (notificationData.bearingInfo.distanceX10 / 10);
					uint32_t decPart = (notificationData.bearingInfo.distanceX10 - (intPart * 10));

					snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%u.%u km", intPart, decPart);
					displayPrintAt((BEARING_AND_DIST_X_CENTER - ((strlen(buffer) * 8) >> 1)),
							(((DISPLAY_SIZE_Y >> 1) - 3) + 2), buffer, FONT_SIZE_3);
				}
			}
			break;

			default:
				break;
		}

		displayThemeResetToDefault();
		displayRenderWithoutNotification();
#if defined(PLATFORM_MD9600) || defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
		displayRestorePrimaryScreenBuffer();
#else
		memcpy(displayGetPrimaryScreenBuffer(), screenNotificationBufData, sizeof(screenNotificationBufData));
#endif
	}
}

bool uiNotificationHasTimedOut(void)
{
	/* Банер вхідного SMS сам НЕ зникає: повідомлення могло прийти, коли рації не було в
	 * руках, і зникнувши за 4 с воно лишалось непоміченим. Прибирає його лише оператор
	 * червоною кнопкою (див. applicationMain.c). */
	if (notificationData.visible && (notificationData.type == NOTIFICATION_TYPE_SMS))
	{
		return false;
	}
	return (notificationData.visible && ticksTimerHasExpired(&notificationData.hideTimer));
}

/* Чи показано саме банер SMS -- щоб головний цикл знав, що RED має його закрити. */
bool uiNotificationIsSms(void)
{
	return (notificationData.visible && (notificationData.type == NOTIFICATION_TYPE_SMS));
}

bool uiNotificationIsVisible(void)
{
	return notificationData.visible;
}

void uiNotificationHide(bool immediateRender)
{
	notificationData.visible = false;
	uiDataGlobal.displayQSOState = uiDataGlobal.displayQSOStatePrev;

	if (immediateRender)
	{
		displayRender();
	}
}

uiNotificationID_t uiNotificationGetId(void)
{
	return notificationData.id;
}

/* Форк: спільна повноекранна картка "заголовок + велике значення + смуга-індикатор".
 * Спершу писана для гучності/шумоподавлення (баг #5, 2026-09-11), тепер перевикористана й
 * для потужності (2026-09-18): та сама картка виглядає й поводиться однаково для всіх
 * трьох -- лише подача (значення для показу й позиція смуги) різна для кожного типу,
 * рахує її сам виклик у uiNotificationRefresh(). Керування (валкодер/таймаут) тут не
 * чіпаємо -- воно й так живе поза відмальовкою, у коді, що змінює currentChannelData->sql /
 * lastVolume / trxSetPowerFromLevel() і викликає uiNotificationShow().
 *
 * valueText -- вже ГОТОВИЙ рядок для великого числа по центру ("42%" для гучності/
 * шумоподавлення, "500mW"/"1W"/"5W" для потужності -- формат не змінюємо, лише переносимо
 * малювання). pct (0-100) -- де саме заповнена смуга; для потужності це позиція серед
 * дискретних рівнів (mW/W), а не відсоток від безперервного діапазону. */
static void displayLevelCard(const char *title, const char *valueText, int16_t pct)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];
	const int16_t barX = 14;
	const int16_t barW = (DISPLAY_SIZE_X - (2 * barX));
	const int16_t barY = 92;
	const int16_t barH = 18;
	int16_t fillW;

	pct = CLAMP(pct, 0, 100);

	// Форк: кольори за темою. НІЧ -- яскраво-жовтий на темному (максимальна
	// читабельність у русі/полі). ДЕНЬ -- темно-синій на білому (на прохання
	// fleet: жовтий на світлому тлі денної теми не читався б).
	uint16_t colBg, colFg, colDec;
	if (DAYTIME_CURRENT == NIGHT)
	{
		colBg  = PLATFORM_COLOUR_FORMAT_SWAP_BYTES(RGB888_TO_PLATFORM_COLOUR_FORMAT(0x0C0C14));
		colFg  = PLATFORM_COLOUR_FORMAT_SWAP_BYTES(RGB888_TO_PLATFORM_COLOUR_FORMAT(0xFFE000));
		colDec = PLATFORM_COLOUR_FORMAT_SWAP_BYTES(RGB888_TO_PLATFORM_COLOUR_FORMAT(0x786914));
	}
	else
	{
		colBg  = PLATFORM_COLOUR_FORMAT_SWAP_BYTES(RGB888_TO_PLATFORM_COLOUR_FORMAT(0xFFFFFF));  // біле тло
		colFg  = PLATFORM_COLOUR_FORMAT_SWAP_BYTES(RGB888_TO_PLATFORM_COLOUR_FORMAT(0x0A2A6B));  // темно-синій текст/смуга
		colDec = PLATFORM_COLOUR_FORMAT_SWAP_BYTES(RGB888_TO_PLATFORM_COLOUR_FORMAT(0x6E86C0));  // світліший синій -- рамка
	}

	// Фон на весь екран
	displaySetForegroundAndBackgroundColours(colFg, colBg);
	displayFillRect(0, 0, DISPLAY_SIZE_X, DISPLAY_SIZE_Y, true);

	// Рамка
	displaySetForegroundAndBackgroundColours(colDec, colBg);
	displayDrawRect(1, 1, (DISPLAY_SIZE_X - 2), (DISPLAY_SIZE_Y - 2), false);

	// Заголовок
	displaySetForegroundAndBackgroundColours(colFg, colBg);
	strncpy(buffer, title, (SCREEN_LINE_BUFFER_SIZE - 1));
	buffer[SCREEN_LINE_BUFFER_SIZE - 1] = 0;
	displayPrintCentered(16, buffer, FONT_SIZE_3);

	// Велике значення по центру (готовий рядок -- "42%" або "500mW" тощо)
	strncpy(buffer, valueText, (SCREEN_LINE_BUFFER_SIZE - 1));
	buffer[SCREEN_LINE_BUFFER_SIZE - 1] = 0;
	displayPrintCentered(44, buffer, FONT_SIZE_4);

	// Смуга-індикатор
	displaySetForegroundAndBackgroundColours(colDec, colBg);
	displayDrawRect(barX, barY, barW, barH, false);

	fillW = (int16_t)(((barW - 4) * pct) / 100);
	if (fillW > 0)
	{
		displaySetForegroundAndBackgroundColours(colFg, colBg);
		displayFillRect((barX + 2), (barY + 2), fillW, (barH - 4), false);
	}
}

/* Форк: вхідне SMS на ВЕСЬ екран, з переносом по словах, тримається до RED.
 * Чому не звичайний банер: той малює 3-4 рядки по 14 символів у рамці й гасне за 4 с --
 * для повідомлення до 144 символів цього замало, а якщо рація лежала в розвантажці, воно
 * зникало непоміченим. Тут використовуємо всю площу й чекаємо на оператора. */
static void displaySmsFullScreen(void)
{
	/* ВЕЛИКИЙ шрифт: font_8x16 (FONT_SIZE_3) -- 8 px завширшки, 16 завввишки. Ширина
	 * символу та сама, що в FONT_SIZE_2, тож у рядок так само влазить 19 символів, а от
	 * рядків стає вдвічі менше. Тому підказку "RED" прибрано (оператор і так знає, а
	 * місце вона з'їдала цілим рядком), а лічильник непрочитаних лишили дрібним. */
	const int16_t charW = 8;
	const int16_t lineH = FONT_SIZE_3_HEIGHT;      /* 16 */
	const int16_t cntH  = FONT_SIZE_2_HEIGHT;      /* 8 -- рядок "ще N непрочит." */
	int16_t perLine = (int16_t)((DISPLAY_SIZE_X - 6) / charW);
	char line[32];
	int more = 0;

#if defined(ENABLE_AES) && defined(ENABLE_DMR_DATA)
	more = dmrSmsUnreadCount() - 1;                /* показане теж рахується непрочитаним */
	if (more < 0) { more = 0; }
#endif

	/* Рядок лічильника з'їдає місце лише тоді, коли він справді є. */
	int16_t reserved = (int16_t)((more > 0) ? (cntH + 2) : 0);
	int16_t maxLines = (int16_t)((DISPLAY_SIZE_Y - 4 - reserved) / lineH);

	if (perLine > (int16_t)(sizeof line - 1)) { perLine = (int16_t)(sizeof line - 1); }
	if (maxLines < 1) { maxLines = 1; }

	/* Фон на весь екран + рамка. isInverted=true -> заливка КОЛЬОРОМ ФОНУ
	 * (displayFillRect: isInverted ? bg : fg); у displayDrawRect полярність обернена
	 * (лінія через !isInverted), тож видима рамка -- теж true. */
	displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);
	displayFillRect(0, 0, DISPLAY_SIZE_X, DISPLAY_SIZE_Y, true);
	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
	displayDrawRect(0, 0, DISPLAY_SIZE_X, DISPLAY_SIZE_Y, true);
	displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);

	const char *p = notificationData.message;
	int16_t y = 2;
	int16_t used = 0;

	while ((*p != 0) && (used < maxLines))
	{
		while (*p == ' ') { p++; }               /* не починати рядок із пробілу */
		if (*p == 0) { break; }

		int16_t n = 0;
		int16_t lastSpace = -1;
		while ((p[n] != 0) && (p[n] != '\n') && (n < perLine))
		{
			if (p[n] == ' ') { lastSpace = n; }
			n++;
		}
		/* Перенос по словах: рвемо на останньому пробілі, якщо рядок обірвався посеред слова. */
		if ((p[n] != 0) && (p[n] != '\n') && (lastSpace > 0)) { n = lastSpace; }

		memcpy(line, p, (size_t)n);
		line[n] = 0;

		p += n;
		if (*p == '\n') { p++; }
		used++;

		/* Не влізло все -- ставимо трикрапку в кінці останнього рядка, щоб оператор бачив,
		 * що текст обрізано, і відкрив «Вхідні». Без цього обрізання виглядає як повний текст. */
		if ((used == maxLines) && (*p != 0) && (n > 0))
		{
			/* Саме ТРИ крапки: одна читалась би як звичайна крапка в кінці речення. */
			int16_t cut = (int16_t)((n <= (perLine - 3)) ? n : (perLine - 3));
			if (cut < 0) { cut = 0; }
			line[cut] = '.'; line[cut + 1] = '.'; line[cut + 2] = '.';
			line[cut + 3] = 0;
		}

		displayPrintCore(3, y, line, FONT_SIZE_3, TEXT_ALIGN_LEFT, false);
		y += lineH;
	}

	/* Скільки ще НЕпрочитаних, крім показаного -- окремим кольором (колір попередження),
	 * щоб не злилось із текстом самого повідомлення. */
#if defined(ENABLE_AES) && defined(ENABLE_DMR_DATA)
	if (more > 0)
	{
		char cnt[24];
		snprintf(cnt, sizeof cnt, MSGS_MORE_UNREAD_FMT, more);
		displayThemeApply(THEME_ITEM_FG_WARNING_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);
		displayPrintCentered((int16_t)(DISPLAY_SIZE_Y - cntH - 2), cnt, FONT_SIZE_2);
		displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);
	}
#endif
}

static void displayMessage(void)
{
	char msg[SCREEN_LINE_BUFFER_SIZE];
	size_t len = strlen(notificationData.message);
	char *pb = notificationData.message;
	char *pe = &notificationData.message[0] + len;
	char *p = strchr(notificationData.message, '\n');
	uint16_t count;
	uint16_t y = 0U;
	uint8_t linesCount = 1U;
	uint8_t maxLines =
#if defined(HAS_COLOURS) || defined(PLATFORM_RD5R)
			4U;
#else
			3U;
#endif

	while (p)
	{
		linesCount++;

		if (linesCount == maxLines)
		{
			break;
		}

		p = strchr(p + 1, '\n');
	}

	y = (((DISPLAY_SIZE_Y - (linesCount * FONT_SIZE_3_HEIGHT)) >> 1) - 3);

	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
	displayDrawRoundRectWithDropShadow(1, (y - 1), (DISPLAY_SIZE_X - 4), ((FONT_SIZE_3_HEIGHT * linesCount) + 6), 3, true);
	displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);

	p = strchr(notificationData.message, '\n');

	while (linesCount > 1U)
	{
		count = SAFE_MIN((uint16_t)(p - pb), 14U);

		memcpy(msg, pb, count);
		msg[count] = '\0';

		displayPrintCentered(y, msg, FONT_SIZE_3);
		y += FONT_SIZE_3_HEIGHT;

		pb = p + 1;
		p = strchr(pb, '\n');

		linesCount--;
	}

	if (pb < pe)
	{
		count = SAFE_MIN((uint16_t)(pe - pb), 14U);

		p++;

		// Looking for another new line
		if ((p = strchr(pb, '\n')) != NULL)
		{
			count = (uint16_t)(p - pb);
		}

		memcpy(msg, pb, count);
		msg[count] = '\0';

		displayPrintCentered(y, msg, FONT_SIZE_3);
	}
}

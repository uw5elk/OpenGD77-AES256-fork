/*
 * dmr_rctl_cfg.h — конфігураційний блок "RCTL" (список довірених видавців команд
 * віддаленого керування) в OpenGD77 custom-data флеші. Модель за зразком MSGC-блоку
 * SMS (dmr_sms.c): пише CHIRP-модуль (PC), прошивка лише читає — жодного запису
 * назад із самої рації, тож баг у прошивці не може зіпсувати/підмінити список
 * довірених ID.
 *
 * Не плутати з crypto/dmr_rctl_pdu.h — той файл суто логіка (пакування PDU,
 * allowlist-перевірка й anti-replay в RAM) і навмисно не залежить від STM32/флеш-API,
 * щоб лишатись тестованим на хості. Цей файл — тонкий місток між ним і реальною
 * флешкою кодплагу.
 */
#ifndef _OPENGD77_DMR_RCTL_CFG_H_
#define _OPENGD77_DMR_RCTL_CFG_H_

#include "crypto/dmr_rctl_pdu.h"

#if defined(ENABLE_DMR_DATA) && defined(ENABLE_AES)

/* Повертає єдиний рантайм-gate (лічильники anti-replay живуть лише в RAM, у флеш не
 * пишуться). Ледаче (пере)завантажується з блоку "RCTL" при першому виклику.
 * Відсутній/пошкоджений блок -> gate лишається вимкненим із порожнім allowlist —
 * fail closed, так само, якби цієї фічі в збірці не було взагалі. */
dmr_rctl_gate_t *dmrRctlGate(void);

/* Примусово перечитати блок "RCTL" при наступному dmrRctlGate()/dmrRctlConfigEnabled()
 * (напр. якщо кодплаг переписали через CPS у тому ж сеансі; тестам теж зручно). */
void dmrRctlConfigReload(void);

/* Дешева перевірка "чи взагалі є сенс щось робити" без побудови самого gate:
 * 1, якщо блок валідний, enabled=1 і allowlist непорожній; інакше 0. */
int dmrRctlConfigEnabled(void);

#endif /* ENABLE_DMR_DATA && ENABLE_AES */
#endif /* _OPENGD77_DMR_RCTL_CFG_H_ */

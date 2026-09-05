/*
 * dmr_rctl_stock.h — побудова й розбір команд керування У СТОКОВОМУ ФОРМАТІ TYT
 * (Motorola CSBK, ВІДКРИТИМ ТЕКСТОМ). Замінює власний 16-байтний PDU (dmr_rctl_pdu.h),
 * щоб форк-рація була сумісна з заводською: стокова розуміє наші команди, а ми — її.
 *
 * Формат реверснуто із САМОГО ЕФІРУ (2026-09-05): захоплено сирі CSBK зі стокової TYT
 * (tools/rctl_capture.py), CRC перевірено математично. Референс і провенанс — у
 * RCTL_COMPAT.md; python-двійник — tools/rctl_stock.py. Хостовий тест
 * (tests/test_dmr_rctl_stock.c) відтворює реальні захвати БАЙТ-У-БАЙТ.
 *
 * Кадр команди — один CSBK, 12 байт:
 *     [b0] [FID=0x10] [0x00] [cmd] [src:3] [dst:3] [crc:2]
 * Перед командою — послідовність преамбул CSBK:
 *     [0xBD] [0x00] [0x00] [лічильник] [dst:3] [src:3] [crc:2]
 * УВАГА: у команді адреси йдуть src,dst; у преамбулі — навпаки, dst,src (так шле стокова).
 *
 * Навмисно БЕЗ STM32/HR-C6000/флеш-залежностей — чиста функція, тестується на хості, як
 * dmr_rctl_pdu.c / dmr_rctl_frame.c. Радійна частина (keying data-виклику, trxDMRID,
 * дозволи) — у functions/dmr_rctl_tx.c.
 */
#ifndef DMR_RCTL_STOCK_H
#define DMR_RCTL_STOCK_H

#include <stdint.h>

typedef enum
{
	DMR_RCTL_STOCK_CHECK   = 0,   /* Radio Check   */
	DMR_RCTL_STOCK_MONITOR = 1,   /* Remote Monitor */
	DMR_RCTL_STOCK_ENABLE  = 2,   /* Radio Enable  (revive) */
	DMR_RCTL_STOCK_DISABLE = 3,   /* Radio Disable (stun)   */
	DMR_RCTL_STOCK_NUM_CMDS
} dmr_rctl_stock_cmd_t;

/* Скільки преамбул шле стокова перед командою (лічильник рахує вниз від цього числа).
 * З ефіру бачили старт 0x10; беремо 16 -> лічильник 16..1, потім команда. */
#define DMR_RCTL_STOCK_PREAMBLES   16

/* Тип-байт бургста в черзі dmrDataTxLoad() для CSBK (сторінка 0x04, рег 0x50: type<<4).
 * Те саме значення DTB_CSBK, що в dmr_rctl_frame.c/dmr_sms.c. */
#define DMR_RCTL_STOCK_BURST_CSBK  0x30

/* Черга бургстів = преамбули + команда, у форматі dmrDataTxLoad(): count*(1 тип + 12 даних).
 * q має вмістити >= (DMR_RCTL_STOCK_PREAMBLES+1)*13 байт. Повертає кількість бургстів. */
int dmr_rctl_stock_build_tx(dmr_rctl_stock_cmd_t cmd, uint32_t src, uint32_t dst, uint8_t *q);

/* Зібрати один 12-байтний CSBK команди (для тестів / точкового використання). */
void dmr_rctl_stock_command(dmr_rctl_stock_cmd_t cmd, uint32_t src, uint32_t dst, uint8_t out12[12]);

/* Зібрати один 12-байтний CSBK преамбули. */
void dmr_rctl_stock_preamble(uint32_t dst, uint32_t src, uint8_t countdown, uint8_t out12[12]);

/* Розібрати вхідний CSBK (12 байт, як його віддає чип). Повертає 1, якщо це впізнана
 * стокова команда керування З ВІРНИМ CRC, і заповнює *cmd/*src/*dst. Інакше 0.
 * Преамбули (0xBD) НЕ вважаються командою — повертає 0. */
int dmr_rctl_stock_parse(const uint8_t in12[12], dmr_rctl_stock_cmd_t *cmd, uint32_t *src, uint32_t *dst);

/* Чи має ЦЯ рація виконати команду cmd, адресовану на dst, якщо наш DMR ID = ourId, а
 * маска дозволів (DMR_RCTL_ALLOW_* із dmr_rctl_pdu.h) = allowMask. Чиста функція, щоб
 * гейт прийому був хостово-тестований окремо від радійної частини:
 *   - dst має точно збігатися з ourId (RCTL завжди індивідуальний виклик, без All-Call);
 *   - для команди має бути виставлений відповідний біт дозволу:
 *       Check->ALLOW_CHECK, Monitor->ALLOW_MONITOR, Enable->ALLOW_REVIVE, Disable->ALLOW_STUN.
 * Повертає 1 = виконувати, 0 = ігнорувати (не нам або не дозволено). Fail-closed. */
int dmr_rctl_stock_should_act(dmr_rctl_stock_cmd_t cmd, uint32_t dst, uint32_t ourId, uint8_t allowMask);

#endif /* DMR_RCTL_STOCK_H */

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
 * стокова команда керування З ВІРНИМ CRC, і заповнює cmd/src/dst. Інакше 0.
 * Преамбули (0xBD) НЕ вважаються командою — повертає 0. */
int dmr_rctl_stock_parse(const uint8_t in12[12], dmr_rctl_stock_cmd_t *cmd, uint32_t *src, uint32_t *dst);

/* ---- Квитанція (ACK) на Radio Check ------------------------------------------
 * Формат знято з ефіру HackRF Portapack 2026-09-11 і декодовано власним декодером
 * (tools/dmr_csbk_rx.py); CRC валідний, байти відтворено у двох незалежних записах.
 * Доведено вимкненням цілі: без увімкненої цілі цієї передачі в ефірі немає взагалі.
 * Провенанс і сирі байти — RCTL_COMPAT.md §5a.
 *
 * Квитанція = ТОЙ САМИЙ кадр, що й команда Radio Check, лише з виставленим СТАРШИМ
 * БІТОМ керуючого байта (0x00 -> 0x80). Адреси місцями НЕ міняються — вони просто
 * віддзеркалюються з команди (поле1 = хто питав, поле2 = хто відповідає):
 *     a4 10 00 80 <хто питав:3> <хто відповідає:3> <crc:2>
 *
 * Стокова повторює квитанцію ~3 рази (хвіст в ефірі 83 мс ≈ 3 бургсти) і починає її
 * через ~37 мс після кінця команди. Слот-точність НЕ потрібна: командир чекає
 * відповідь ~1.3 с (з ефіру — саме з таким періодом він повторює запит, коли цілі нема). */
#define DMR_RCTL_STOCK_ACK_ARG      0x80
#define DMR_RCTL_STOCK_ACK_REPEATS  3

/* Правило узагальнено на ВСІ команди й підтверджено байтами з ефіру (2026-09-12,
 * кільце захвату самого форка, RCTL_COMPAT.md §6): квитанція = та сама команда зі
 * вставленим старшим бітом керуючого байта:
 *     Check   0x00 -> 0x80     a4 10 00 80 <хто питав:3> <хто відповів:3> <crc:2>
 *     Enable  0x7E -> 0xFE     a4 10 00 fe ...
 *     Disable 0x7F -> 0xFF     a4 10 00 ff ...
 * Байт b0 й адреси лишаються такими самими, як у команді. */
#define DMR_RCTL_STOCK_ACK_BIT      0x80

/* Зібрати один 12-байтний CSBK квитанції. */
void dmr_rctl_stock_ack_for(dmr_rctl_stock_cmd_t cmd, uint32_t requester, uint32_t responder, uint8_t out12[12]);
/* Сумісність: квитанція саме на Radio Check. */
void dmr_rctl_stock_ack(uint32_t requester, uint32_t responder, uint8_t out12[12]);

/* Черга бургстів квитанції у форматі dmrDataTxLoad() (повтори підряд, без преамбул —
 * так шле стокова). q має вмістити >= DMR_RCTL_STOCK_ACK_REPEATS*13 байт.
 * Повертає кількість бургстів. */
int dmr_rctl_stock_build_ack_tx_for(dmr_rctl_stock_cmd_t cmd, uint32_t requester, uint32_t responder, uint8_t *q);
int dmr_rctl_stock_build_ack_tx(uint32_t requester, uint32_t responder, uint8_t *q);

/* Розібрати вхідний CSBK як квитанцію (потрібно командирському боку, щоб показати
 * «онлайн»). Повертає 1 і заповнює requester/responder, якщо CRC вірний. */
int dmr_rctl_stock_parse_ack_for(const uint8_t in12[12], dmr_rctl_stock_cmd_t *cmd,
                                 uint32_t *requester, uint32_t *responder);
int dmr_rctl_stock_parse_ack(const uint8_t in12[12], uint32_t *requester, uint32_t *responder);

/* Чи має ЦЯ рація виконати команду cmd, адресовану на dst, якщо наш DMR ID = ourId, а
 * маска дозволів (DMR_RCTL_ALLOW_* із dmr_rctl_pdu.h) = allowMask. Чиста функція, щоб
 * гейт прийому був хостово-тестований окремо від радійної частини:
 *   - dst має точно збігатися з ourId (RCTL завжди індивідуальний виклик, без All-Call);
 *   - для команди має бути виставлений відповідний біт дозволу:
 *       Check->ALLOW_CHECK, Monitor->ALLOW_MONITOR, Enable->ALLOW_REVIVE, Disable->ALLOW_STUN.
 * Повертає 1 = виконувати, 0 = ігнорувати (не нам або не дозволено). Fail-closed. */
int dmr_rctl_stock_should_act(dmr_rctl_stock_cmd_t cmd, uint32_t dst, uint32_t ourId, uint8_t allowMask);

#endif /* DMR_RCTL_STOCK_H */

/*
 * dmr_rctl_tx.h — радійна частина RCTL: TX (ключує data-call через dmrDataTxLoad(), як
 * dmr_sms.c) і RX (ISR-збірка бургстів + декодування в основному циклі). Клей між чистими
 * crypto/dmr_rctl_pdu.c + crypto/dmr_rctl_frame.c і функцією functions/dmr_rctl_cfg.c
 * (allowlist/anti-replay gate).
 *
 * Компілюється в ніщо без -DENABLE_AES -DENABLE_DMR_DATA (дзеркалить dmr_sms.h/.c).
 *
 * Фаза 1 (2026-09-03, PLANS.md §3): реалізовано лише CHECK_REQ/CHECK_ACK ("рація на
 * зв'язку?"). MONITOR_START/STOP, STUN, REVIVE навмисно ІГНОРУЮТЬСЯ на прийомі -- логіку
 * для них ще не написано (див. коментарі в dmr_rctl_pdu.h). Це свідомий, обережний крок
 * "по черзі": спершу перевірений, найбезпечніший підклас команд (запит присутності, без
 * жодної дії над чужою рацією), решта -- окремими кроками потім.
 */
#ifndef _OPENGD77_DMR_RCTL_TX_H_
#define _OPENGD77_DMR_RCTL_TX_H_

#include <stdint.h>

#if defined(ENABLE_DMR_DATA) && defined(ENABLE_AES)

/* ---- TX ------------------------------------------------------------------ *
 * Надіслати команду RCTL адресату targetId (індивідуальний DMR ID). issuerId
 * підставляється автоматично (trxDMRID), seq -- монотонний лічильник цього сеансу роботи.
 * Шифрується поточним каналовим AES-ключем (та сама логіка вибору ключа, що й голос/SMS);
 * якщо ключа немає -- команда НЕ йде в ефір (керування без шифру неможливе за дизайном).
 * Повертає 0 успіх, -2 канал даних вже зайнятий, -3 немає завантаженого ключа. */
int dmrRctlSendCmd(uint32_t targetId, uint8_t cmd, uint32_t arg);

/* Зручна обгортка: запит "чи ти на зв'язку?" (DMR_RCTL_CMD_CHECK_REQ, arg=0). */
int dmrRctlRequestCheck(uint32_t targetId);

/* Надіслати команду керування у СТОКОВОМУ форматі TYT (сумісність із заводською рацією).
 * cmd -- dmr_rctl_stock_cmd_t (0=Check,1=Monitor,2=Enable,3=Disable). Без AES/ключа.
 * 0 = поставлено в чергу, -2 = дата-виклик активний, -4 = не зібралось. */
int dmrRctlStockSend(int cmd, uint32_t targetId);

/* Останній отриманий CHECK_ACK: 1 якщо був хоч один з моменту завантаження, з ID видавця
 * (тобто радіостанції, що відповіла) та віком відповіді в мс. Використовується екраном
 * "Remote control" (menuRCTLRemote.c, Фаза 1б, 2026-09-03). */
int dmrRctlLastCheckAck(uint32_t *outFromId, uint32_t *outAgeMs);

/* Монотонний лічильник ПРИЙНЯТИХ CHECK_ACK з моменту завантаження (від будь-якого
 * видавця, зростає на 1 при кожному). Потрібен екрану "Remote control", щоб відрізнити
 * "прийшла НОВА відповідь після мого запиту" від застарілого стану dmrRctlLastCheckAck()
 * ще з попереднього, не пов'язаного запиту -- порівнювати значення до/після запиту
 * через "!=" (лічильник просто зростає, переповнення на практиці не досяжне). */
uint32_t dmrRctlAckGeneration(void);

/* ---- RX (ISR-контекст HR-C6000) ------------------------------------------ *
 * Викликати для КОЖНОГО CRC-валідного бургста класу data-sync (rxDataType: 6=Data Header,
 * 7=Rate-1/2), паралельно з dmrSmsRxBurst() -- той самий вже прочитаний 12-байтний буфер,
 * повторний SPI-запит не потрібен. Незалежний стан збірки, не заважає SMS і навпаки. */
void dmrRctlRxBurst(int rxDataType, const uint8_t *payload12);
/* Скинути часткову збірку (виклик на Terminator/кінець виклику, поруч з dmrSmsRxReset()). */
void dmrRctlRxReset(void);

/* ---- RX (основний цикл) --------------------------------------------------- *
 * Розшифрувати зібраний PDU, перевірити allowlist/anti-replay (dmrRctlGate()) і,
 * якщо команда відома й дозволена -- виконати (Фаза 1: лише авто-відповідь на
 * CHECK_REQ). Безпечно викликати щотік; нічого не робить, доки немає зібраного кадру. */
void dmrRctlTick(void);

#else

static inline int  dmrRctlSendCmd(uint32_t targetId, uint8_t cmd, uint32_t arg) { (void)targetId; (void)cmd; (void)arg; return -1; }
static inline int  dmrRctlRequestCheck(uint32_t targetId) { (void)targetId; return -1; }
static inline int  dmrRctlStockSend(int cmd, uint32_t targetId) { (void)cmd; (void)targetId; return -1; }
static inline int  dmrRctlLastCheckAck(uint32_t *outFromId, uint32_t *outAgeMs) { (void)outFromId; (void)outAgeMs; return 0; }
static inline uint32_t dmrRctlAckGeneration(void) { return 0; }
static inline void dmrRctlRxBurst(int rxDataType, const uint8_t *payload12) { (void)rxDataType; (void)payload12; }
static inline void dmrRctlRxReset(void) { }
static inline void dmrRctlTick(void) { }

#endif /* ENABLE_DMR_DATA && ENABLE_AES */
#endif /* _OPENGD77_DMR_RCTL_TX_H_ */

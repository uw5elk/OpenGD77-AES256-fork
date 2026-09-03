/*
 * dmr_rctl_frame.h — чиста побудова ефірного кадру (DMR data-call burst queue) для однієї
 * команди RCTL. Дзеркалить TX-секцію dmr_sms.c (той самий CSBK-преамбул + Unconfirmed Data
 * Header + Motorola ENC extended header + rate-1/2 блоки), але спрощено під фіксований
 * 16-байтний PDU (crypto/dmr_rctl_pdu.h), який ЗАВЖДИ шифрується цілком одним блоком AES
 * (на відміну від SMS, де хвіст може лишатись відкритим тексом).
 *
 * Навмисно БЕЗ жодної залежності від STM32/HR-C6000/флеш/globals (як і dmr_rctl_pdu.c) —
 * компілюється й тестується на хості (tests/test_dmr_rctl_frame.c). Радійна частина
 * (виклик dmrDataTxLoad(), вибір ключа/trxDMRID) — у functions/dmr_rctl_tx.c.
 */
#ifndef DMR_RCTL_FRAME_H
#define DMR_RCTL_FRAME_H

#include <stdint.h>
#include "crypto/dmr_rctl_pdu.h"

/* 6 CSBK-преамбул + Unconfirmed Data Header + ENC extended header + 2 rate-1/2 блоки з
 * 16-байтним зашифрованим PDU (24 B = ct(16)+pad(4)+crc32(4), поділені по 12 B/блок). */
#define DMR_RCTL_TX_BURST_COUNT  10

/* DMR data-PDU CRC32 (побайтовий своп пар, поліном 0x04C11DB7, рахується над (len*8-32)
 * бітами) — побайтова копія crc32_dmr() з dmr_sms.c. Винесено сюди й названо публічно, щоб
 * RX-збирач (functions/dmr_rctl_tx.c) перевіряв прийнятий кадр ТІЄЮ Ж функцією, якою
 * побудований вихідний. */
uint32_t dmr_rctl_crc32(const uint8_t *pdu, int len);

/* Зібрати чергу бургстів для однієї команди RCTL, адресованої ІНДИВІДУАЛЬНИМ викликом на
 * dst (RCTL навмисно не має групового режиму). msg (cmd/issuerId/seq/arg) пакується
 * dmr_rctl_pack() і шифрується AES-256-ECB одним 16-байтним блоком під key; keyId лише
 * підписує ENC-заголовок (яким слотом ключа скористався відправник), щоб приймач спробував
 * саме його першим.
 * q має вмістити >= DMR_RCTL_TX_BURST_COUNT*13 байт (розкладка як у dmrDataTxLoad():
 * count*(1 байт типу + 12 байт корисного навантаження)).
 * Повертає DMR_RCTL_TX_BURST_COUNT (>0) при успіху, -1 якщо key==NULL.
 * Чиста функція — без STM32/флешу/глобального стану, повністю тестована на хості. */
int dmr_rctl_build_tx_bursts(uint32_t dst, uint32_t src, uint8_t keyId, const uint8_t key[32],
                              const dmr_rctl_msg_t *msg, uint8_t *q);

#endif /* DMR_RCTL_FRAME_H */

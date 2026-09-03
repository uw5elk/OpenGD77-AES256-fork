/*
 * dmr_rctl_tx.c — див. dmr_rctl_tx.h.
 *
 * TX-фрейминг: crypto/dmr_rctl_frame.c (чиста, хост-тестована). RX-збірка бургстів тут
 * побайтово дзеркалить dmrSmsRxBurst() з dmr_sms.c (той самий Unconfirmed Data Header /
 * ENC extended header розбір), але з ОКРЕМИМ статичним станом -- обидва збирачі викликаються
 * паралельно на кожен бургст (HR-C6000.c) і не заважають один одному.
 */
#include "functions/dmr_rctl_tx.h"

#if defined(ENABLE_DMR_DATA) && defined(ENABLE_AES)

#include "functions/dmr_data.h"
#include "functions/dmr_rctl_cfg.h"
#include "functions/trx.h"
#include "functions/ticks.h"
#include "functions/codeplug.h"
#include "functions/settings.h"
#include "crypto/dmr_aes.h"
#include "crypto/dmr_aes_hook.h"
#include "crypto/dmr_rctl_pdu.h"
#include "crypto/dmr_rctl_frame.h"
#include "user_interface/menuSystem.h"
#include <string.h>
#include <stdio.h>

#define DT_DATA_HEADER   6
#define DT_RATE12_DATA   7

/* ============================ TX ========================================= */

/* Той самий вибір ключа, що й голос/SMS (dзеркалить smsResolveTxKeyId() у dmr_sms.c):
 * глобальний TX-селектор, перевизначений каналовим байтом шифрування (0xFF -> відкрито,
 * 1..15 -> той слот, 0 -> успадкувати глобальний). RCTL без ключа НЕ передається (return -3
 * у dmrRctlSendCmd) -- керування без шифру неможливе за дизайном (dmr_rctl_pdu.h). */
static uint8_t rctlResolveTxKeyId(void)
{
	uint8_t keyId = dmrAesTxKeyId();
	if (currentChannelData != NULL)
	{
		uint8_t chEnc = codeplugChannelGetAesKeySlot(currentChannelData);
		if (chEnc == 0xFF) { keyId = 0; }
		else if ((chEnc >= 1) && (chEnc < DMR_AES_MAX_KEYS)) { keyId = chEnc; }
	}
	return keyId;
}

static uint32_t     s_txSeq;
static uint8_t       s_txSeqSeeded;

int dmrRctlSendCmd(uint32_t targetId, uint8_t cmd, uint32_t arg)
{
	if (dmrDataTxActive()) { return -2; }

	uint8_t keyId = rctlResolveTxKeyId();
	const uint8_t *key = (keyId != 0) ? dmr_aes_key_ptr(keyId) : NULL;
	if (key == NULL) { return -3; }

	if (!s_txSeqSeeded) { s_txSeq = (uint32_t)ticksGetMillis() | 1u; s_txSeqSeeded = 1; }
	s_txSeq++;

	dmr_rctl_msg_t m;
	m.cmd = cmd;
	m.issuerId = trxDMRID;
	m.seq = s_txSeq;
	m.arg = arg;

	uint8_t q[DMR_RCTL_TX_BURST_COUNT * 13];
	int n = dmr_rctl_build_tx_bursts(targetId, trxDMRID, keyId, key, &m, q);
	if (n <= 0) { return -4; }

	dmrDataTxLoad(q, (uint8_t)n);
	return 0;
}

int dmrRctlRequestCheck(uint32_t targetId)
{
	return dmrRctlSendCmd(targetId, DMR_RCTL_CMD_CHECK_REQ, 0);
}

static uint32_t s_lastAckFromId;
static uint32_t s_lastAckMillis;
static uint8_t  s_haveAck;
static uint32_t s_ackGen;   /* зростає на 1 при кожному прийнятому CHECK_ACK (dmrRctlTick()) */

int dmrRctlLastCheckAck(uint32_t *outFromId, uint32_t *outAgeMs)
{
	if (!s_haveAck) { return 0; }
	if (outFromId) { *outFromId = s_lastAckFromId; }
	if (outAgeMs)  { *outAgeMs = (uint32_t)(ticksGetMillis() - s_lastAckMillis); }
	return 1;
}

uint32_t dmrRctlAckGeneration(void)
{
	return s_ackGen;
}

/* ============================ RX (ISR) ==================================== */
/* Дзеркалить структуру ISR-стану dmrSmsRxBurst() -- окремий namespace, той самий буфер
 * бургста, що вже прочитала HR-C6000.c для SMS, повторний SPI-запит не потрібен. */
static volatile uint8_t  s_rxHaveHeader;
static volatile uint8_t  s_rxHaveEnc;
static volatile uint8_t  s_rxCount;
static volatile uint8_t  s_rxGroup;
static volatile uint8_t  s_rxKeyId;
static volatile uint32_t s_rxSrc;
static volatile uint32_t s_rxDst;
static uint8_t  s_rxBlocks[4][12];   /* RCTL завжди рівно 2 rate-1/2 блоки; трохи запасу */

static volatile uint8_t  s_rxReady;
static uint8_t  s_rxPdu[24];
static volatile uint16_t s_rxPduLen;
static volatile uint32_t s_rxPeerSrc;
static volatile uint32_t s_rxPeerDst;
static volatile uint8_t  s_rxPeerGroup;
static volatile uint8_t  s_rxPeerKeyId;

void dmrRctlRxReset(void)
{
	s_rxHaveHeader = 0; s_rxHaveEnc = 0; s_rxCount = 0;
}

void dmrRctlRxBurst(int rxDataType, const uint8_t *p)
{
	if (rxDataType == DT_DATA_HEADER)
	{
		if (p[0] == 0x4F && p[1] == 0x10 && (p[2] & 0x3F) == (0x51 & 0x3F))
		{
			s_rxKeyId = p[3];
			s_rxHaveEnc = 1;
			s_rxCount = 0;
		}
		else
		{
			s_rxGroup = (p[0] & 0x80) ? 1 : 0;
			s_rxDst = ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
			s_rxSrc = ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
			s_rxCount = 0;
			s_rxHaveHeader = 1;
			s_rxHaveEnc = 0;
		}
		return;
	}

	if (rxDataType == DT_RATE12_DATA)
	{
		if (!s_rxHaveHeader || s_rxCount >= 4) { return; }
		memcpy(s_rxBlocks[s_rxCount], p, 12);
		s_rxCount++;

		/* RCTL -- завжди РІВНО 2 блоки (24 B), на відміну від SMS зі змінною довжиною.
		 * CRC32 все одно перевіряємо (а не довіряємо лічильнику блоків), той самий
		 * захист від змішування бургстів пропущеного заголовка, що й у dmr_sms.c. */
		if (s_rxHaveHeader && (s_rxCount == 2) && !s_rxReady)
		{
			uint8_t pdu[24];
			memcpy(pdu, s_rxBlocks[0], 12);
			memcpy(pdu + 12, s_rxBlocks[1], 12);
			uint32_t want = ((uint32_t)pdu[20] << 24) | ((uint32_t)pdu[21] << 16) |
					((uint32_t)pdu[22] << 8) | (uint32_t)pdu[23];
			if (dmr_rctl_crc32(pdu, 24) != want)
			{
				return;   /* не повний/чистий кадр -- продовжуємо накопичувати (до 4) */
			}
			memcpy(s_rxPdu, pdu, 24);
			s_rxPduLen = 24;
			s_rxPeerSrc = s_rxSrc;
			s_rxPeerDst = s_rxDst;
			s_rxPeerGroup = s_rxGroup;
			s_rxPeerKeyId = s_rxKeyId;
			s_rxReady = 1;
			dmrRctlRxReset();
		}
	}
}

/* ============================ RX (основний цикл) ========================== */

void dmrRctlTick(void)
{
	if (!s_rxReady) { return; }

	uint8_t pdu[24];
	memcpy(pdu, s_rxPdu, 24);
	uint32_t src = s_rxPeerSrc;
	uint32_t dst = s_rxPeerDst;
	uint8_t  group = s_rxPeerGroup;
	uint8_t  keyId = s_rxPeerKeyId;
	s_rxReady = 0;

	/* RCTL за дизайном лише індивідуальний виклик (немає групового режиму керування) і
	 * має бути адресований САМЕ цій рації -- інакше це або чужа команда (не наша справа),
	 * або хтось намагається "підслухати" перевірку іншого адресата. */
	if (group || (dst != trxDMRID)) { return; }

	uint32_t want = ((uint32_t)pdu[20] << 24) | ((uint32_t)pdu[21] << 16) |
			((uint32_t)pdu[22] << 8) | (uint32_t)pdu[23];
	if (dmr_rctl_crc32(pdu, 24) != want) { return; }   /* захист від псевдо-збірки */

	dmr_rctl_msg_t msg;
	int got = 0;
	for (int attempt = 0; attempt <= DMR_AES_MAX_KEYS && !got; attempt++)
	{
		uint8_t k = (attempt == 0) ? keyId : (uint8_t)attempt;
		if (k == 0 || k >= DMR_AES_MAX_KEYS) { continue; }
		const uint8_t *key = dmr_aes_key_ptr(k);
		if (key == NULL) { continue; }
		uint8_t tmp[16];
		memcpy(tmp, pdu, 16);
		aes256_ecb_decrypt(key, tmp);
		if (dmr_rctl_unpack(tmp, &msg)) { got = 1; }
	}
	if (!got) { return; }             /* не наш ключ / не RCTL-кадр -- тихо ігноруємо */
	if (msg.issuerId != src) { return; }   /* заголовок і зашифрований issuerId мають збігатись */

	/* Допуск і захист від replay (allowlist з кодплагу, пише лише CHIRP) -- fail-closed:
	 * вимкнено або видавця немає в списку -> команда ІГНОРУЄТЬСЯ. */
	if (!dmr_rctl_gate_check(dmrRctlGate(), msg.issuerId, msg.seq)) { return; }

	switch (msg.cmd)
	{
		case DMR_RCTL_CMD_CHECK_REQ:
			/* Авто-відповідь. Best-effort: якщо канал даних саме зайнятий -- відповідь
			 * пропускається, видавець може повторити запит. */
			dmrRctlSendCmd(msg.issuerId, DMR_RCTL_CMD_CHECK_ACK, 0);
			break;

		case DMR_RCTL_CMD_CHECK_ACK:
		{
			s_lastAckFromId = msg.issuerId;
			s_lastAckMillis = ticksGetMillis();
			s_haveAck = 1;
			s_ackGen++;
			char note[40];
			snprintf(note, sizeof note, "Радіоперевірка: ID %lu на зв'язку", (unsigned long)msg.issuerId);
			uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 4000, note, true);
			break;
		}

		/* MONITOR_START/STOP, STUN, REVIVE -- НЕ реалізовано (Фаза 1 навмисно обмежена
		 * найбезпечнішою командою; див. dmr_rctl_tx.h). Мовчки ігноруємо, а не "падаємо"
		 * чи виконуємо частково. */
		default:
			break;
	}
}

#endif /* ENABLE_DMR_DATA && ENABLE_AES */

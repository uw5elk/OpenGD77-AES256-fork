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
#include "crypto/dmr_rctl_stock.h"   // стоковий формат команд (сумісність із заводською TYT)
#include "user_interface/menuSystem.h"
#include <string.h>
#include <stdio.h>

/* Український текст -- ЛИШЕ через rctl_ua.h (cp1251). Писати кирилицю прямо тут не
 * можна: файл у UTF-8, а шрифт рації індексується байтом cp1251, тож на екрані вийде
 * каша. Перевіряється автоматично -- tools/check_string_encoding.py. */
#if defined(LANGUAGE_BUILD_UKRAINIAN)
#include "user_interface/languages/rctl_ua.h"
#else
#define RCTL_NOTE_ACK_FMT        "Radio check: ID %lu"
#endif

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

int dmrRctlSendCmd(uint32_t targetId, uint8_t cmd, uint32_t arg)
{
	if (dmrDataTxActive()) { return -2; }

	/* Ключ НЕ обов'язковий (2026-09-05). На каналі без шифрування команда йде відкритим
	 * кадром -- як у Motorola/Hytera, де віддалене керування працює й у відкритій мережі,
	 * а вирішує доступ цільова рація своїми дозволами. Раніше тут стояло `return -3`, тож
	 * на відкритих каналах віддалене керування не працювало взагалі. */
	uint8_t keyId = rctlResolveTxKeyId();
	const uint8_t *key = (keyId != 0) ? dmr_aes_key_ptr(keyId) : NULL;

	/* Номер послідовності беремо з ПОСТІЙНОГО лічильника. Раніше він сіявся з
	 * ticksGetMillis(), тобто від моменту ввімкнення -- після перезавантаження номери
	 * починались наново, і разом зі збереженням лічильників на цілі це заблокувало б
	 * зв'язок назавжди. 0 = діапазон зарезервувати не вдалося -> не відправляємо. */
	uint32_t seq = dmrRctlNextTxSeq();

	if (seq == 0) { return -5; }

	dmr_rctl_msg_t m;
	m.cmd = cmd;
	m.issuerId = trxDMRID;
	m.seq = seq;
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

/* Надіслати команду керування У СТОКОВОМУ ФОРМАТІ TYT (Motorola CSBK, відкритим текстом).
 * Це шлях сумісності: заводська рація розуміє наш кадр напряму (формат реверснуто з ефіру,
 * див. RCTL_COMPAT.md). На відміну від власного PDU -- без AES і без вибору ключа: команди
 * керування стокова шле відкрито, а доступ вирішує цільова рація своїми дозволами.
 *
 * Повертає 0 = поставлено в чергу TX, -2 = дата-виклик уже активний, -4 = не зібралось.
 * ACK від цілі тут НЕ очікується: формат стокової ACK-відповіді ще не реалізовано в RX
 * (спершу треба захопити його з ефіру -- крок 2.5). Тож меню показує лише "надіслано". */
int dmrRctlStockSend(int cmd, uint32_t targetId)
{
	if (dmrDataTxActive()) { return -2; }
	if ((cmd < 0) || (cmd >= DMR_RCTL_STOCK_NUM_CMDS)) { return -4; }

	uint8_t q[(DMR_RCTL_STOCK_PREAMBLES + 1) * 13];
	int n = dmr_rctl_stock_build_tx((dmr_rctl_stock_cmd_t)cmd, trxDMRID, targetId, q);
	if (n <= 0) { return -4; }

	dmrDataTxLoad(q, (uint8_t)n);
	return 0;
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
static volatile uint8_t  s_rxPeerEnc;    /* 1 = був ENC-заголовок (розшифрувати); 0 = відкритий кадр */

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
			s_rxPeerEnc = s_rxHaveEnc;
			s_rxReady = 1;
			dmrRctlRxReset();
		}
	}
}

/* ==================== RX стокових команд (форк як ціль) ================== */
/* Стокові команди приходять одним CSBK (тип 3), не rate-1/2 -- тож окремий від
 * власного PDU шлях. ISR лише розбирає+фільтрує на нашу адресу й лишає "pending";
 * рішення (гейт дозволів) і дії -- у tick, поза ISR. */
static volatile uint8_t  s_stockPending;
static volatile uint8_t  s_stockCmd;
static volatile uint32_t s_stockSrc;
static volatile uint32_t s_stockDst;

/* Квитанція на НАШ Radio Check (ми -- командир). ISR лише фіксує факт, облік часу й
 * поколінь -- у tick, як і для команд. */
static volatile uint8_t  s_stockAckPending;
static volatile uint32_t s_stockAckFrom;

/* Діагностика шляху квитанції (розрізнити три різні причини хреста на екрані):
 *  s_csbkSeen -- скільки CSBK-бургстів узагалі дійшло сюди (0 => не чуємо ефір
 *                в потрібний момент -- питання повороту TX->RX або каналу);
 *  s_ackSeen  -- з них розібрано як квитанцію (CRC зійшовся);
 *  s_ackForUs -- з них адресовано саме нам (requester == trxDMRID).
 * Читається з ПК: rctl_capture.py --rxdiag. */
static volatile uint32_t s_csbkSeen;
static volatile uint32_t s_ackSeen;
static volatile uint32_t s_ackForUs;

/* Вікно спостереження після ВЛАСНОЇ передачі: чи чуємо хоч щось і через
 * скільки мс. Це й є відповідь на питання "рація глуха після свого TX чи ні". */
#define RCTL_WIN_MS   15000U   /* широко: щоб встигнути подати контрольний сигнал вручну */
static volatile uint8_t  s_winArmed;
static volatile uint32_t s_winT0;
static volatile uint32_t s_winTxEndMs;   /* скільки мс тривало завершення передачі */
static volatile uint32_t s_winAny;       /* усі переривання "прийнято дані" у вікні */
static volatile uint32_t s_winData;      /* з них із data-синхронізацією */
static volatile uint32_t s_winFirstMs;   /* затримка до першого data-бургста (0 = не було) */
static volatile uint32_t s_winFirstInfo; /* type | crcOk<<8 | txEnabled<<9 */
static volatile uint32_t s_intTotal;      /* УСІ переривання "прийнято дані" від обнулення -- перевірка самого хука */

void dmrRctlNoteOwnTxEnd(uint32_t txFinishMs)
{
	s_winT0 = ticksGetMillis();
	s_winTxEndMs = txFinishMs;
	s_winAny = 0;
	s_winData = 0;
	s_winFirstMs = 0;
	s_winFirstInfo = 0;
	s_winArmed = 1;
}

void dmrRctlNoteRxDataInt(int rxDataType, int rxSyncClass, int crcOk, int txEnabled)
{
	s_intTotal++;   /* безумовно: якщо це нуль після будь-якого DMR-сигналу -- зламаний сам вимір */

	if (!s_winArmed) { return; }

	uint32_t dt = (uint32_t)(ticksGetMillis() - s_winT0);
	if (dt > RCTL_WIN_MS) { s_winArmed = 0; return; }

	s_winAny++;
	if (rxSyncClass == 2)   /* SYNC_CLASS_DATA */
	{
		s_winData++;
		if (s_winFirstMs == 0)
		{
			s_winFirstMs = (dt == 0) ? 1 : dt;   /* 0 зарезервовано під "не було" */
			s_winFirstInfo = (uint32_t)(rxDataType & 0x0F) |
					 (crcOk ? 0x100U : 0U) | (txEnabled ? 0x200U : 0U);
		}
	}
}

/* Діагностика (тихе підтвердження прийому по USB, без напису на екрані): */
static volatile uint32_t s_stockSeen;                            /* упізнаних команд на нашу адресу (до гейта) */
static volatile uint32_t s_stockLastSrc;                         /* хто останній командував */
static volatile uint8_t  s_stockLastCmd;
static volatile uint32_t s_stockActed[DMR_RCTL_STOCK_NUM_CMDS];  /* упізнаних+ДОЗВОЛЕНИХ, по команді */

void dmrRctlStockRxBurst(const uint8_t *p12)
{
	dmr_rctl_stock_cmd_t cmd;
	uint32_t src, dst;

	s_csbkSeen++;

	/* Спершу -- квитанція на НАШ запит (форк як командир). Вона має той самий опкод,
	 * що й команда Check, і відрізняється лише керуючим байтом (0x80), тож
	 * dmr_rctl_stock_parse() її не впізнає -- перевіряємо окремо й раніше.
	 * Формат знято з ефіру, див. RCTL_COMPAT.md §5a. */
	{
		uint32_t requester = 0, responder = 0;
		if (dmr_rctl_stock_parse_ack(p12, &requester, &responder))
		{
			s_ackSeen++;
			/* Реагуємо лише на відповідь САМЕ на наш запит -- чужі квитанції в ефірі
			 * не мають вмикати нам "рація на зв'язку". */
			if (requester == trxDMRID)
			{
				s_ackForUs++;
				s_stockAckFrom = responder;
				s_stockAckPending = 1;
			}
			return;
		}
	}

	/* parse робить і перевірку CRC -- шум/чужі кадри сюди не пройдуть */
	if (!dmr_rctl_stock_parse(p12, &cmd, &src, &dst)) { return; }
	if (dst != trxDMRID) { return; }   /* не нам -- мовчки ігноруємо (RCTL індивідуальний) */

	s_stockCmd = (uint8_t)cmd;
	s_stockSrc = src;
	s_stockDst = dst;
	s_stockPending = 1;
}

void dmrRctlStockRxDiag(uint32_t out[16])
{
	out[0] = s_stockSeen;
	out[1] = s_stockLastSrc;
	out[2] = s_stockActed[DMR_RCTL_STOCK_CHECK];
	out[3] = s_stockActed[DMR_RCTL_STOCK_MONITOR];
	out[4] = s_stockActed[DMR_RCTL_STOCK_ENABLE];
	out[5] = s_stockActed[DMR_RCTL_STOCK_DISABLE];
	out[6] = (uint32_t)dmrRctlIsInhibited();   /* поточний стан блокування (переживає ребут) */
	out[7] = s_csbkSeen;
	out[8] = s_ackSeen;
	out[9] = s_ackForUs;
	out[10] = s_winAny;
	out[11] = s_winData;
	out[12] = s_winFirstMs;
	out[13] = s_winFirstInfo;
	out[14] = s_winTxEndMs;
	out[15] = s_intTotal;
}

void dmrRctlStockRxDiagReset(void)
{
	s_stockSeen = 0;
	s_stockLastSrc = 0;
	s_stockLastCmd = 0;
	for (int i = 0; i < DMR_RCTL_STOCK_NUM_CMDS; i++) { s_stockActed[i] = 0; }
	s_csbkSeen = 0;
	s_ackSeen = 0;
	s_ackForUs = 0;
	s_winAny = 0;
	s_winData = 0;
	s_winFirstMs = 0;
	s_winFirstInfo = 0;
	s_winTxEndMs = 0;
	s_intTotal = 0;
}

/* Обробити відкладену стокову команду (з tick, поза ISR). Поки лише лічимо -- це тихо
 * підтверджує, що прийом+гейт працюють на залізі. Самі дії (ACK/блокування/монітор)
 * додамо наступними інкрементами -- кожну окремо й із перевіркою на залізі. */
static void dmrRctlStockProcessPending(void)
{
	/* Квитанція на наш запит -- окремо від команд: вона нічого не виконує, лише оновлює
	 * стан "рація на зв'язку" для екрана Від. керування. */
	if (s_stockAckPending)
	{
		s_stockAckPending = 0;
		s_lastAckFromId = s_stockAckFrom;
		s_lastAckMillis = ticksGetMillis();
		s_haveAck = 1;
		s_ackGen++;
	}

	if (!s_stockPending) { return; }
	s_stockPending = 0;

	dmr_rctl_stock_cmd_t cmd = (dmr_rctl_stock_cmd_t)s_stockCmd;
	uint32_t dst = s_stockDst;
	uint32_t requester = s_stockSrc;   /* хто нас перевіряє -- йому й адресуємо квитанцію */

	s_stockSeen++;
	s_stockLastSrc = s_stockSrc;
	s_stockLastCmd = s_stockCmd;

	if (dmr_rctl_stock_should_act(cmd, dst, trxDMRID, dmrRctlAllowMask()))
	{
		if ((int)cmd >= 0 && (int)cmd < DMR_RCTL_STOCK_NUM_CMDS)
		{
			s_stockActed[cmd]++;
		}

		switch (cmd)
		{
			case DMR_RCTL_STOCK_CHECK:
				/* Квитанція «я на зв'язку»: командир (стокова/RT4D) показує нашу рацію
				 * онлайн. Формат знято з ефіру (RCTL_COMPAT.md §5a). Слот-точність не
				 * потрібна -- командир чекає відповідь ~1.3 с; наш data-TX стартує за
				 * ~100 мс. Не відповідаємо, якщо рація заблокована (stun): заблокована
				 * має виглядати «мертвою», тож і на Check вона мовчить -- як стокова. */
				if (!dmrRctlIsInhibited())
				{
					uint8_t q[DMR_RCTL_STOCK_ACK_REPEATS * 13];
					int n = dmr_rctl_stock_build_ack_tx(requester, trxDMRID, q);
					if (n > 0) { dmrDataTxLoad(q, (uint8_t)n); }
				}
				break;
			case DMR_RCTL_STOCK_DISABLE:
				dmrRctlSetInhibited(1);   /* заблокувати (переживає перезавантаження) */
				break;
			case DMR_RCTL_STOCK_ENABLE:
				dmrRctlSetInhibited(0);   /* розблокувати */
				break;
			default:
				break;                    /* Monitor(мік) -- наступний інкремент */
		}
	}
}

/* ============================ RX (основний цикл) ========================== */

void dmrRctlTick(void)
{
	dmrRctlStockProcessPending();   /* стокові команди -- незалежно від власного PDU нижче */

	if (!s_rxReady) { return; }

	uint8_t pdu[24];
	memcpy(pdu, s_rxPdu, 24);
	uint32_t src = s_rxPeerSrc;
	uint32_t dst = s_rxPeerDst;
	uint8_t  group = s_rxPeerGroup;
	uint8_t  keyId = s_rxPeerKeyId;
	uint8_t  enc = s_rxPeerEnc;
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

	if (!enc)
	{
		/* Відкритий кадр (ENC-заголовка не було): 16 байт PDU лежать як є. Магія "RC" +
		 * CRC32 вище відсіюють чужі дата-виклики, тож зайвого сюди не потрапить. */
		got = dmr_rctl_unpack(pdu, &msg) ? 1 : 0;
	}
	else
	{
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
	}
	if (!got) { return; }             /* не наш ключ / не RCTL-кадр -- тихо ігноруємо */
	if (msg.issuerId != src) { return; }   /* заголовок і зашифрований issuerId мають збігатись */

	/* Допуск і захист від replay (allowlist з кодплагу, пише лише CHIRP) -- fail-closed:
	 * вимкнено або видавця немає в списку -> команда ІГНОРУЄТЬСЯ. */
	/* Дозволи НА КОЖНУ КОМАНДУ окремо (як у Motorola/Hytera). Перевіряємо ДО gate_check:
	 * заборонена команда не має ані витрачати номер послідовності, ані спричиняти запис
	 * у флеш -- інакше чужий потік заборонених команд зношував би пам'ять.
	 *
	 * CHECK_ACK -- це відповідь на НАШ власний запит, а не дія над нами, тож маскою не
	 * гейтиться; головний перемикач до неї все одно застосується нижче, у gate_check. */
	if ((msg.cmd != DMR_RCTL_CMD_CHECK_ACK) && !dmrRctlCommandAllowed(msg.cmd)) { return; }

	if (!dmr_rctl_gate_check(dmrRctlGate(), msg.issuerId, msg.seq)) { return; }

	/* Зберігаємо оновлені лічильники ДО того, як виконати команду. Якщо зробити навпаки,
	 * знеструмлення між дією і записом лишає рівно те вікно, заради якого все й робилось:
	 * той самий записаний кадр пройшов би вдруге. */
	dmrRctlGatePersist();

	switch (msg.cmd)
	{
		case DMR_RCTL_CMD_CHECK_REQ:
			/* Авто-відповідь. Best-effort: якщо канал даних саме зайнятий -- відповідь
			 * пропускається, видавець може повторити запит. */
			/* Жодного сповіщення на ЦІЛЬОВІЙ рації: радіоперевірка навмисно ТИХА, як у
			 * Motorola/Hytera -- у цьому й сенс функції, перевірити наявність, не
			 * турбуючи оператора. Спроба (2026-09-04) показувати тут банер була
			 * відкинута: вона ламала саме ту властивість, заради якої функцію роблять.
			 * Захист від примусової передачі забезпечує anti-replay у dmr_rctl_cfg.c,
			 * а не напис на екрані. */
			break;

		case DMR_RCTL_CMD_CHECK_ACK:
		{
			s_lastAckFromId = msg.issuerId;
			s_lastAckMillis = ticksGetMillis();
			s_haveAck = 1;
			s_ackGen++;
			char note[40];
			snprintf(note, sizeof note, RCTL_NOTE_ACK_FMT, (unsigned long)msg.issuerId);
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

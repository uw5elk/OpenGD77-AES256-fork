/*
 * dmr_sms.c — on-radio encrypted DMR SMS service. See dmr_sms.h.
 *
 * Compiles to nothing unless built -DENABLE_AES -DENABLE_DMR_DATA (stock = byte-identical).
 *
 * Cipher + framing are the RE'd, on-air-validated stock-TYT scheme (a factory radio decrypts
 * our TX): AES-256-ECB over an IPv4/UDP/TMS plaintext (each 16-byte block independent, no IV),
 * carried in an Unconfirmed data PDU with a Motorola ENC extended header (ALG05 AES256). This
 * is the C port of tools/dmr_enc_sms.py (TX) and the inverse for RX, reusing crypto/dmr_aes.c.
 */
#include "functions/dmr_sms.h"

#if defined(ENABLE_DMR_DATA) && defined(ENABLE_AES)

#include "functions/dmr_data.h"
#include "functions/trx.h"
#include "functions/ticks.h"
#include "functions/codeplug.h"
#include "functions/settings.h"   /* currentChannelData (per-channel encrypt byte, voice logic) */
#include "crypto/dmr_aes.h"
#include "crypto/dmr_aes_hook.h"
#include "user_interface/menuSystem.h"   /* uiNotificationShow + NOTIFICATION_* */
#include "user_interface/uiGlobals.h"    /* currentRxGroupData + *_ALL_CALL_VALUE: адресний фільтр RX */
#include "hardware/HR-C6000.h"           /* PC_CALL_FLAG: тип виклику в старшому байті trxTalkGroupOrPcId */
#include "functions/sound.h"             /* soundSetMelody: audible RX alert */
#include <string.h>

/* ETSI slot data types as reported in HR-C6000 reg 0x51 [7:4]. */
#define DT_DATA_HEADER   6
#define DT_RATE12_DATA   7
#define DT_RATE34_DATA   8   /* rate-3/4 data: 18 інфо-байтів/burst (стокова шле SMS саме так) */
/* Burst slot-type bytes we write on TX (page 0x04 reg 0x50: type<<4). */
#define DTB_CSBK         0x30
#define DTB_DATA_HEADER  0x60
#define DTB_RATE12_DATA  0x70

#define UDP_SMS_PORT     0x0FA7          /* 4007, src==dst, stock TYT */

/* ============================ message store ============================== */
/* Variable-length packed store: the MSGS custom-data block holds a small header plus a
 * byte area of back-to-back entries, so a message uses only the space its text needs
 * (stock-like: many short messages OR a few long ones share the same fixed block). A CCM
 * working copy (DMR_AES_CCM -> no net-new main RAM) is the live store. Block stays 1352 B
 * (== the old fixed 24x56 array) so it drops into an existing codeplug slot.
 * Entry layout in data[]:  [0]=flags [1]=textLen [2..3]=seq(LE) [4..7]=peerId(LE) [8..]=text */
#define SMS_ENTRY_HDR  8
typedef struct
{
	char     magic[4];                 /* "MSGV" (v2 variable-length; old "MSGS" is ignored) */
	uint8_t  version;                  /* 2 */
	uint8_t  rsvd;
	uint16_t used;                     /* bytes in use in data[] */
	uint16_t nextSeq;                  /* next message ordering id */
	uint8_t  data[DMR_SMS_STORE_DATA];
} dmrSmsStore_t;

static dmrSmsStore_t   s_store   DMR_AES_CCM;
static uint8_t         s_loaded  DMR_AES_CCM;  /* CCM is not zeroed at boot -> guard init */
static dmrSmsMessage_t s_scratch;                /* copy-out buffer for dmrSmsGet (main RAM, not CCM) */

static void runtimeReset(void);

static int entry_size(int off) { return SMS_ENTRY_HDR + s_store.data[off + 1]; }
static uint16_t entry_seq(int off) { return (uint16_t)(s_store.data[off + 2] | (s_store.data[off + 3] << 8)); }
static int entry_matches(int off, int outgoing)
{
	int want = outgoing ? DMR_SMS_FLAG_OUTGOING : 0;
	return (s_store.data[off] & DMR_SMS_FLAG_OUTGOING) == want;
}

static void store_blank(void)
{
	memset(&s_store, 0, sizeof s_store);
	memcpy(s_store.magic, "MSGV", 4);
	s_store.version = 2;
	s_store.used = 0;
	s_store.nextSeq = 1;
}

/* Walk the packed entry chain: every entry must lie fully inside used[] with a sane
 * textLen, and the chain must tile data[] exactly up to used. Rejects a corrupted
 * flash block whose lengths would otherwise index garbage entry boundaries. */
static int store_chain_valid(void)
{
	int o = 0;
	while (o < (int)s_store.used)
	{
		if ((o + SMS_ENTRY_HDR > (int)s_store.used) ||
				(s_store.data[o + 1] > DMR_SMS_TEXT_MAX) ||
				(o + entry_size(o) > (int)s_store.used))
		{
			return 0;
		}
		o += entry_size(o);
	}
	return 1;
}

void dmrSmsInit(void)
{
	uint8_t *blk = (uint8_t *)&s_store;
	runtimeReset();     /* clear CCM runtime state (counters, flags) before first use */
	s_loaded = 1;
	dmrAesLoadKeys();   /* ensure the key store is populated for RX decrypt */
	// Обмежене читання. Довжина блока береться з ЗАГОЛОВКА У ФЛЕШІ (blockHeader.dataLength),
	// тож кодплаг із чужим або пошкодженим блоком MESSAGES міг наказати прочитати більше за
	// sizeof s_store і затерти сусідню пам'ять -- а поруч у тій самій секції CCM лежить
	// сховище AES-ключів. Обмежену версію ми вже мали й застосували до ключів, MSGC і RCTL,
	// але саме тут -- забули.
	if (codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_MESSAGES, blk, (int)sizeof s_store) &&
			(memcmp(s_store.magic, "MSGV", 4) == 0) && (s_store.version == 2) &&
			(s_store.used <= DMR_SMS_STORE_DATA) && store_chain_valid())
	{
		return;   /* valid variable-length store loaded from flash */
	}
	store_blank();      /* fresh, corrupted, or an old "MSGS" v1 block -> start empty */
}

static void store_ensure(void) { if (!s_loaded) { dmrSmsInit(); } }

static void store_save(void)
{
	dmrAesEnsureCustomDataRegion();   /* OpenGD77 magic must exist for the block chain */
	codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_MESSAGES,
			(uint8_t *)&s_store, (int)sizeof s_store);
}

/* Byte offset of the idx-th newest entry of a folder (newest = idx 0), or -1. Selection
 * scan (no large stack array): rank 0 = highest seq, each next rank = highest seq below it. */
static int off_for(int outgoing, int idx)
{
	uint32_t prevSeq = 0x10000;   /* above any 16-bit seq */
	int chosen = -1;
	for (int rank = 0; rank <= idx; rank++)
	{
		int bestOff = -1; uint32_t bestSeq = 0;
		for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
		{
			if (!entry_matches(o, outgoing)) { continue; }
			uint32_t sq = entry_seq(o);
			if (sq < prevSeq && (bestOff < 0 || sq > bestSeq)) { bestSeq = sq; bestOff = o; }
		}
		if (bestOff < 0) { return -1; }
		chosen = bestOff; prevSeq = bestSeq;
	}
	return chosen;
}

int dmrSmsCount(int outgoing)
{
	store_ensure();
	int n = 0;
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		if (entry_matches(o, outgoing)) { n++; }
	}
	return n;
}

const dmrSmsMessage_t *dmrSmsGet(int outgoing, int idx)
{
	store_ensure();
	int o = off_for(outgoing, idx);
	if (o < 0) { return NULL; }
	memset(&s_scratch, 0, sizeof s_scratch);
	s_scratch.flags   = (uint8_t)(s_store.data[o] | DMR_SMS_FLAG_USED);
	s_scratch.textLen = s_store.data[o + 1];
	s_scratch.seq     = entry_seq(o);
	s_scratch.peerId  = (uint32_t)s_store.data[o + 4] | ((uint32_t)s_store.data[o + 5] << 8) |
	                    ((uint32_t)s_store.data[o + 6] << 16) | ((uint32_t)s_store.data[o + 7] << 24);
	int tl = s_scratch.textLen; if (tl > DMR_SMS_TEXT_MAX) { tl = DMR_SMS_TEXT_MAX; }
	memcpy(s_scratch.text, &s_store.data[o + SMS_ENTRY_HDR], tl);
	return &s_scratch;
}

int dmrSmsUnreadCount(void)
{
	store_ensure();
	int n = 0;
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		if (((s_store.data[o] & DMR_SMS_FLAG_OUTGOING) == 0) && (s_store.data[o] & DMR_SMS_FLAG_UNREAD)) { n++; }
	}
	return n;
}

void dmrSmsMarkRead(int outgoing, int idx)
{
	store_ensure();
	int o = off_for(outgoing, idx);
	if (o >= 0 && (s_store.data[o] & DMR_SMS_FLAG_UNREAD))
	{
		s_store.data[o] &= (uint8_t)~DMR_SMS_FLAG_UNREAD;
		store_save();
	}
}

void dmrSmsMarkAllRead(void)
{
	store_ensure();
	int changed = 0;
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		if (((s_store.data[o] & DMR_SMS_FLAG_OUTGOING) == 0) && (s_store.data[o] & DMR_SMS_FLAG_UNREAD))
		{
			s_store.data[o] &= (uint8_t)~DMR_SMS_FLAG_UNREAD; changed = 1;
		}
	}
	if (changed) { store_save(); }
}

/* Remove the entry at byte offset o, compacting the block down. */
static void entry_remove(int o)
{
	int sz = entry_size(o);
	int tail = (int)s_store.used - (o + sz);
	if (tail > 0) { memmove(&s_store.data[o], &s_store.data[o + sz], (size_t)tail); }
	s_store.used = (uint16_t)(s_store.used - sz);
}

void dmrSmsDelete(int outgoing, int idx)
{
	store_ensure();
	int o = off_for(outgoing, idx);
	if (o >= 0) { entry_remove(o); store_save(); }
}

void dmrSmsDeleteAll(int outgoing)
{
	store_ensure();
	int changed = 0;
	int o = 0;
	while (o + SMS_ENTRY_HDR <= (int)s_store.used)
	{
		int match = (outgoing < 0) ||
				(((s_store.data[o] & DMR_SMS_FLAG_OUTGOING) != 0) == (outgoing != 0));
		if (match) { entry_remove(o); changed = 1; }   /* next entry shifted into o; don't advance */
		else { o += entry_size(o); }
	}
	if (changed) { store_save(); }
}

/* Renumber every entry's seq to 1..N preserving age order (oldest = 1). Called when the
 * 16-bit nextSeq wraps, so "newest = highest seq" ordering and lowest-seq eviction stay
 * correct across the wrap. Flag bit 0x80 is a transient "renumbered" marker (cleared
 * before returning, never persisted set). */
#define SMS_FLAG_TMP_MARK  0x80
static void seq_renumber(void)
{
	int count = 0;
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		s_store.data[o] &= (uint8_t)~SMS_FLAG_TMP_MARK;
		count++;
	}
	for (int newSeq = count; newSeq >= 1; newSeq--)
	{
		int bestOff = -1; uint16_t bestSq = 0;
		for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
		{
			if (s_store.data[o] & SMS_FLAG_TMP_MARK) { continue; }
			uint16_t sq = entry_seq(o);
			if (bestOff < 0 || sq >= bestSq) { bestSq = sq; bestOff = o; }
		}
		if (bestOff < 0) { break; }
		s_store.data[bestOff + 2] = (uint8_t)(newSeq & 0xFF);
		s_store.data[bestOff + 3] = (uint8_t)(newSeq >> 8);
		s_store.data[bestOff] |= SMS_FLAG_TMP_MARK;
	}
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		s_store.data[o] &= (uint8_t)~SMS_FLAG_TMP_MARK;
	}
	s_store.nextSeq = (uint16_t)(count + 1);
}

/* Insert a new message, evicting the globally-oldest (lowest seq) until it fits. */
static void store_add(uint8_t flags, uint32_t peerId, const char *text, int textLen)
{
	store_ensure();
	if (textLen > DMR_SMS_TEXT_MAX) { textLen = DMR_SMS_TEXT_MAX; }
	if (textLen < 0) { textLen = 0; }
	int need = SMS_ENTRY_HDR + textLen;
	if (need > DMR_SMS_STORE_DATA) { return; }   /* can't ever fit */

	while ((int)s_store.used + need > DMR_SMS_STORE_DATA)
	{
		int oldest = -1; uint16_t lo = 0xFFFF;
		for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
		{
			uint16_t sq = entry_seq(o);
			if (sq <= lo) { lo = sq; oldest = o; }
		}
		if (oldest < 0) { break; }
		entry_remove(oldest);
	}

	int o = s_store.used;
	if (s_store.nextSeq == 0) { seq_renumber(); }   /* 16-bit seq wrapped -> renumber by age */
	uint16_t seq = s_store.nextSeq++;
	s_store.data[o + 0] = (uint8_t)(DMR_SMS_FLAG_USED | flags);
	s_store.data[o + 1] = (uint8_t)textLen;
	s_store.data[o + 2] = (uint8_t)(seq & 0xFF);
	s_store.data[o + 3] = (uint8_t)(seq >> 8);
	s_store.data[o + 4] = (uint8_t)(peerId & 0xFF);
	s_store.data[o + 5] = (uint8_t)((peerId >> 8) & 0xFF);
	s_store.data[o + 6] = (uint8_t)((peerId >> 16) & 0xFF);
	s_store.data[o + 7] = (uint8_t)((peerId >> 24) & 0xFF);
	memcpy(&s_store.data[o + SMS_ENTRY_HDR], text, (size_t)textLen);
	s_store.used = (uint16_t)(o + need);
	store_save();
}

/* ============================ config (MSGC block) ======================= */
/* Read-only from the firmware's side; written by the CHIRP module. Layout is
 * shared byte-for-byte (opengd77_aes.py MsgConfig). */
#define MSGC_PRESET_LEN  48
typedef struct
{
	char     magic[4];                 /* "MSGC" */
	uint8_t  version;
	uint8_t  numPresets;
	uint8_t  defaultGroup;
	uint8_t  maxLen;                 /* CHIRP-set max compose length (0 = default 144) */
	uint32_t defaultDst;               /* little-endian on the wire == native */
	char     preset[DMR_SMS_NUM_PRESETS][MSGC_PRESET_LEN];
	uint8_t  smsEncrypt;             /* SMS-encrypt master gate: 0 = default (on), 1 = off/clear, 2 = on.
	                                  * Appended last so an old (shorter) MSGC block reads as 0 = default. */
} dmrSmsCfg_t;

static dmrSmsCfg_t s_cfg DMR_AES_CCM;
static uint8_t     s_cfgLoaded DMR_AES_CCM;

static void cfg_load(void)
{
	s_cfgLoaded = 1;
	memset(&s_cfg, 0, sizeof s_cfg);   /* default any field a shorter/absent block omits (smsEncrypt=0) */
	/* Bounded read: dataLength comes from flash — a corrupt/oversized block must not overrun s_cfg. */
	if (codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_MSG_CONFIG, (uint8_t *)&s_cfg, (int)sizeof s_cfg) &&
			(memcmp(s_cfg.magic, "MSGC", 4) == 0))
	{
		/* Force-terminate every preset row: dmrSmsPresetGet() returns these as C strings
		 * and the compose copy reads up to dmrSmsMaxLen() (144) chars, so an unterminated
		 * 48-byte row written by CHIRP must not run into the next row / off the struct. */
		for (int i = 0; i < DMR_SMS_NUM_PRESETS; i++) { s_cfg.preset[i][MSGC_PRESET_LEN - 1] = 0; }
		return;
	}
	memset(&s_cfg, 0, sizeof s_cfg);
}

static void cfg_ensure(void) { if (!s_cfgLoaded) { cfg_load(); } }

int dmrSmsPresetCount(void)
{
	cfg_ensure();
	int n = 0;
	for (int i = 0; i < DMR_SMS_NUM_PRESETS; i++)
	{
		if (s_cfg.preset[i][0] != 0) { n++; }
	}
	return n;
}

const char *dmrSmsPresetGet(int idx)
{
	cfg_ensure();
	if (idx < 0 || idx >= DMR_SMS_NUM_PRESETS || s_cfg.preset[idx][0] == 0) { return 0; }
	return s_cfg.preset[idx];
}

void dmrSmsDefaultRecipient(uint32_t *dst, int *group)
{
	cfg_ensure();
	if (dst)   { *dst = s_cfg.defaultDst & 0x00FFFFFF; }
	if (group) { *group = s_cfg.defaultGroup ? 1 : 0; }
}

int dmrSmsMaxLen(void)
{
	cfg_ensure();
	int m = s_cfg.maxLen;
	if (m <= 0 || m > DMR_SMS_TEXT_MAX) { m = DMR_SMS_TEXT_MAX; }
	return m;
}

/* SMS-encrypt master gate (CHIRP "Encrypt SMS"): 1 = encrypt-per-channel-like-voice,
 * 0 = always cleartext. Default (unset MSGC byte) = 1 to preserve the encrypted behaviour. */
int dmrSmsEncryptEnabled(void)
{
	cfg_ensure();
	return (s_cfg.smsEncrypt == 1) ? 0 : 1;   /* 1 = force clear; 0(default)/2 = encrypt */
}

/* AES TX key for the current channel — a mirror of hrc6000ResolveAesTxKeyId (HR-C6000.c) so
 * SMS encryption follows the exact same per-channel logic as voice: the global TX selector,
 * overridden by the channel encrypt byte (0xFF -> clear, 1..15 -> key slot, 0 -> inherit).
 * Byte 41 is shared with optional-DMR-ID; на такому каналі слот лежить у _UNUSED_2 з міткою,
 * тож читаємо через codeplugChannelGetAesKeySlot(), щоб SMS шифрувались і там теж. */
static uint8_t smsResolveTxKeyId(void)
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

/* ============================ checksums / CRCs =========================== */
static uint16_t ip_cksum(const uint8_t *b, int len)
{
	uint32_t s = 0;
	for (int i = 0; i < len; i += 2)
	{
		s += ((uint32_t)b[i] << 8) | ((i + 1 < len) ? b[i + 1] : 0);
	}
	while (s >> 16) { s = (s & 0xFFFF) + (s >> 16); }
	return (uint16_t)(~s & 0xFFFF);
}

static uint16_t crc16d(const uint8_t *data, int len)   /* CCITT, poly 0x1021, ^0xFFFF */
{
	uint16_t crc = 0;
	for (int i = 0; i < len; i++)
	{
		for (int k = 7; k >= 0; k--)
		{
			int bit = (data[i] >> k) & 1;
			if (((crc >> 15) & 1) ^ bit) { crc = (uint16_t)((crc << 1) ^ 0x1021); }
			else                         { crc = (uint16_t)(crc << 1); }
		}
	}
	return (uint16_t)(crc ^ 0xFFFF);
}

static void hdr_crc(const uint8_t *h, int len, uint16_t mask, uint8_t out2[2])
{
	uint16_t v = (uint16_t)(crc16d(h, len) ^ mask);
	out2[0] = (uint8_t)(v >> 8); out2[1] = (uint8_t)(v & 0xFF);
}

/* DMR data-PDU CRC32: byte-pair swap, poly 0x04C11DB7, over (len*8-32) bits. */
static uint32_t crc32_dmr(const uint8_t *pdu, int len)
{
	uint32_t crc = 0;
	int nbits = len * 8 - 32;
	int bitno = 0;
	for (int i = 0; i + 1 < len; i += 2)
	{
		for (int pass = 0; pass < 2; pass++)
		{
			uint8_t byte = (pass == 0) ? pdu[i + 1] : pdu[i];
			for (int k = 7; k >= 0; k--)
			{
				if (bitno >= nbits) { goto done; }
				int bit = (byte >> k) & 1;
				if (((crc >> 31) & 1) ^ bit) { crc = (crc << 1) ^ 0x04C11DB7; }
				else                         { crc = (crc << 1); }
				bitno++;
			}
		}
	}
done:
	/* byte-reverse to wire order */
	return ((crc & 0xFF) << 24) | ((crc & 0xFF00) << 8) | ((crc >> 8) & 0xFF00) | ((crc >> 24) & 0xFF);
}

/* ============================ plaintext builder ========================== */
/* Build the IPv4/UDP/TMS plaintext into out (>=160 B). Returns length. */
static int build_plaintext(const char *text, int tlen, uint32_t src, uint32_t dst,
                           uint8_t seq, uint16_t ipid, uint8_t *out)
{
	uint8_t tms[16 + 2 * DMR_SMS_TEXT_MAX];
	int L = tlen * 2;            /* UTF-16LE byte count */
	int ti = 0;
	tms[ti++] = (uint8_t)((8 + L) >> 8); tms[ti++] = (uint8_t)(8 + L);   /* 2-byte TMS length */
	tms[ti++] = 0xA0; tms[ti++] = 0x00; tms[ti++] = seq; tms[ti++] = 0x04;
	tms[ti++] = 0x0D; tms[ti++] = 0x00;   /* fixed CRLF header (stock uses 0d/0a here, NOT L+3/L) */
	tms[ti++] = 0x0A; tms[ti++] = 0x00;
	for (int i = 0; i < tlen; i++) { tms[ti++] = (uint8_t)text[i]; tms[ti++] = 0x00; }

	int udpLen = 8 + ti;
	/* MotoTRBO CAI IP = prefix.<id[23:16]>.<id[15:8]>.<id[7:0]> — the FULL 24-bit DMR ID/TG.
	 * (Byte 1 was hardcoded 0x00, truncating to 16 bits; correct only for ids <= 65535, wrong
	 * for real 7-digit DMR ids.) src = individual radio (12.x.x.x), group dst = 225.x.x.x. */
	uint8_t srcIp[4] = { 0x0C, (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src };
	uint8_t dstIp[4] = { 0xE1, (uint8_t)(dst >> 16), (uint8_t)(dst >> 8), (uint8_t)dst };

	uint8_t udp[8 + 16 + 2 * DMR_SMS_TEXT_MAX];
	udp[0] = UDP_SMS_PORT >> 8; udp[1] = UDP_SMS_PORT & 0xFF;
	udp[2] = UDP_SMS_PORT >> 8; udp[3] = UDP_SMS_PORT & 0xFF;
	udp[4] = (uint8_t)(udpLen >> 8); udp[5] = (uint8_t)udpLen; udp[6] = 0; udp[7] = 0;
	memcpy(udp + 8, tms, ti);

	/* UDP checksum (pseudo-header) */
	{
		uint8_t pseudo[12 + 8 + 16 + 2 * DMR_SMS_TEXT_MAX];
		int p = 0;
		memcpy(pseudo + p, srcIp, 4); p += 4;
		memcpy(pseudo + p, dstIp, 4); p += 4;
		pseudo[p++] = 0; pseudo[p++] = 0x11;
		pseudo[p++] = (uint8_t)(udpLen >> 8); pseudo[p++] = (uint8_t)udpLen;
		memcpy(pseudo + p, udp, udpLen); p += udpLen;
		if (p & 1) { pseudo[p++] = 0; }
		uint16_t uc = ip_cksum(pseudo, p);
		if (uc == 0) { uc = 0xFFFF; }
		udp[6] = (uint8_t)(uc >> 8); udp[7] = (uint8_t)uc;
	}

	int totLen = 20 + udpLen;
	uint8_t *ip = out;
	ip[0] = 0x45; ip[1] = 0x00; ip[2] = (uint8_t)(totLen >> 8); ip[3] = (uint8_t)totLen;
	ip[4] = (uint8_t)(ipid >> 8); ip[5] = (uint8_t)ipid; ip[6] = 0; ip[7] = 0;
	ip[8] = 0x40; ip[9] = 0x11; ip[10] = 0; ip[11] = 0;
	memcpy(ip + 12, srcIp, 4); memcpy(ip + 16, dstIp, 4);
	uint16_t ic = ip_cksum(ip, 20);
	ip[10] = (uint8_t)(ic >> 8); ip[11] = (uint8_t)ic;
	memcpy(out + 20, udp, udpLen);
	return totLen;
}

/* ============================ TX ======================================== */
/* Burst queue for the data-TX harness: count*(1 type byte + 12 payload). */
static int append_burst(uint8_t *q, int n, uint8_t typeByte, const uint8_t *p12)
{
	q[n * 13 + 0] = typeByte;
	memcpy(q + n * 13 + 1, p12, 12);
	return n + 1;
}

/* DIAGNOSTIC (USB 0x97): lets the host invoke the REAL dmrSmsSend() menu path directly,
 * optionally skipping the Sent-folder flash write, to A/B-test TX behaviour without pushing
 * radio buttons. This was used to hunt bug #3 (see below) -- the store_add() write turned out
 * to be innocent, but the harness is kept as a general on-demand SMS-TX diagnostic. Plain .bss;
 * the physical menu path never sets it. */
static uint8_t s_diagSkipStore;
void dmrSmsDiagSetSkipStore(int skip) { s_diagSkipStore = (skip != 0) ? 1 : 0; }

/* ---- deferred Sent-folder persist (defensive hygiene, not the bug #3 fix) ---
 * store_add() does a BLOCKING SPI-flash sector erase+write (~100s of ms). Investigating bug #3
 * (see s_txMsgCounter below for the actual root cause) an A/B test initially looked like this
 * write disrupted the on-air burst timing -- but a follow-up test with the write fully skipped
 * STILL failed after the first send, which exonerated it. Kept anyway as good practice: doing a
 * blocking flash op anywhere in the TX-keying window is fragile regardless, so dmrSmsSend stashes
 * the outgoing message and dmrSmsRxTick() flushes it to flash only once the data call has fully
 * un-keyed (dmrDataTxActive()==0 && !trxIsTransmitting). */
static uint8_t  s_pendSent;                    /* 1 = a Sent-folder write is queued */
static uint8_t  s_pendFlags;
static uint32_t s_pendPeer;
static int      s_pendTextLen;
static char     s_pendText[DMR_SMS_TEXT_MAX + 1];

/* Per-message IP-ID / TMS-sequence counter (bug #3 root cause). A stock TYT tracks these like a
 * real SMS client and DROPS a message that repeats the previous IP-ID + TMS-seq as a duplicate
 * retransmission. The firmware used to hardcode ipid=0x0001, seq=0x90 on EVERY send, so only the
 * first of a run reached the stock inbox while every later (byte-identical) send was silently
 * dropped -- HW root-caused: raw 0x91 sends with per-send-varied ipid/seq always landed; identical
 * menu sends did not, though the frame was byte-perfect on air. Seed from the boot tick so the
 * sequence doesn't restart at the same value each power-up and collide with a stock that still
 * remembers the previous session's traffic. */
static uint16_t s_txMsgCounter;
static uint8_t  s_txMsgSeeded;

/* ---- Квитанція на вхідне CONFIRMED SMS (link-layer ACK, реверс #11, ефір BBD_0005) --------
 * Стокова TYT / RT4D шлють текст CONFIRMED data-заголовком (DPF=3, біт A=1 -> «прошу
 * підтвердження»). Отримавши всі блоки (CRC32 PDU сходиться), приймач має відповісти
 * Response data header (DPF=1): відправник тоді показує «доставлено», інакше сигналить
 * помилку й ретрансмітить -- рівно як було з монітором до реверсу його квитанції.
 *
 * Знято з ефіру (BBD_0005, стокова 0x26EA3D квитує RT4D 0x26EA1B):
 *   01 40 <кому:3=початк.відправник> <від кого:3=ми> 00 08 <CRC16^0xCCCC>
 *   o0=0x01: G/I=0 (індивід.), A=0, DPF=1 (Response)
 *   o1=0x40: SAP=4 (IP based packet data) -- той самий SAP, що й у нашому тексті
 *   o8=0x00: Blocks-to-Follow = 0 (гола квитанція, за нею блоків немає)
 *   o9=0x08: Class=00 (ACK), Type=001, Status=000
 * ОБИДВА байти -- КОНСТАНТИ, нічого туди не підставляти. Перша реалізація (2026-09-12)
 * трактувала їх навпаки (o8=код, o9=луна к-сті блоків) і слала в полі `00 04` -- тобто
 * Status=4 замість ACK. Квитанція йшла в ефір (лічильники: вефір=3), але стокова її не
 * приймала й ретрансмітила повідомлення 3 рази. Не «винаходити» ці байти: шлемо рівно те,
 * що знято з ефіру.
 * CRC заголовка -- той самий, що для всіх data-заголовків (crc16d ^ 0xCCCC).
 *
 * Ключуємо з невеликою затримкою після завершення прийому (стокова відповідала ~80 мс по
 * кінці), і лише коли канал звільнився (!dmrDataTxActive && !trxIsTransmitting) -- щоб не
 * зіткнутися з хвостом передачі відправника. */
static uint8_t  s_ackPending;   /* 1 = винні квитанцію */
static uint32_t s_ackTo;        /* кому (початковий відправник) */
static uint32_t s_ackAtMs;      /* найраніший момент ключування */
static uint32_t s_ackDeadlineMs;/* після цього квитанція протухла (канал так і не звільнився) */

/* Час останнього ПРИЙНЯТОГО data-burst (будь-якого типу) -- проксі «канал зайнятий».
 * Заповнюється в dmrSmsRxDiagBurst(), який HR-C6000 кличе на КОЖЕН data-sync burst.
 * Квитанцію ключуємо лише після паузи: інакше влучаємо у хвіст/термінатор відправника,
 * він у цей момент ще передає (отже глухий) -- і замість «доставлено» шле все наново. */
static volatile uint32_t s_lastRxBurstMs;
#define SMS_ACK_KEY_DELAY_MS 15  /* замість типових 100 мс: стокова слухає вузьке вікно */
#define SMS_ACK_MAX_WAIT_MS 2500 /* не тягнути квитанцію вічно, якщо канал не звільняється */
#define SMS_ACK_MAX_REPEATS  6   /* межа буфера черги */
#define SMS_ACK_MAX_PREAMBLES 16 /* стільки ж, скільки шле стокова перед повідомленням */

/* ПІДБІРНІ ПАРАМЕТРИ (USB 0xB2) -- щоб не перебирати прошивками по одній.
 * Типово = рівно як у стокової, знято з ефіру (BBD_0005): ОДИН burst через ~80 мс після
 * того, як відправник домовк. Ми ж слали 3 повтори через 30 мс -- і форма, і таймінг були
 * «правильні», але саме ЦИМ від стокової й відрізнялись: три Response header підряд вона
 * цілком могла зарахувати як дублікати/сміття, а 30 мс -- влучити в її перехід
 * передача->прийом. */
static uint8_t  s_ackRepeats = 1;      /* скільки разів повторити заголовок (1..6) */
static uint16_t s_ackTargetMs = 80;    /* цільова пауза після тиші в каналі, мс */
/* CSBK-преамбули перед квитанцією. ТИПОВО 6 -- і ось чому.
 * Еталоном я довго вважав те, що стокова НАДСИЛАЄ (її квитанція йде без преамбул). Але
 * правильний еталон -- те, що вона успішно ПРИЙМАЄ, і такий приклад є власний: наше
 * звичайне SMS стокова приймає, а воно йде з 6 преамбулами (dmrSmsSend). Схоже, її приймач
 * просто не «заводить» data-виклик без преамбули; RT4D виявився поблажливішим, тому її
 * власна безпреамбульна квитанція йому зайшла. Коли преамбули були в нас (rev<=4), тоді
 * стояв хибний код 00 04 -- тож поєднання «преамбули + правильний код» не перевірялось. */
static uint8_t  s_ackPreambles = 6;
static uint8_t  s_txAskAck = 1;        /* виставляти біт A на власних повідомленнях */

/* Вхідна квитанція НА НАШЕ повідомлення -> позначка «доставлено» у теці «Надіслані». */
static volatile uint8_t  s_gotAck DMR_AES_CCM;      /* прийшла квитанція, ще не оброблена */
static volatile uint32_t s_gotAckFrom DMR_AES_CCM;  /* від кого (кому ми слали) */
static volatile uint32_t s_ackRcvdCount;            /* діагностика: скільки квитанцій прийняли */

void dmrSmsAckSetAskAck(int on) { s_txAskAck = (on != 0) ? 1 : 0; }

void dmrSmsAckSetTuning(uint8_t repeats, uint16_t delayMs, uint8_t preambles)
{
	if (repeats < 1) { repeats = 1; }
	if (repeats > SMS_ACK_MAX_REPEATS) { repeats = SMS_ACK_MAX_REPEATS; }
	if (delayMs > 2000) { delayMs = 2000; }
	if (preambles > SMS_ACK_MAX_PREAMBLES) { preambles = SMS_ACK_MAX_PREAMBLES; }
	s_ackRepeats = repeats;
	s_ackTargetMs = delayMs;
	s_ackPreambles = preambles;
}

/* Ревізія формату/подачі квитанції -- видно в діагностиці (sms_diag.py), щоб не гадати,
 * яка саме прошивка залита:
 *   1 = перша реалізація: 2 преамбули + заголовок, у o9 підставлявся лічильник блоків (00 04);
 *   2 = ключування по тиші в каналі + лічильники;
 *   3 = квитуємо будь-який CONFIRMED (без вимоги біта A);
 *   4 = ВИПРАВЛЕНО o8/o9 = 00 08 (ACK) -- до цього стокова законно не приймала;
 *   5 = подача як у RCTL: 3 повтори заголовка, без преамбул; гейт тиші 40 мс;
 *   6 = ТАЙМІНГ: квитанція йшла НЕГАЙНО по CRC32 -- але це ще до термінатора відправника,
 *       тобто в ефір лізли, поки він передає (глухий). РАНО;
 *   7 = вікно з обох боків: чекаємо тиші 30 мс + ключування 15 мс, блокуючий запис у флеш
 *       відкладено. Таймінг і форма стали правильні -- стокова ВСЕ ОДНО пише помилку;
 *   8 = ОДИН burst через ~80 мс (як шле стокова) + підбір по USB. Жодне зі значень сітки
 *       (1/60..150, 2/100) стокову не влаштувало -> подача ні до чого;
 *   9 = перевертаємо еталон: копіюємо не те, що стокова НАДСИЛАЄ, а те, що вона успішно
 *       ПРИЙМАЄ -- наше власне SMS із 6 CSBK-преамбулами. Преамбули тепер теж підбірні. */
#define SMS_ACK_FORMAT_REV  9

/* Лічильники для польової діагностики (USB 0x93, хвіст відповіді). Саме вони мають сказати,
 * де рветься ланцюг: чи бачили ми взагалі CONFIRMED-заголовок із проханням квитанції,
 * чи поставили її в чергу, чи реально віддали в ефір. s_ackLastHdr0/1 -- перші два байти
 * останнього вхідного data-заголовка (тобто що НАСПРАВДІ шле стокова). */
static volatile uint32_t s_ackSeen;      /* заголовків CONFIRMED + біт A */
static volatile uint32_t s_ackQueued;    /* разів поставили квитанцію в чергу */
static volatile uint32_t s_ackSent;      /* разів реально віддали в ефір (dmrDataTxLoad) */
static volatile uint32_t s_ackStale;     /* разів кинули, бо канал не звільнився */
static volatile uint8_t  s_ackLastHdr0;  /* p[0] останнього вхідного data-заголовка */
static volatile uint8_t  s_ackLastHdr1;  /* p[1] -- SAP/блоки */
static volatile uint8_t  s_ackLastGroup; /* останнє повідомлення було груповим? */
static volatile uint8_t  s_ackLastForUs; /* останнє повідомлення адресоване нам? */
static volatile uint32_t s_ackLastDelayMs;/* мс від ост. прийнятого burst-а до віддачі квитанції */

/* Flush the deferred Sent-folder entry if one is queued and the radio has finished transmitting.
 * Called from the main loop (dmrSmsRxTick). Safe to call every tick; a no-op when idle. */
/* Відкладений запис ВХІДНОГО у теку: те саме, що s_pendSent для «Надісланих», але для
 * прийнятого повідомлення, коли ми щойно віддали квитанцію в ефір -- блокуючий флеш не має
 * потрапити у вікно, поки квитанція летить. */
static uint8_t  s_pendInbox;
static uint8_t  s_pendInboxFlags;
static uint32_t s_pendInboxPeer;
static int      s_pendInboxLen;
static char     s_pendInboxText[DMR_SMS_TEXT_MAX + 1];

static void dmrSmsTxPersistTick(void)
{
	if (s_pendSent && !dmrDataTxActive() && !trxIsTransmitting)
	{
		store_add(s_pendFlags, s_pendPeer, s_pendText, s_pendTextLen);
		s_pendSent = 0;
	}
	/* Вхідне пишемо у флеш лише коли квитанція ВЖЕ пішла (s_ackPending знято) і передача
	 * завершилась. Інакше блокуючий erase+write на сотні мс з'їдає те саме вікно, заради
	 * якого ми чекаємо тиші -- рівно те, що зламало rev 5. */
	if (s_pendInbox && !s_ackPending && !dmrDataTxActive() && !trxIsTransmitting)
	{
		store_add(s_pendInboxFlags, s_pendInboxPeer, s_pendInboxText, s_pendInboxLen);
		s_pendInbox = 0;
	}
}

/* Поставити квитанцію в чергу (викликається з dmrSmsRxTick, коли прийнято адресоване нам
 * CONFIRMED повідомлення з проханням підтвердити). Це РЕЗЕРВНИЙ шлях -- коли негайно
 * віддати не вийшло (ми самі щось передавали). Основний шлях -- buildAndLoadSmsAck() одразу. */
static void buildAndLoadSmsAck(uint32_t dst);
static void queueSmsAck(uint32_t to)
{
	uint32_t now = ticksGetMillis();
	s_ackTo = to;
	s_ackAtMs = now;                    /* далі чекаємо не таймер, а ТИШУ в каналі */
	s_ackDeadlineMs = now + SMS_ACK_MAX_WAIT_MS;
	s_ackPending = 1;
	s_ackQueued++;
}

/* Резервний шлях: віддати відкладену квитанцію, коли канал звільниться.
 * Черга = Response data header, ПОВТОРЕНИЙ SMS_ACK_REPEATS разів, БЕЗ CSBK-преамбул.
 * Саме так влаштована квитанція RCTL (dmr_rctl_stock_build_ack_tx: 3 повтори, без преамбул),
 * а вона з цією ж стоковою перевірена на залізі. Команди RCTL шлються з 16 преамбулами,
 * квитанції -- ні: відповідь має початися ЯКНАЙШВИДШЕ, поки відправник ще слухає.
 * Попередня версія слала 2 преамбули + заголовок, тобто сам заголовок виходив в ефір на
 * ~120 мс пізніше; в ефірному зразку (BBD_0005) преамбул перед квитанцією теж не знайшлось. */
static void dmrSmsAckTick(void)
{
	if (!s_ackPending) { return; }

	uint32_t now = ticksGetMillis();
	if ((int32_t)(now - s_ackDeadlineMs) >= 0)
	{
		/* Канал так і не звільнився (відправник молотить ретрансміти впритул) -- кидаємо,
		 * інакше квитанція вилетить у зовсім інший момент і тільки заважатиме. */
		s_ackPending = 0;
		s_ackStale++;
		return;
	}
	if (dmrDataTxActive() || trxIsTransmitting) { return; }        /* ми самі передаємо */
	/* Головна умова: відправник має ЗАМОВКНУТИ. Поки в каналі ідуть data-burst-и (хвіст
	 * повідомлення, термінатор), він передає і нас не почує. */
	{
		uint32_t wait = (s_ackTargetMs > SMS_ACK_KEY_DELAY_MS)
				? (uint32_t)(s_ackTargetMs - SMS_ACK_KEY_DELAY_MS) : 1u;
		if ((uint32_t)(now - s_lastRxBurstMs) < wait) { return; }
	}
	/* На slotState НЕ гейтуємо навмисно: після прийому він тримається в RX-стані до
	 * END_TICK_TIMEOUT тиші, тож чекання на IDLE могло б з'їсти весь дедлайн і квитанція б
	 * не пішла ніколи. Детерміноване ключування все одно робить сам dmrDataTxLoad()
	 * (він кличе HRC6000ForceDMRIdleForTx()). */

	/* Самовимір: скільки минуло від ОСТАННЬОГО прийнятого burst-а відправника до моменту,
	 * коли ми віддали квитанцію. Еталон -- ~80 мс у стокової; 366 мс (rev 5) було пізно.
	 * Видно в sms_diag.py, тож наступну перевірку таймінгу можна робити без HackRF. */
	s_ackLastDelayMs = now - s_lastRxBurstMs;
	buildAndLoadSmsAck(s_ackTo);
}

/* Зібрати й НЕГАЙНО віддати квитанцію в ефір. Виділено окремо, бо головний шлях -- саме
 * негайний: чекати тіка чи блокуючого запису у флеш не можна, вікно відправника вузьке. */
static void buildAndLoadSmsAck(uint32_t dst)
{
	uint32_t src = trxDMRID;        /* від кого: ми */
	uint8_t  h[10];
	h[0] = 0x01; h[1] = 0x40;                          /* Response, SAP=4 IP */
	h[2] = (uint8_t)(dst >> 16); h[3] = (uint8_t)(dst >> 8); h[4] = (uint8_t)dst;
	h[5] = (uint8_t)(src >> 16); h[6] = (uint8_t)(src >> 8); h[7] = (uint8_t)src;
	h[8] = 0x00;                                       /* Blocks-to-Follow = 0 (гола квитанція) */
	h[9] = 0x08;                                       /* Class=00 ACK, Type=001, Status=000 */
	uint8_t p12[12]; memcpy(p12, h, 10); hdr_crc(h, 10, 0xCCCC, p12 + 10);

	static uint8_t q[(SMS_ACK_MAX_PREAMBLES + SMS_ACK_MAX_REPEATS) * 13];
	int n = 0;
	/* CSBK-преамбули (як перед звичайним повідомленням) -- «заводять» data-виклик у приймача. */
	for (int i = 0; i < (int)s_ackPreambles; i++)
	{
		uint8_t body[10];
		body[0] = 0xBD; body[1] = 0x00; body[2] = 0x80;   /* індивідуальна data-преамбула */
		body[3] = (uint8_t)(((int)s_ackPreambles - 1 - i) + (int)s_ackRepeats);
		body[4] = (uint8_t)(dst >> 16); body[5] = (uint8_t)(dst >> 8); body[6] = (uint8_t)dst;
		body[7] = (uint8_t)(src >> 16); body[8] = (uint8_t)(src >> 8); body[9] = (uint8_t)src;
		uint8_t pb[12]; memcpy(pb, body, 10); hdr_crc(body, 10, 0xA5A5, pb + 10);
		n = append_burst(q, n, DTB_CSBK, pb);
	}
	for (int i = 0; i < (int)s_ackRepeats; i++)
	{
		n = append_burst(q, n, DTB_DATA_HEADER, p12);
	}
	/* Коротка затримка ключування замість типових 100 мс: відправник слухає вузьке вікно. */
	dmrDataTxLoadDelayed(q, (uint8_t)n, SMS_ACK_KEY_DELAY_MS);
	s_ackPending = 0;
	s_ackSent++;
}

int dmrSmsSend(const char *text, uint32_t dst, int group, uint8_t keyId)
{
	store_ensure();
	if (text == NULL) { return -1; }
	int tlen = (int)strlen(text);
	if (tlen == 0) { return -1; }
	if (tlen > DMR_SMS_TEXT_MAX) { tlen = DMR_SMS_TEXT_MAX; }
	if (dmrDataTxActive()) { return -2; }   /* a data call is already keyed */

	/* Decide encrypt-or-clear, following the VOICE key-selection logic gated by the CHIRP
	 * "Encrypt SMS" master switch: gate OFF -> always cleartext (even on an encrypted channel);
	 * ON -> the per-channel encrypt byte exactly like voice (0xFF or no loaded key -> clear,
	 * 1..15 -> that key slot, 0 -> the global TX selector). An explicit caller keyId (CPS/bench)
	 * overrides the gate. When encrypting, the ENC ext header signals the key id to the receiver. */
	if (keyId == 0 && dmrSmsEncryptEnabled()) { keyId = smsResolveTxKeyId(); }
	const uint8_t *key = (keyId != 0) ? dmr_aes_key_ptr(keyId) : NULL;
	int encrypt = (key != NULL);             /* resolved to a loaded key -> encrypt; else cleartext */

	uint32_t src = trxDMRID;

	/* 1) plaintext. When encrypting, ECB-encrypt the WHOLE 16-byte blocks only; the trailing
	 *    partial block (< 16 B) stays CLEAR, exactly like a stock TYT. Cleartext SMS skips the
	 *    encryption (same IPv4/UDP/TMS structure, and no ENC header emitted below). Do NOT pad. */
	uint8_t pt[360];   /* IPv4(20)+UDP(8)+TMS(10)+2*144 = 326 B max */
	/* Advance the per-message counter and derive a unique IP-ID + TMS-seq so consecutive sends
	 * aren't dropped by the stock radio as duplicate retransmissions (bug #3). The TMS byte keeps
	 * the 0x9x "text message" high nibble (0x00 there makes a stock TYT read the text 4 bytes early
	 * and prepend the length field as a stray char) and varies the low nibble; the 16-bit IP-ID
	 * gives full per-datagram uniqueness (and changes the IP/UDP checksums + data CRC32 too). */
	if (!s_txMsgSeeded) { s_txMsgCounter = (uint16_t)ticksGetMillis(); s_txMsgSeeded = 1; }
	s_txMsgCounter++;
	uint16_t ipid = s_txMsgCounter;
	uint8_t  tmsSeq = (uint8_t)(0x90 | (s_txMsgCounter & 0x0F));
	int ptLen = build_plaintext(text, tlen, src, dst, tmsSeq, ipid, pt);
	if (encrypt) { for (int i = 0; i + 16 <= ptLen; i += 16) { aes256_ecb_encrypt(key, pt + i); } }
	int ctLen = ptLen;                       /* ct = whole enc blocks + clear tail (or all clear) */

	/* 2) pdu = ct + pad(poc) + crc32, padded so (ct+4) fills whole 12-byte blocks */
	int totalData = (((ctLen + 4) + 11) / 12) * 12;
	int poc = totalData - ctLen - 4;
	uint8_t pdu[384];
	if (totalData > (int)sizeof pdu) { return -4; }
	memcpy(pdu, pt, ctLen);
	memset(pdu + ctLen, 0, poc);
	uint32_t crc = crc32_dmr(pdu, totalData) ;  /* placeholder bytes already zero */
	pdu[totalData - 4] = (uint8_t)(crc >> 24); pdu[totalData - 3] = (uint8_t)(crc >> 16);
	pdu[totalData - 2] = (uint8_t)(crc >> 8);  pdu[totalData - 1] = (uint8_t)crc;
	int nDataBlocks = totalData / 12;
	int nblocks = (encrypt ? 1 : 0) + nDataBlocks;  /* +1 ENC ext header block only when encrypting */

	/* 3) build the burst queue: CSBK preamble + 2 headers + rate-1/2 blocks */
	static uint8_t q[DMR_DATA_MAX_BURSTS * 13];
	int n = 0;
	int preamble = 6;
	int tail = (encrypt ? 2 : 1) + nDataBlocks;  /* headers (Unconfirmed [+ ENC]) + data blocks after CSBKs */
	uint8_t g = group ? 0x80 : 0x00;
	uint8_t gc = group ? 0xC0 : 0x80;
	for (int i = 0; i < preamble; i++)
	{
		uint8_t body[10];
		body[0] = 0xBD; body[1] = 0x00; body[2] = gc; body[3] = (uint8_t)((preamble - 1 - i) + tail);
		body[4] = (uint8_t)(dst >> 16); body[5] = (uint8_t)(dst >> 8); body[6] = (uint8_t)dst;
		body[7] = (uint8_t)(src >> 16); body[8] = (uint8_t)(src >> 8); body[9] = (uint8_t)src;
		uint8_t p12[12]; memcpy(p12, body, 10); hdr_crc(body, 10, 0xA5A5, p12 + 10);
		if (n >= DMR_DATA_MAX_BURSTS) { return -5; }
		n = append_burst(q, n, DTB_CSBK, p12);
	}
	/* Unconfirmed data header. SAP 09 [EXTD HDR] when encrypting (the ENC ext header is
	 * the extended header that follows); SAP 04 [IP Based] for cleartext (no extended
	 * header) — a stock TYT rejects SAP09-with-no-extended-header, decoded from a real
	 * stock cleartext SMS capture (2026-07-04). */
	{
		uint8_t h[10];
		/* Біт A (0x40) = «прошу квитанцію». Це Unconfirmed + Response Requested: приймач шле
		 * ОДНУ фінальну квитанцію на все повідомлення, БЕЗ поблокового ARQ -- тобто нам не
		 * потрібні ні DBSN, ні CRC9, ні rate-3/4 кодер. Саме так ми отримуємо «доставлено»,
		 * не воюючи з CRC9 (його 9 біт не вдалось звести до CRC від жодного шматка блока,
		 * попри вичерпний перебір поліномів і порядків -- див. STATUS.md).
		 * Вимикається через USB (dmrSmsAckSetTuning, 4-й байт), якщо якийсь приймач
		 * спотикається об виставлений A на Unconfirmed. */
		h[0] = (uint8_t)(g | 0x02 | (s_txAskAck ? 0x40 : 0x00));
		h[1] = (uint8_t)(((encrypt ? 9 : 4) << 4) | (poc & 0x0F));
		h[2] = (uint8_t)(dst >> 16); h[3] = (uint8_t)(dst >> 8); h[4] = (uint8_t)dst;
		h[5] = (uint8_t)(src >> 16); h[6] = (uint8_t)(src >> 8); h[7] = (uint8_t)src;
		h[8] = (uint8_t)(0x80 | (nblocks & 0x7F)); h[9] = 0x00;
		uint8_t p12[12]; memcpy(p12, h, 10); hdr_crc(h, 10, 0xCCCC, p12 + 10);
		if (n >= DMR_DATA_MAX_BURSTS) { return -5; }
		n = append_burst(q, n, DTB_DATA_HEADER, p12);
	}
	/* ENC extended header (SAP04 IP, MFID Moto, ALG05 AES256, key id, MI=0) — encrypted SMS only */
	if (encrypt)
	{
		uint8_t e[10] = { 0x4F, 0x10, 0x51, keyId, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		uint8_t p12[12]; memcpy(p12, e, 10); hdr_crc(e, 10, 0xCCCC, p12 + 10);
		if (n >= DMR_DATA_MAX_BURSTS) { return -5; }
		n = append_burst(q, n, DTB_DATA_HEADER, p12);
	}
	/* rate-1/2 data blocks */
	for (int b = 0; b < nDataBlocks; b++)
	{
		if (n >= DMR_DATA_MAX_BURSTS) { return -5; }
		n = append_burst(q, n, DTB_RATE12_DATA, pdu + b * 12);
	}

	/* Queue the Sent-folder write for AFTER the TX finishes (see s_pendSent / dmrSmsTxPersistTick)
	 * -- keeps the blocking flash erase+write out of the TX-keying window on general principle
	 * (see the comment above s_pendSent for why this turned out not to be bug #3 itself). */
	if (!s_diagSkipStore)
	{
		/* A prior send not yet flushed? The guard above ensured no active data call, so persist it
		 * now before reusing the single pending slot. In practice the main-loop tick already
		 * flushed it (menu sends are seconds apart). */
		if (s_pendSent)
		{
			store_add(s_pendFlags, s_pendPeer, s_pendText, s_pendTextLen);
			s_pendSent = 0;
		}
		s_pendFlags = (uint8_t)(DMR_SMS_FLAG_OUTGOING | (group ? DMR_SMS_FLAG_GROUP : 0));
		s_pendPeer = dst;
		s_pendTextLen = tlen;
		memcpy(s_pendText, text, tlen);
		s_pendText[tlen] = 0;
		s_pendSent = 1;
	}
	dmrDataTxLoad(q, (uint8_t)n);
	return 0;
}

/* ============================ RX ======================================== */
/* ISR-side reassembly state (CCM, not zeroed at boot -> reset in dmrSmsRxReset). */
static volatile uint8_t  s_rxHaveHeader DMR_AES_CCM;
static volatile uint8_t  s_rxHaveEnc DMR_AES_CCM;
static volatile uint8_t  s_rxExpBlocks DMR_AES_CCM;   /* rate-1/2 blocks expected (nblocks-1) */
static volatile uint8_t  s_rxCount DMR_AES_CCM;
static volatile uint8_t  s_rxGroup DMR_AES_CCM;
static volatile uint8_t  s_rxKeyId DMR_AES_CCM;
static volatile uint32_t s_rxSrc DMR_AES_CCM;
static volatile uint32_t s_rxDst DMR_AES_CCM;
/* Блоки навантаження пишемо ПРЯМО в s_rxPdu у міру надходження (замість окремого масиву
 * блоків -- економить CCM і природно підтримує і 12-байтні rate-1/2, і 18-байтні rate-3/4).
 * s_rxLen -- скільки байтів уже накопичено. */
static volatile uint16_t s_rxLen DMR_AES_CCM;
/* hand-off to main loop */
static volatile uint8_t  s_rxReady DMR_AES_CCM;       /* a complete PDU is waiting */
static uint8_t  s_rxPdu[384] DMR_AES_CCM;
static volatile uint16_t s_rxPduLen DMR_AES_CCM;
static volatile uint32_t s_rxPeer DMR_AES_CCM;
static volatile uint32_t s_rxPeerDst DMR_AES_CCM;   /* адресат із ВІДКРИТОГО заголовка -- для фільтра */
static volatile uint8_t  s_rxPeerGroup DMR_AES_CCM;
static volatile uint8_t  s_rxPeerKeyId DMR_AES_CCM;
static volatile uint8_t  s_rxPeerEnc DMR_AES_CCM;   /* 1 = PDU carried the ENC header (decrypt); 0 = cleartext */
static volatile uint8_t  s_rxAckReq DMR_AES_CCM;    /* вхідний data-заголовок = CONFIRMED + прохання квитанції */
static volatile uint8_t  s_rxPeerAckReq DMR_AES_CCM;/* знімок s_rxAckReq на момент готового PDU */
/* diagnostic counters (visible on the Messages home screen) to localise RX failures */
static volatile uint32_t s_diagData   DMR_AES_CCM; /* ALL data-sync-class bursts the chip delivered */
static volatile uint32_t s_diagHdrOk  DMR_AES_CCM; /* type-6 data-header, CRC OK   */
static volatile uint32_t s_diagHdrBad DMR_AES_CCM; /* type-6 data-header, CRC bad  */
static volatile uint32_t s_diagBlkOk  DMR_AES_CCM; /* type-7 rate-1/2,   CRC OK    */
static volatile uint32_t s_diagBlkBad DMR_AES_CCM; /* type-7 rate-1/2,   CRC bad   */
static volatile uint32_t s_diagPdu    DMR_AES_CCM; /* completed PDUs handed to main loop */
static volatile uint32_t s_diagMsg    DMR_AES_CCM; /* successfully decrypted + stored    */
static volatile uint32_t s_diagType[16] DMR_AES_CCM; /* гістограма rxDataType (0..15) усіх data-sync бурстів -- щоб бачити, яким типом стокова шле навантаження */
/* snapshot of the last reassembled (encrypted) PDU, for offline inspection over USB */
static uint8_t  s_diagLastPdu[120] DMR_AES_CCM;
static volatile uint16_t s_diagLastPduLen DMR_AES_CCM;
static volatile uint8_t  s_diagLastKeyId DMR_AES_CCM;
static volatile uint8_t  s_diagLastExp DMR_AES_CCM;
static volatile uint32_t s_diagLastPeer DMR_AES_CCM;
/* СИРІ блоки навантаження, як їх віддає чип -- РАЗОМ зі службовими байтами (DBSN+CRC9).
 * Потрібні, щоб реверснути CRC9 по реальному еталону, а не вигадувати його: без валідного
 * CRC9 наші власні CONFIRMED-блоки приймач відкине. Штатний шлях ці 2 байти відкидає, тож
 * тут зберігаємо блок цілим. */
#define SMS_RAWBLK_MAX   8
#define SMS_RAWBLK_SIZE  18
static uint8_t  s_rawBlk[SMS_RAWBLK_MAX][SMS_RAWBLK_SIZE] DMR_AES_CCM;
static volatile uint8_t s_rawBlkLen[SMS_RAWBLK_MAX] DMR_AES_CCM;
static volatile uint8_t s_rawBlkCount DMR_AES_CCM;

/* Fill out with [pduLen_hi,pduLen_lo, keyId, expBlocks, peer(4 LE), rawPdu...]. Returns bytes.
 * pduLen (and the raw bytes) are clamped to the snapshot buffer size: a PDU longer than
 * sizeof s_diagLastPdu is stored truncated, so only that many bytes exist to dump. */
int dmrSmsRxLastPdu(uint8_t *out, int maxlen)
{
	int n = s_diagLastPduLen;
	if (n > (int)sizeof s_diagLastPdu) { n = (int)sizeof s_diagLastPdu; }
	if (maxlen < 8 + n) { n = maxlen - 8; if (n < 0) n = 0; }
	out[0] = (uint8_t)(s_diagLastPduLen >> 8);
	out[1] = (uint8_t)(s_diagLastPduLen);
	out[2] = s_diagLastKeyId;
	out[3] = s_diagLastExp;
	out[4] = (uint8_t)(s_diagLastPeer);
	out[5] = (uint8_t)(s_diagLastPeer >> 8);
	out[6] = (uint8_t)(s_diagLastPeer >> 16);
	out[7] = (uint8_t)(s_diagLastPeer >> 24);
	for (int i = 0; i < n; i++) { out[8 + i] = s_diagLastPdu[i]; }
	return 8 + n;
}

/* Called for EVERY data-sync-class burst (any type, any CRC) so we can see exactly what
 * the HR-C6000 delivers during a stock SMS transmission. ISR context: counters only. */
void dmrSmsRxDiagBurst(int rxDataType, int crcOk)
{
	s_diagData++;
	s_lastRxBurstMs = ticksGetMillis();   /* канал зайнятий ЗАРАЗ -- гейт тиші для квитанції */
	s_diagType[rxDataType & 0x0F]++;   /* гістограма типів -- бачити тип блоків навантаження */
	if (rxDataType == DT_DATA_HEADER) { if (crcOk) s_diagHdrOk++; else s_diagHdrBad++; }
	else if (rxDataType == DT_RATE12_DATA) { if (crcOk) s_diagBlkOk++; else s_diagBlkBad++; }
}

void dmrSmsRxDiagReset(void)
{
	s_diagData = s_diagHdrOk = s_diagHdrBad = s_diagBlkOk = s_diagBlkBad = 0;
	s_diagPdu = s_diagMsg = 0;
	s_diagLastPduLen = 0;
	for (int i = 0; i < 16; i++) { s_diagType[i] = 0; }
	s_ackSeen = s_ackQueued = s_ackSent = s_ackStale = 0;
	s_ackLastHdr0 = s_ackLastHdr1 = s_ackLastGroup = s_ackLastForUs = 0;
	s_ackLastDelayMs = 0;
	s_rawBlkCount = 0;
	s_ackRcvdCount = 0;
}

/* Позначити найсвіжіше НАДІСЛАНЕ повідомлення цьому адресатові як доставлене.
 * Розкладка запису: [0]=flags [1]=textLen [2..3]=seq(LE) [4..7]=peerId(LE) [8..]=text. */
static void markDeliveredForPeer(uint32_t peer)
{
	store_ensure();
	int best = -1;
	uint16_t bestSeq = 0;
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		if (!entry_matches(o, 1)) { continue; }              /* тільки «Надіслані» */
		if (s_store.data[o] & DMR_SMS_FLAG_DELIVERED) { continue; }
		uint32_t pid = (uint32_t)s_store.data[o + 4] | ((uint32_t)s_store.data[o + 5] << 8) |
				((uint32_t)s_store.data[o + 6] << 16) | ((uint32_t)s_store.data[o + 7] << 24);
		if (pid != peer) { continue; }
		uint16_t sq = entry_seq(o);
		if ((best < 0) || (sq >= bestSeq)) { best = o; bestSeq = sq; }
	}
	if (best >= 0)
	{
		s_store.data[best] |= DMR_SMS_FLAG_DELIVERED;
		store_save();
	}
}

/* Дамп сирих блоків: [count, (len, bytes...) x count]. Повертає довжину. */
int dmrSmsRxRawBlocks(uint8_t *out, int maxlen)
{
	int n = 0;
	if (maxlen < 1) { return 0; }
	out[n++] = s_rawBlkCount;
	for (int b = 0; b < (int)s_rawBlkCount; b++)
	{
		int len = s_rawBlkLen[b];
		if (n + 1 + len > maxlen) { break; }
		out[n++] = (uint8_t)len;
		for (int i = 0; i < len; i++) { out[n++] = s_rawBlk[b][i]; }
	}
	return n;
}

/* Діагностика квитанції (USB 0x93, дописано в хвіст відповіді):
 *   [0]=заголовків CONFIRMED+A, [1]=поставлено в чергу, [2]=віддано в ефір,
 *   [3]=кинуто (канал не звільнився), [4]=p[0] ост. заголовка, [5]=p[1],
 *   [6]=ост. було груповим, [7]=ост. адресоване нам.
 * Читається: якщо [0]=0 -- стокова не просить квитанції (дивись [4]); якщо [0]>0, а [1]=0 --
 * відсіяв фільтр (груповий/не нам: [6]/[7]); якщо [1]>0, а [2]=0 -- канал не звільнявся. */
void dmrSmsAckDiag(uint32_t out[15])
{
	out[0] = s_ackSeen;   out[1] = s_ackQueued;
	out[2] = s_ackSent;   out[3] = s_ackStale;
	out[4] = s_ackLastHdr0; out[5] = s_ackLastHdr1;
	out[6] = s_ackLastGroup; out[7] = s_ackLastForUs;
	out[8] = SMS_ACK_FORMAT_REV;   /* яка саме прошивка залита -- щоб не гадати */
	out[9] = s_ackRepeats;
	out[10] = s_ackLastDelayMs;    /* виміряна пауза; еталон стокової ~80 мс */
	out[11] = s_ackTargetMs;       /* цільова пауза (підбірна) */
	out[12] = s_ackPreambles;      /* CSBK-преамбул перед квитанцією (підбірні) */
	out[13] = s_ackRcvdCount;      /* квитанцій НА НАШІ повідомлення прийнято */
	out[14] = s_txAskAck;          /* чи просимо квитанцію (біт A) на власних */
}

/* Гістограма rxDataType (16 значень) усіх прийнятих data-sync бурстів. */
void dmrSmsRxDiagTypes(uint32_t out[16])
{
	for (int i = 0; i < 16; i++) { out[i] = s_diagType[i]; }
}

void dmrSmsRxDiag(uint32_t out[7])
{
	out[0] = s_diagData;
	out[1] = s_diagHdrOk; out[2] = s_diagHdrBad;
	out[3] = s_diagBlkOk; out[4] = s_diagBlkBad;
	out[5] = s_diagPdu;   out[6] = s_diagMsg;
}

void dmrSmsRxReset(void)
{
	s_rxHaveHeader = 0; s_rxHaveEnc = 0; s_rxExpBlocks = 0; s_rxCount = 0; s_rxLen = 0;
	s_rxAckReq = 0;
}

void dmrSmsRxBurst(int rxDataType, const uint8_t *p)
{
	if (rxDataType == DT_DATA_HEADER)
	{
		/* Вхідна КВИТАНЦІЯ (Response data header, DPF=1) на НАШЕ повідомлення: адресат = ми.
		 * Розкладка та сама, що ми й самі шлемо: 01 40 <кому> <від кого> 00 08 <CRC>. */
		if (((p[0] & 0x0F) == 0x01) && (((uint32_t)p[2] << 16 | (uint32_t)p[3] << 8 | p[4]) == trxDMRID))
		{
			s_gotAckFrom = ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
			s_gotAck = 1;
			s_ackRcvdCount++;
			return;
		}

		if (p[0] == 0x4F && p[1] == 0x10 && (p[2] & 0x3F) == (0x51 & 0x3F))
		{
			/* Motorola ENC extended header: ALG/key/MI. It immediately precedes this
			 * message's data blocks, so restart block accumulation here — this prevents
			 * mixing leftover blocks from a previous (missed-terminator) retransmit. */
			s_rxKeyId = p[3];
			s_rxHaveEnc = 1;
			s_rxCount = 0;
			s_rxLen = 0;
		}
		else
		{
			/* Unconfirmed/UDT data header: start a fresh PDU */
			s_rxGroup = (p[0] & 0x80) ? 1 : 0;
			s_rxDst = ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
			s_rxSrc = ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
			uint8_t nblocks = (uint8_t)(p[8] & 0x7F);
			s_rxExpBlocks = (uint8_t)((nblocks > 0) ? (nblocks - 1) : 0);  /* minus ENC header */
			s_rxCount = 0;
			s_rxLen = 0;
			s_rxHaveHeader = 1;
			s_rxHaveEnc = 0;
			/* CONFIRMED data (DPF=3) -> відправник чекає link-layer квитанцію. Біт A (0x40)
			 * НЕ вимагаємо навмисно: за ETSI сам CONFIRMED означає підтверджувану доставку,
			 * а в ефірі (BBD_0005) стокова слала 0x43, тобто A однаково стоїть. Якщо ж
			 * конкретна прошивка шле 0x03 -- краще відповісти, ніж мовчати (зайва квитанція
			 * нікому не шкодить, а її брак дає ретрансміти). Unconfirmed (DPF=2) і UDT -- ні.
			 * Реальний байт видно в діагностиці (s_ackLastHdr0). */
			/* Квитуємо на ПРОХАННЯ (біт A), байдуже CONFIRMED воно чи Unconfirmed: саме так
			 * працює наш фрок-фрок обмін -- Unconfirmed + Response Requested. */
			s_rxAckReq = ((((p[0] & 0x0F) == 0x03) || (p[0] & 0x40)) ? 1 : 0);
			/* Діагностика: що НАСПРАВДІ прислала стокова (перші два байти заголовка). */
			s_ackLastHdr0 = p[0];
			s_ackLastHdr1 = p[1];
			if (s_rxAckReq) { s_ackSeen++; }
		}
		return;
	}

	/* Блоки навантаження: rate-1/2 (12 байт) АБО rate-3/4 (18 байт -- саме так шле стокова).
	 * Пишемо прямо в s_rxPdu; тип блока задає довжину. Завершення -- за CRC32 усього PDU
	 * (самотермінувальне, не довіряємо лічильнику блоків із заголовка). */
	if (rxDataType == DT_RATE12_DATA || rxDataType == DT_RATE34_DATA)
	{
		if (!s_rxHaveHeader) { return; }
		/* Не чіпаємо s_rxPdu, поки готовий PDU ще не забрав основний цикл (пишемо тепер
		 * прямо в s_rxPdu, тож інакше новий burst затер би те, що ще не прочитано). */
		if (s_rxReady) { return; }

		/* rate-1/2 (unconfirmed): весь 12-байтний блок -- навантаження.
		 * rate-3/4 від стокової -- CONFIRMED data: 18-байтний блок = [2 байти DBSN+CRC9] +
		 * [16 байтів навантаження]. Беремо лише 16 байтів навантаження: перші 2 службові й НЕ
		 * входять у data-PDU/CRC32. Підтверджено дампом з ефіру (2026-09-05): так блоки
		 * складаються в чистий IPv4/UDP/TMS і CRC32 усього PDU сходиться (0x43bc6082). */
		/* Знімок СИРОГО блока (до відкидання службових байтів) -- еталон для CRC9. */
		if (s_rawBlkCount < SMS_RAWBLK_MAX)
		{
			int raw = (rxDataType == DT_RATE34_DATA) ? 18 : 12;
			for (int i = 0; i < raw; i++) { s_rawBlk[s_rawBlkCount][i] = p[i]; }
			s_rawBlkLen[s_rawBlkCount] = (uint8_t)raw;
			s_rawBlkCount++;
		}

		const uint8_t *src = p;
		int blkLen = 12;
		if (rxDataType == DT_RATE34_DATA) { src = p + 2; blkLen = 16; }
		if ((int)s_rxLen + blkLen > (int)sizeof s_rxPdu) { return; }   /* захист від переповнення */
		memcpy(s_rxPdu + s_rxLen, src, blkLen);
		s_rxLen = (uint16_t)(s_rxLen + blkLen);
		s_rxCount++;

		/* Завершуємо, коли накопичене утворює CRC32-валідний data-PDU. Самотермінувально:
		 * змішування/обрив блоків просто не дасть валідний CRC32. Мінімум ~2 rate-1/2 блоки
		 * (24 Б). Гейт на s_rxHaveHeader (не s_rxHaveEnc), щоб і ЧИСТИЙ текст (без ENC-заголовка)
		 * теж збирався; тип шифрування несе s_rxHaveEnc, IPv4/UDP-перевірка в tick відкине не-SMS. */
		if (s_rxHaveHeader && (s_rxLen >= 24) && !s_rxReady)
		{
			int total = s_rxLen;
			uint32_t want = ((uint32_t)s_rxPdu[total - 4] << 24) | ((uint32_t)s_rxPdu[total - 3] << 16) |
					((uint32_t)s_rxPdu[total - 2] << 8) | (uint32_t)s_rxPdu[total - 1];
			if (crc32_dmr(s_rxPdu, total) != want)
			{
				return;   /* not a complete/clean PDU yet — keep accumulating */
			}
			s_rxPduLen = (uint16_t)total;
			s_rxPeer = s_rxSrc;
			s_rxPeerDst = s_rxDst;
			s_rxPeerGroup = s_rxGroup;
			s_rxPeerKeyId = s_rxKeyId;
			s_rxPeerEnc = s_rxHaveEnc;   /* decrypt if the ENC header was seen, else read cleartext */
			s_rxPeerAckReq = s_rxAckReq; /* чи винні ми квитанцію за це повідомлення */
			s_rxReady = 1;          /* main loop will decrypt (or read cleartext) + store */
			s_diagPdu++;
			/* snapshot raw (still-encrypted) PDU for USB inspection (clamped to the
			 * snapshot buffer — the reported length must never exceed the bytes stored) */
			s_diagLastPduLen = (uint16_t)((total > (int)sizeof s_diagLastPdu) ? (int)sizeof s_diagLastPdu : total);
			s_diagLastKeyId = s_rxKeyId;
			s_diagLastExp = s_rxExpBlocks;
			s_diagLastPeer = s_rxSrc;
			for (int i = 0; i < total && i < (int)sizeof s_diagLastPdu; i++) { s_diagLastPdu[i] = s_rxPdu[i]; }
			dmrSmsRxReset();
		}
	}
}

/* Чи адресоване це повідомлення саме НАМ?
 *
 *   - приватне (group == 0): адресат має точно збігатися з нашим DMR ID;
 *   - групове: адресат = TG активного каналу, будь-який TG зі списку RX-груп цього каналу,
 *     або All Call. Це той самий критерій, за яким пропускається ГОЛОС
 *     (hrc6000CallAcceptFilter + currentRxGroupData), тож SMS і голос поводяться однаково.
 *
 * Адресат лежить у ВІДКРИТОМУ заголовку даних, тому перевірка робиться ДО розшифровки:
 * на чуже повідомлення не витрачається ані AES, ані блокуючий запис у флеш.
 *
 * NOT_IN_CODEPLUG_contactsTG[] уже містить розкриті номери TG (їх заповнює
 * codeplugRxGroupGetDataForIndex), тож звертань до флеша тут немає взагалі. */
static int smsIsForUs(uint32_t dst, int group)
{
	if (dst == 0) { return 0; }

	if (group == 0)
	{
		return (dst == trxDMRID);
	}

	if ((dst >= MIN_ALL_CALL_VALUE) && (dst <= MAX_ALL_CALL_VALUE)) { return 1; }

	/* trxTalkGroupOrPcId у старшому байті тримає тип виклику: 0x03 = приватний. Порівнювати
	 * ГРУПОВИЙ адресат з ним можна лише коли там справді TG, інакше рація, налаштована на
	 * приватний виклик 1234, приймала б групові повідомлення на TG 1234. */
	if ((((trxTalkGroupOrPcId >> 24) & 0xFF) != PC_CALL_FLAG) &&
			(dst == (trxTalkGroupOrPcId & 0x00FFFFFF)))
	{
		return 1;
	}

	for (int i = 0; i < currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup; i++)
	{
		if (currentRxGroupData.NOT_IN_CODEPLUG_contactsTG[i] == dst) { return 1; }
	}
	return 0;
}

void dmrSmsRxTick(void)
{
	dmrSmsTxPersistTick();   /* flush any deferred Sent-folder write once the TX has fully un-keyed */
	dmrSmsAckTick();         /* відключити квитанцію на вхідне CONFIRMED SMS, коли настав час */

	/* Прийшла квитанція на НАШЕ повідомлення -> позначка «доставлено». store_save() пише у
	 * флеш (блокуюче), тож робимо це лише коли передача завершилась. */
	if (s_gotAck && !dmrDataTxActive() && !trxIsTransmitting)
	{
		uint32_t from = s_gotAckFrom;
		s_gotAck = 0;
		markDeliveredForPeer(from);
		/* Без нового мовного рядка (його довелось би заводити в усіх мовах): факт доставки
		 * видно позначкою в списку «Надіслані», а тут лише короткий сигнал. */
		soundSetMelody(MELODY_ACK_BEEP);
	}

	if (!s_rxReady) { return; }

	/* snapshot the FULL pdu (incl. pad+crc32), then release the ISR buffer */
	uint8_t pdu[384];
	int pduLen = s_rxPduLen;
	uint32_t peer = s_rxPeer;
	uint32_t dst = s_rxPeerDst;
	uint8_t  group = s_rxPeerGroup;
	uint8_t  keyId = s_rxPeerKeyId;
	uint8_t  enc = s_rxPeerEnc;
	uint8_t  ackReq = s_rxPeerAckReq;
	if (pduLen > (int)sizeof pdu) { pduLen = (int)sizeof pdu; }
	memcpy(pdu, s_rxPdu, pduLen);
	s_rxReady = 0;

	if (pduLen < 32) { return; }            /* whole PDU is passed; decrypt derives the enc len */

	/* Адресний фільтр. Раніше його не було зовсім: приймалося ВСЕ, що пролізло крізь
	 * частоту/таймслот/кольоровий код і відкрилося будь-яким нашим ключем -- зокрема приватні
	 * повідомлення між іншими рацями. Наслідки були не лише "етичні": чужий трафік витісняв
	 * власні повідомлення з 1342-байтового сховища, кожне з них тягло блокуючий запис у флеш,
	 * банер і звук, а на екрані видно лише ВІДПРАВНИКА -- тобто чужий наказ виглядав як свій.
	 *
	 * Тепер за замовчуванням приймається лише адресоване нам. Монітор (Повідомлення >
	 * "Монітор: увімк") лишає стару поведінку для навмисного прослуховування -- але тоді
	 * чуже позначається DMR_SMS_FLAG_FOREIGN і показується окремо. */
	int forUs = smsIsForUs(dst, group);
	if ((forUs == 0) && (settingsIsOptionBitSet(BIT_SMS_MONITOR_ALL) == false))
	{
		return;
	}

	/* Validate the data-PDU CRC32 over [ct+pad] before trusting the bytes. This rejects
	 * reassemblies that mixed blocks across retransmits (a missed burst/header) — without it
	 * a corrupted PDU decrypts to garbage and, worse, can store a junk message. */
	{
		uint32_t want = ((uint32_t)pdu[pduLen - 4] << 24) | ((uint32_t)pdu[pduLen - 3] << 16) |
				((uint32_t)pdu[pduLen - 2] << 8) | (uint32_t)pdu[pduLen - 1];
		if (crc32_dmr(pdu, pduLen) != want) { return; }   /* corrupted reassembly -> drop */
	}

	/* Link-layer квитанція: блоки прийнято цілими (CRC32 зійшовся) і повідомлення адресоване
	 * саме нам приватно, а відправник просив підтвердити (CONFIRMED+A) -> шлемо Response header.
	 * Робимо це ДО розшифровки: квитанція -- про доставку кадрів, а не про те, чи ми прочитали
	 * текст (стокова так само квитує ще до показу). Групові й «моніторні»/чужі не квитуємо. */
	s_ackLastGroup = group;
	s_ackLastForUs = (uint8_t)(forUs ? 1 : 0);
	/* Квитанцію ставимо в чергу, а віддає її dmrSmsAckTick() -- щойно канал ЗАМОВКНЕ.
	 * Два ефірні заміри задали це вікно з обох боків:
	 *  - rev 5 (BBD_0006): форма квитанції правильна байт-у-байт, але вийшла через 366 мс
	 *    по кінці передачі проти ~80 мс у стокової -- ПІЗНО, вікно закрите. Левову частку
	 *    тих 366 мс з'їдав store_add() нижче: блокуючий erase+write SPI-флеша тримав
	 *    головний цикл, і відкладена квитанція не могла піти.
	 *  - rev 6: віддавали негайно по CRC32 -- але PDU збирається на ОСТАННЬОМУ блоці даних,
	 *    після якого відправник ще шле термінатор. Тобто ми лізли в ефір, поки він передає
	 *    (отже глухий) -- РАНО.
	 * Тому: чекаємо тиші SMS_ACK_QUIET_MS, ключуємо через SMS_ACK_KEY_DELAY_MS (разом
	 * ~45-60 мс після кінця передачі, як у стокової), а блокуючий запис у флеш відкладено
	 * до моменту, коли квитанція вже пішла -- інакше він знову з'їсть усе вікно. */
	int ackedNow = 0;
	if (ackReq && forUs && !group)
	{
		queueSmsAck(peer);
		ackedNow = 1;   /* квитанція винна -> запис у флеш відкладаємо, щоб не тримав цикл */
	}

	char text[DMR_SMS_TEXT_MAX + 1];
	int got = -1;

	if (enc)
	{
		/* Encrypted: try the signalled key id first, then every loaded key (decrypt is
		 * destructive, so each attempt works on a fresh copy of the ciphertext). */
		uint8_t tmp[384];
		for (int attempt = 0; attempt <= DMR_AES_MAX_KEYS && got < 0; attempt++)
		{
			uint8_t k = (attempt == 0) ? keyId : (uint8_t)attempt;
			if (k == 0 || k >= DMR_AES_MAX_KEYS) { continue; }
			memcpy(tmp, pdu, pduLen);
			int r = dmr_aes_sms_decrypt(k, tmp, pduLen, text, sizeof text);
			if (r > 0) { got = r; }
		}
	}
	else
	{
		/* Cleartext SMS: the reassembled PDU IS the plaintext IPv4/UDP/TMS packet. */
		got = dmr_sms_text_from_plaintext(pdu, pduLen, text, sizeof text);
	}
	if (got <= 0) { return; }   /* wrong/no key, not IPv4/UDP, or not an SMS */

	{
		uint8_t flags = (uint8_t)(DMR_SMS_FLAG_UNREAD | (group ? DMR_SMS_FLAG_GROUP : 0) |
				(forUs ? 0 : DMR_SMS_FLAG_FOREIGN));
		if (ackedNow)
		{
			/* Квитанція ЗАРАЗ у польоті. store_add() -- блокуючий erase+write SPI-флеша на
			 * сотні мс; якщо зробити його тут, він з'їсть саме те вікно, заради якого ми
			 * поспішали (і взагалі блокуючий флеш у вікні ключування -- крихко, та сама
			 * причина, що й для відкладеного запису теки «Надіслані»). Відкладаємо до
			 * завершення передачі -- зливає dmrSmsInboxPersistTick() з головного циклу. */
			/* Якщо попереднє відкладене ще не зляглось -- запишемо його ЗАРАЗ, інакше воно
			 * просто загубиться (слот один). Той самий запобіжник, що й для «Надісланих». */
			if (s_pendInbox)
			{
				store_add(s_pendInboxFlags, s_pendInboxPeer, s_pendInboxText, s_pendInboxLen);
				s_pendInbox = 0;
			}
			s_pendInbox = 1;
			s_pendInboxFlags = flags;
			s_pendInboxPeer = peer;
			s_pendInboxLen = got;
			memcpy(s_pendInboxText, text, (size_t)got);
			s_pendInboxText[got] = 0;
		}
		else
		{
			store_add(flags, peer, text, got);
		}
	}
	s_diagMsg++;

	/* notify the user: visual banner (existing) + audible alert (new — раніше цей шлях був
	 * німим, і вхідне SMS можна було пропустити, якщо не дивитись на екран саме в цю мить).
	 * Перехоплене в моніторі позначається просто в тексті банера: свій наказ і чужий мають
	 * різнитися вже в ту секунду, коли банер вискочив, а не лише в списку. */
	char note[DMR_SMS_TEXT_MAX + 12];
	snprintf(note, sizeof note, forUs ? "SMS: %s" : "SMS>: %s", text);
	uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 4000, note, true);
	soundSetMelody(MELODY_SMS_RECEIVED_BEEP);
}

/* Clear all CCM runtime state to known values (see the forward decl up top). */
static void runtimeReset(void)
{
	s_cfgLoaded = 0;                 /* force the MSGC config to (re)load */
	s_diagData = s_diagHdrOk = s_diagHdrBad = s_diagBlkOk = s_diagBlkBad = 0;
	s_diagPdu = s_diagMsg = 0;
	for (int i = 0; i < 16; i++) { s_diagType[i] = 0; }
	s_rxReady = 0;                   /* don't process stray garbage as a PDU */
	s_ackPending = 0;                /* не тягнути квитанцію через зміну каналу */
	s_pendInbox = 0;                 /* і відкладений запис вхідного теж */
	dmrSmsRxReset();                 /* clear the burst accumulator */
}

#endif /* ENABLE_DMR_DATA && ENABLE_AES */

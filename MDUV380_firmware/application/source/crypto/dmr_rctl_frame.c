/* dmr_rctl_frame.c — див. dmr_rctl_frame.h. Побайтова адаптація TX-секції dmr_sms.c під
 * фіксований 16-байтний, завжди-шифрований PDU. Без STM32-залежностей: crypto/dmr_aes.h
 * (aes256_ecb_encrypt) сам по собі чистий і вже тестується на хості (tests/test_dmr_aes.c). */
#include "crypto/dmr_rctl_frame.h"
#include "crypto/dmr_aes.h"
#include <string.h>

/* Burst-типи HR-C6000 (сторінка 0x04, регістр 0x50: type<<4) -- ті самі значення, що й у
 * dmr_sms.c, ETSI-стандартні, тож не project-специфічні "магічні числа". */
#define DTB_CSBK         0x30
#define DTB_DATA_HEADER  0x60
#define DTB_RATE12_DATA  0x70

static uint16_t ip_crc16d(const uint8_t *data, int len)   /* CCITT, poly 0x1021, ^0xFFFF */
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
	uint16_t v = (uint16_t)(ip_crc16d(h, len) ^ mask);
	out2[0] = (uint8_t)(v >> 8); out2[1] = (uint8_t)(v & 0xFF);
}

uint32_t dmr_rctl_crc32(const uint8_t *pdu, int len)
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
	return ((crc & 0xFF) << 24) | ((crc & 0xFF00) << 8) | ((crc >> 8) & 0xFF00) | ((crc >> 24) & 0xFF);
}

static int append_burst(uint8_t *q, int n, uint8_t typeByte, const uint8_t *p12)
{
	q[n * 13 + 0] = typeByte;
	memcpy(q + n * 13 + 1, p12, 12);
	return n + 1;
}

int dmr_rctl_build_tx_bursts(uint32_t dst, uint32_t src, uint8_t keyId, const uint8_t key[32],
                              const dmr_rctl_msg_t *msg, uint8_t *q)
{
	if (key == NULL) { return -1; }

	/* 1) plaintext(16) -> шифруємо ЦІЛИЙ блок (на відміну від SMS тут нема відкритого
	 *    хвоста -- команда керування або повністю зашифрована, або не йде в ефір). */
	uint8_t ct[16];
	dmr_rctl_pack(msg, ct);
	aes256_ecb_encrypt(key, ct);

	/* 2) pdu = ct(16) + pad(4) + crc32(4) = 24 B = 2 блоки по 12 B (та сама формула
	 *    заповнення, що й у dmr_sms.c, підставлена під фіксовану довжину). */
	uint8_t pdu[24];
	memcpy(pdu, ct, 16);
	memset(pdu + 16, 0, 4);
	uint32_t crc = dmr_rctl_crc32(pdu, 24);
	pdu[20] = (uint8_t)(crc >> 24); pdu[21] = (uint8_t)(crc >> 16);
	pdu[22] = (uint8_t)(crc >> 8);  pdu[23] = (uint8_t)crc;
	const int nDataBlocks = 2;
	const int nblocks = 1 /* ENC header */ + nDataBlocks;

	/* 3) черга бургстів: CSBK-преамбул (індивідуальний виклик, dst=ціль, src=свій ID) +
	 *    Unconfirmed Data Header (SAP09, зашифрований) + ENC extended header + 2 rate-1/2. */
	int n = 0;
	const int preamble = 6;
	const int tail = 2 /* Unconfirmed + ENC headers */ + nDataBlocks;
	for (int i = 0; i < preamble; i++)
	{
		uint8_t body[10];
		body[0] = 0xBD; body[1] = 0x00; body[2] = 0x80; /* individual call, не групова */
		body[3] = (uint8_t)((preamble - 1 - i) + tail);
		body[4] = (uint8_t)(dst >> 16); body[5] = (uint8_t)(dst >> 8); body[6] = (uint8_t)dst;
		body[7] = (uint8_t)(src >> 16); body[8] = (uint8_t)(src >> 8); body[9] = (uint8_t)src;
		uint8_t p12[12]; memcpy(p12, body, 10); hdr_crc(body, 10, 0xA5A5, p12 + 10);
		n = append_burst(q, n, DTB_CSBK, p12);
	}
	{
		uint8_t h[10];
		h[0] = 0x02; /* individual, response-requested=0 */
		h[1] = (uint8_t)((9 << 4) | 0x04); /* SAP 09 [EXTD HDR], poc=4 (фіксований pad) */
		h[2] = (uint8_t)(dst >> 16); h[3] = (uint8_t)(dst >> 8); h[4] = (uint8_t)dst;
		h[5] = (uint8_t)(src >> 16); h[6] = (uint8_t)(src >> 8); h[7] = (uint8_t)src;
		h[8] = (uint8_t)(0x80 | (nblocks & 0x7F)); h[9] = 0x00;
		uint8_t p12[12]; memcpy(p12, h, 10); hdr_crc(h, 10, 0xCCCC, p12 + 10);
		n = append_burst(q, n, DTB_DATA_HEADER, p12);
	}
	{
		/* ENC extended header (SAP04 IP, MFID Moto, ALG05 AES256, key id, MI=0) -- та сама
		 * розкладка, що й dmr_sms.c використовує для шифрованих SMS. */
		uint8_t e[10] = { 0x4F, 0x10, 0x51, keyId, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		uint8_t p12[12]; memcpy(p12, e, 10); hdr_crc(e, 10, 0xCCCC, p12 + 10);
		n = append_burst(q, n, DTB_DATA_HEADER, p12);
	}
	for (int b = 0; b < nDataBlocks; b++)
	{
		n = append_burst(q, n, DTB_RATE12_DATA, pdu + b * 12);
	}
	return n;   /* == DMR_RCTL_TX_BURST_COUNT */
}

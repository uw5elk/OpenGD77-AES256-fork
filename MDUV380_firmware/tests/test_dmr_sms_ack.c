/*
 * test_dmr_sms_ack.c — контракт формату SMS-квитанції (link-layer ACK на CONFIRMED SMS).
 *
 * Формат знято з ефіру HackRF-ом (BBD_0005, 2026-09-12): стокова TYT 0x26EA3D квитує
 * CONFIRMED SMS від RT4D 0x26EA1B. Декодовано власним декодером (tools/dmr_data_rx.py):
 *
 *   квитанція (Response data header, DPF=1):
 *       01 40 26 ea 1b 26 ea 3d 00 08 e5 0b
 *   заголовок оригіналу (Confirmed data header, DPF=3, біт A=1):
 *       43 4b 26 ea 1b 26 ea 3d 83 38 35 b8   (тут перевіряємо лише CRC-контракт)
 *
 * CRC усіх data-заголовків: crc16d (CCITT poly 0x1021, ^0xFFFF) ^ 0xCCCC, big-endian.
 * Цей тест дублює побудову з dmr_sms.c (dmrSmsAckTick) БЕЗ залізних залежностей файлу
 * й звіряє байт-у-байт із ефірним зразком. Якщо колись зміниться CRC-маска, порядок полів
 * чи опкод -- тест впаде.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- копія з dmr_sms.c (той самий алгоритм) --- */
static uint16_t crc16d(const uint8_t *data, int len)
{
	uint16_t crc = 0;
	for (int i = 0; i < len; i++)
		for (int k = 7; k >= 0; k--)
		{
			int bit = (data[i] >> k) & 1;
			if (((crc >> 15) & 1) ^ bit) crc = (uint16_t)((crc << 1) ^ 0x1021);
			else                         crc = (uint16_t)(crc << 1);
		}
	return (uint16_t)(crc ^ 0xFFFF);
}
static void hdr_crc(const uint8_t *h, int len, uint16_t mask, uint8_t out2[2])
{
	uint16_t v = (uint16_t)(crc16d(h, len) ^ mask);
	out2[0] = (uint8_t)(v >> 8); out2[1] = (uint8_t)(v & 0xFF);
}

/* Побудова заголовка-квитанції — дзеркало dmrSmsAckTick() (без CSBK-преамбул: тут перевіряємо
 * сам Response header). */
static void build_ack(uint32_t to, uint32_t from, uint8_t bf, uint8_t out12[12])
{
	uint8_t h[10];
	h[0] = 0x01; h[1] = 0x40;
	h[2] = (uint8_t)(to >> 16); h[3] = (uint8_t)(to >> 8); h[4] = (uint8_t)to;
	h[5] = (uint8_t)(from >> 16); h[6] = (uint8_t)(from >> 8); h[7] = (uint8_t)from;
	h[8] = 0x00; h[9] = bf;
	memcpy(out12, h, 10);
	hdr_crc(h, 10, 0xCCCC, out12 + 10);
}

static int fails = 0;
static void expect_eq(const char *what, const uint8_t *got, const uint8_t *exp, int n)
{
	if (memcmp(got, exp, n) != 0)
	{
		fails++;
		printf("  FAIL %s\n    очік:", what);
		for (int i = 0; i < n; i++) printf(" %02x", exp[i]);
		printf("\n    маєм:");
		for (int i = 0; i < n; i++) printf(" %02x", got[i]);
		printf("\n");
	}
	else
	{
		printf("  ok   %s\n", what);
	}
}

int main(void)
{
	printf("test_dmr_sms_ack:\n");

	/* 1) Квитанція байт-у-байт як в ефірі (BBD_0005). */
	const uint8_t ackAir[12] = { 0x01,0x40, 0x26,0xea,0x1b, 0x26,0xea,0x3d, 0x00, 0x08, 0xe5,0x0b };
	uint8_t ack[12];
	build_ack(0x26EA1B /*кому: RT4D*/, 0x26EA3D /*від кого: стокова*/, 0x08, ack);
	expect_eq("Response header == ефірний зразок", ack, ackAir, 12);

	/* 2) CRC-контракт заголовка оригіналу (Confirmed data header): ті самі 10 байт -> ті самі CRC. */
	const uint8_t confAir[12] = { 0x43,0x4b, 0x26,0xea,0x1b, 0x26,0xea,0x3d, 0x83,0x38, 0x35,0xb8 };
	uint8_t crc2[2];
	hdr_crc(confAir, 10, 0xCCCC, crc2);
	expect_eq("Confirmed header CRC (^0xCCCC)", crc2, confAir + 10, 2);

	/* 3) Санітарна: інша адреса -> інший CRC (не константа). */
	uint8_t ack2[12];
	build_ack(0x123456, 0x654321, 0x08, ack2);
	if (memcmp(ack2 + 10, ack + 10, 2) == 0) { fails++; printf("  FAIL CRC не залежить від адрес\n"); }
	else { printf("  ok   CRC залежить від вмісту\n"); }

	printf(fails ? "ПРОВАЛ (%d)\n" : "ПРОЙДЕНО\n", fails);
	return fails ? 1 : 0;
}

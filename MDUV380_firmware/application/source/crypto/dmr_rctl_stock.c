/* dmr_rctl_stock.c — див. dmr_rctl_stock.h. Чиста побудова/розбір стокових CSBK-команд
 * керування TYT. Без STM32-залежностей, тестується на хості. */
#include "crypto/dmr_rctl_stock.h"
#include "crypto/dmr_rctl_pdu.h"   /* DMR_RCTL_ALLOW_* біти дозволів */
#include <string.h>

/* CRC-CCITT "d" (poly 0x1021, init 0, ^0xFFFF) над len байтами -- той самий, що
 * dmr_rctl_frame.c/dmr_enc_sms.py для заголовків. */
static uint16_t crc16d(const uint8_t *data, int len)
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

/* Дописати 2-байтний CSBK-CRC (^0xA5A5) у out[10..11] за перші 10 байт out. */
static void csbk_crc(uint8_t out12[12])
{
	uint16_t v = (uint16_t)(crc16d(out12, 10) ^ 0xA5A5);
	out12[10] = (uint8_t)(v >> 8);
	out12[11] = (uint8_t)(v & 0xFF);
}

/* Кодування кожної команди: (b0, керуючий байт). З реального ефіру:
 *   Check A4/00, Monitor 9D/01, Enable A4/7E, Disable A4/7F.
 * b0 несе опкод CSBK у молодших 6 бітах (0x24 для Check/Enable/Disable, 0x1D для Monitor)
 * і Last-Block у старшому. FID=0x10 (Motorola). */
static const uint8_t CMD_B0[DMR_RCTL_STOCK_NUM_CMDS]  = { 0xA4, 0x9D, 0xA4, 0xA4 };
static const uint8_t CMD_ARG[DMR_RCTL_STOCK_NUM_CMDS] = { 0x00, 0x01, 0x7E, 0x7F };
#define STOCK_FID  0x10

void dmr_rctl_stock_command(dmr_rctl_stock_cmd_t cmd, uint32_t src, uint32_t dst, uint8_t out12[12])
{
	if ((int)cmd < 0 || (int)cmd >= DMR_RCTL_STOCK_NUM_CMDS) { memset(out12, 0, 12); return; }
	out12[0] = CMD_B0[cmd];
	out12[1] = STOCK_FID;
	out12[2] = 0x00;
	out12[3] = CMD_ARG[cmd];
	out12[4] = (uint8_t)(src >> 16); out12[5] = (uint8_t)(src >> 8); out12[6] = (uint8_t)src;
	out12[7] = (uint8_t)(dst >> 16); out12[8] = (uint8_t)(dst >> 8); out12[9] = (uint8_t)dst;
	csbk_crc(out12);
}

void dmr_rctl_stock_preamble(uint32_t dst, uint32_t src, uint8_t countdown, uint8_t out12[12])
{
	out12[0] = 0xBD; out12[1] = 0x00; out12[2] = 0x00; out12[3] = countdown;
	out12[4] = (uint8_t)(dst >> 16); out12[5] = (uint8_t)(dst >> 8); out12[6] = (uint8_t)dst;
	out12[7] = (uint8_t)(src >> 16); out12[8] = (uint8_t)(src >> 8); out12[9] = (uint8_t)src;
	csbk_crc(out12);
}

int dmr_rctl_stock_build_tx(dmr_rctl_stock_cmd_t cmd, uint32_t src, uint32_t dst, uint8_t *q)
{
	if ((int)cmd < 0 || (int)cmd >= DMR_RCTL_STOCK_NUM_CMDS) { return 0; }
	int n = 0;
	for (int i = 0; i < DMR_RCTL_STOCK_PREAMBLES; i++)
	{
		/* лічильник = кадрів, що лишились разом із командою: 16..1, потім команда */
		q[n * 13] = DMR_RCTL_STOCK_BURST_CSBK;
		dmr_rctl_stock_preamble(dst, src, (uint8_t)(DMR_RCTL_STOCK_PREAMBLES - i), q + n * 13 + 1);
		n++;
	}
	q[n * 13] = DMR_RCTL_STOCK_BURST_CSBK;
	dmr_rctl_stock_command(cmd, src, dst, q + n * 13 + 1);
	n++;
	return n;
}

int dmr_rctl_stock_parse(const uint8_t in12[12], dmr_rctl_stock_cmd_t *cmd, uint32_t *src, uint32_t *dst)
{
	/* Обгортка: FID=0x10, байт2=0x00; преамбула (0xBD) сюди не підходить -> 0. */
	if (in12[1] != STOCK_FID || in12[2] != 0x00) { return 0; }

	/* CRC має збігтись -- інакше це шум/чужий кадр. */
	uint16_t want = (uint16_t)((in12[10] << 8) | in12[11]);
	if ((uint16_t)(crc16d(in12, 10) ^ 0xA5A5) != want) { return 0; }

	/* Знайти команду за (b0, керуючий байт). */
	for (int c = 0; c < DMR_RCTL_STOCK_NUM_CMDS; c++)
	{
		if (in12[0] == CMD_B0[c] && in12[3] == CMD_ARG[c])
		{
			if (cmd) { *cmd = (dmr_rctl_stock_cmd_t)c; }
			if (src) { *src = ((uint32_t)in12[4] << 16) | ((uint32_t)in12[5] << 8) | in12[6]; }
			if (dst) { *dst = ((uint32_t)in12[7] << 16) | ((uint32_t)in12[8] << 8) | in12[9]; }
			return 1;
		}
	}
	return 0;
}

/* Біт дозволу для кожної команди. Enable=revive, Disable=stun -- мітки наших дозволів
 * історичні, тож мапимо явно, щоб не сплутати. */
static uint8_t allowBitFor(dmr_rctl_stock_cmd_t cmd)
{
	switch (cmd)
	{
		case DMR_RCTL_STOCK_CHECK:   return DMR_RCTL_ALLOW_CHECK;
		case DMR_RCTL_STOCK_MONITOR: return DMR_RCTL_ALLOW_MONITOR;
		case DMR_RCTL_STOCK_ENABLE:  return DMR_RCTL_ALLOW_REVIVE;
		case DMR_RCTL_STOCK_DISABLE: return DMR_RCTL_ALLOW_STUN;
		default:                     return 0;   /* невідома -> fail-closed */
	}
}

int dmr_rctl_stock_should_act(dmr_rctl_stock_cmd_t cmd, uint32_t dst, uint32_t ourId, uint8_t allowMask)
{
	if (dst == 0 || dst != ourId) { return 0; }   /* не нам (індивідуальний виклик, без All-Call) */
	uint8_t bit = allowBitFor(cmd);
	if (bit == 0) { return 0; }                    /* невідома команда -> заборонено */
	return ((allowMask & bit) != 0) ? 1 : 0;
}

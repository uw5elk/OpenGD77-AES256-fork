/*
 * test_dmr_rctl_stock.c — хостовий тест побудови/розбору стокових CSBK-команд керування.
 * Головне: відтворити РЕАЛЬНІ захвати зі стокової TYT (2026-09-05) байт-у-байт, і що
 * парсер упізнає кожну команду й правильно дістає src/dst. Без STM32 — звичайний gcc.
 *
 *   gcc -O2 -Wall -Wextra -DENABLE_AES -DENABLE_DMR_DATA -I ../application/include \
 *       -o t test_dmr_rctl_stock.c ../application/source/crypto/dmr_rctl_stock.c && ./t
 */
#include "crypto/dmr_rctl_stock.h"
#include "crypto/dmr_rctl_pdu.h"   /* DMR_RCTL_ALLOW_* */
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, name) do { \
	if (cond) { printf("  PASS %s\n", name); } \
	else { printf("  FAIL %s\n", name); fails++; } \
} while (0)

/* стокова 2550333 -> форк 2550287, з реального ефіру */
#define SRC 2550333u
#define DST 2550287u

static int eqhex(const uint8_t *got, const char *hexs)
{
	uint8_t want[12]; int n = 0;
	const char *p = hexs;
	while (*p && n < 12) {
		if (*p == ' ') { p++; continue; }
		unsigned v; sscanf(p, "%2x", &v); want[n++] = (uint8_t)v; p += 2;
	}
	return (n == 12) && (memcmp(got, want, 12) == 0);
}

int main(void)
{
	uint8_t b[12];

	/* --- команди байт-у-байт --- */
	dmr_rctl_stock_command(DMR_RCTL_STOCK_CHECK, SRC, DST, b);
	CHECK(eqhex(b, "a4 10 00 00 26 ea 3d 26 ea 0f 93 7c"), "Radio Check байт-у-байт");
	dmr_rctl_stock_command(DMR_RCTL_STOCK_MONITOR, SRC, DST, b);
	CHECK(eqhex(b, "9d 10 00 01 26 ea 3d 26 ea 0f a3 88"), "Remote Monitor байт-у-байт");
	dmr_rctl_stock_command(DMR_RCTL_STOCK_ENABLE, SRC, DST, b);
	CHECK(eqhex(b, "a4 10 00 7e 26 ea 3d 26 ea 0f 25 95"), "Radio Enable байт-у-байт");
	dmr_rctl_stock_command(DMR_RCTL_STOCK_DISABLE, SRC, DST, b);
	CHECK(eqhex(b, "a4 10 00 7f 26 ea 3d 26 ea 0f 9d f4"), "Radio Disable байт-у-байт");

	/* --- преамбули байт-у-байт (dst,src — порядок стокової) --- */
	dmr_rctl_stock_preamble(DST, SRC, 0x02, b);
	CHECK(eqhex(b, "bd 00 00 02 26 ea 0f 26 ea 3d c6 69"), "преамбула 0x02");
	dmr_rctl_stock_preamble(DST, SRC, 0x10, b);
	CHECK(eqhex(b, "bd 00 00 10 26 ea 0f 26 ea 3d 91 f1"), "преамбула 0x10");

	/* --- розбір: кожна команда впізнається, src/dst правильні --- */
	for (int c = 0; c < DMR_RCTL_STOCK_NUM_CMDS; c++)
	{
		dmr_rctl_stock_command((dmr_rctl_stock_cmd_t)c, SRC, DST, b);
		dmr_rctl_stock_cmd_t pc; uint32_t ps, pd;
		int ok = dmr_rctl_stock_parse(b, &pc, &ps, &pd);
		char nm[48]; snprintf(nm, sizeof nm, "розбір команди %d (src/dst)", c);
		CHECK(ok && pc == (dmr_rctl_stock_cmd_t)c && ps == SRC && pd == DST, nm);
	}

	/* --- преамбула НЕ вважається командою --- */
	dmr_rctl_stock_preamble(DST, SRC, 0x05, b);
	CHECK(dmr_rctl_stock_parse(b, NULL, NULL, NULL) == 0, "преамбула не приймається за команду");

	/* --- зіпсований CRC відкидається --- */
	dmr_rctl_stock_command(DMR_RCTL_STOCK_CHECK, SRC, DST, b);
	b[11] ^= 0xFF;
	CHECK(dmr_rctl_stock_parse(b, NULL, NULL, NULL) == 0, "битий CRC відкидається");

	/* --- черга TX: правильна кількість бургстів, останній = команда, решта преамбули --- */
	{
		uint8_t q[(DMR_RCTL_STOCK_PREAMBLES + 1) * 13];
		int n = dmr_rctl_stock_build_tx(DMR_RCTL_STOCK_DISABLE, SRC, DST, q);
		CHECK(n == DMR_RCTL_STOCK_PREAMBLES + 1, "кількість бургстів = преамбули + 1");
		int all_csbk = 1;
		for (int i = 0; i < n; i++) { if (q[i * 13] != DMR_RCTL_STOCK_BURST_CSBK) { all_csbk = 0; } }
		CHECK(all_csbk, "усі бургсти типу CSBK");
		/* останній бургст має розбиратись як Disable */
		dmr_rctl_stock_cmd_t pc;
		CHECK(dmr_rctl_stock_parse(q + (n - 1) * 13 + 1, &pc, NULL, NULL) == 1 && pc == DMR_RCTL_STOCK_DISABLE,
		      "останній бургст = команда Disable");
		/* перший бургст — преамбула (не команда) з лічильником = PREAMBLES */
		CHECK(dmr_rctl_stock_parse(q + 1, NULL, NULL, NULL) == 0 && q[1] == 0xBD && q[4] == DMR_RCTL_STOCK_PREAMBLES,
		      "перший бургст = преамбула, лічильник = PREAMBLES");
	}

	/* --- гейт прийому (форк як ціль): dst==ourId + дозвіл --- */
	{
		const uint32_t US = 2550313u, OTHER = 2550333u;
		const uint8_t ALL = DMR_RCTL_ALLOW_CHECK | DMR_RCTL_ALLOW_MONITOR |
		                    DMR_RCTL_ALLOW_STUN | DMR_RCTL_ALLOW_REVIVE;

		CHECK(dmr_rctl_stock_should_act(DMR_RCTL_STOCK_CHECK, US, US, ALL) == 1, "Check нам+дозволено -> діяти");
		CHECK(dmr_rctl_stock_should_act(DMR_RCTL_STOCK_CHECK, OTHER, US, ALL) == 0, "Check не нам -> ігнор");
		CHECK(dmr_rctl_stock_should_act(DMR_RCTL_STOCK_CHECK, US, US, 0) == 0, "Check без дозволу -> ігнор");
		/* точкові дозволи: лише свій біт відмикає свою команду */
		CHECK(dmr_rctl_stock_should_act(DMR_RCTL_STOCK_DISABLE, US, US, DMR_RCTL_ALLOW_STUN) == 1, "Disable з ALLOW_STUN -> діяти");
		CHECK(dmr_rctl_stock_should_act(DMR_RCTL_STOCK_DISABLE, US, US, DMR_RCTL_ALLOW_CHECK) == 0, "Disable лише з ALLOW_CHECK -> ігнор");
		CHECK(dmr_rctl_stock_should_act(DMR_RCTL_STOCK_ENABLE, US, US, DMR_RCTL_ALLOW_REVIVE) == 1, "Enable з ALLOW_REVIVE -> діяти");
		CHECK(dmr_rctl_stock_should_act(DMR_RCTL_STOCK_MONITOR, US, US, DMR_RCTL_ALLOW_MONITOR) == 1, "Monitor з ALLOW_MONITOR -> діяти");
		CHECK(dmr_rctl_stock_should_act(DMR_RCTL_STOCK_CHECK, 0, US, ALL) == 0, "dst=0 -> ігнор");
	}

	/* --- КВИТАНЦІЯ на Radio Check: байт-у-байт проти реального захвату (2026-09-11) --- */
	{
		/* Ефір: RT4D(2550299) перевіряє стокову(2550333); квитанція стокової:
		 *   a4 10 00 80 26ea1b(RT4D=хто питав) 26ea3d(стокова=хто відповідає) 3e43 */
		const uint32_t RT4D = 2550299u, STOCKID = 2550333u;
		dmr_rctl_stock_ack(RT4D, STOCKID, b);
		CHECK(eqhex(b, "a4 10 00 80 26 ea 1b 26 ea 3d 3e 43"), "квитанція байт-у-байт (реальний захват)");

		/* розбір квитанції: requester/responder правильні */
		uint32_t rq = 0, rp = 0;
		int ok = dmr_rctl_stock_parse_ack(b, &rq, &rp);
		CHECK(ok && rq == RT4D && rp == STOCKID, "розбір квитанції (requester/responder)");

		/* квитанція НЕ впізнається як команда (arg 0x80 не входить у CMD_ARG) */
		CHECK(dmr_rctl_stock_parse(b, NULL, NULL, NULL) == 0, "квитанція не приймається за команду");
		/* команда Check НЕ впізнається як квитанція (arg 0x00 != 0x80) */
		dmr_rctl_stock_command(DMR_RCTL_STOCK_CHECK, RT4D, STOCKID, b);
		CHECK(dmr_rctl_stock_parse_ack(b, NULL, NULL) == 0, "команда Check не приймається за квитанцію");

		/* битий CRC квитанції відкидається */
		dmr_rctl_stock_ack(RT4D, STOCKID, b); b[10] ^= 0x55;
		CHECK(dmr_rctl_stock_parse_ack(b, NULL, NULL) == 0, "битий CRC квитанції відкидається");

		/* черга TX квитанції: рівно ACK_REPEATS бургстів CSBK, кожен розбирається як квитанція */
		uint8_t q[DMR_RCTL_STOCK_ACK_REPEATS * 13];
		int n = dmr_rctl_stock_build_ack_tx(RT4D, STOCKID, q);
		CHECK(n == DMR_RCTL_STOCK_ACK_REPEATS, "черга квитанції = ACK_REPEATS бургстів");
		int okq = 1;
		for (int i = 0; i < n; i++)
		{
			if (q[i * 13] != DMR_RCTL_STOCK_BURST_CSBK) { okq = 0; }
			uint32_t a = 0, c = 0;
			if (!dmr_rctl_stock_parse_ack(q + i * 13 + 1, &a, &c) || a != RT4D || c != STOCKID) { okq = 0; }
		}
		CHECK(okq, "усі бургсти квитанції валідні й однакові");
	}

	/* Квитанції Enable/Disable -- байти з реального захвату (кільце форка 2026-09-12,
	 * форк 2550313 керує стоковою 2550333; RCTL_COMPAT.md §6). */
	{
		const uint32_t FORK = 2550313, STOCKID = 2550333;
		uint8_t b[12];
		dmr_rctl_stock_cmd_t c; uint32_t rq, rp;

		dmr_rctl_stock_ack_for(DMR_RCTL_STOCK_ENABLE, FORK, STOCKID, b);
		CHECK(eqhex(b, "a4 10 00 fe 26 ea 29 26 ea 3d 49 2b"), "квитанція Enable байт-у-байт (захват)");
		CHECK(dmr_rctl_stock_parse_ack_for(b, &c, &rq, &rp) && c == DMR_RCTL_STOCK_ENABLE &&
		      rq == FORK && rp == STOCKID, "розбір квитанції Enable");

		dmr_rctl_stock_ack_for(DMR_RCTL_STOCK_DISABLE, FORK, STOCKID, b);
		CHECK(eqhex(b, "a4 10 00 ff 26 ea 29 26 ea 3d f1 4a"), "квитанція Disable байт-у-байт (захват)");
		CHECK(dmr_rctl_stock_parse_ack_for(b, &c, &rq, &rp) && c == DMR_RCTL_STOCK_DISABLE &&
		      rq == FORK && rp == STOCKID, "розбір квитанції Disable");

		/* старий вузький parse_ack приймає лише Check, не Enable/Disable */
		CHECK(dmr_rctl_stock_parse_ack(b, NULL, NULL) == 0, "вузький parse_ack — лише Check");

		/* квитанція Enable/Disable НЕ приймається за команду */
		CHECK(dmr_rctl_stock_parse(b, NULL, NULL, NULL) == 0, "квитанція Disable не — команда");

		/* битий CRC відкидається й тут */
		dmr_rctl_stock_ack_for(DMR_RCTL_STOCK_ENABLE, FORK, STOCKID, b); b[11] ^= 0x33;
		CHECK(dmr_rctl_stock_parse_ack_for(b, &c, &rq, &rp) == 0, "битий CRC квитанції Enable відкинуто");
	}

	printf(fails ? "\nПРОВАЛЕНО: %d\n" : "\nУсі тести пройдено\n", fails);
	return fails ? 1 : 0;
}

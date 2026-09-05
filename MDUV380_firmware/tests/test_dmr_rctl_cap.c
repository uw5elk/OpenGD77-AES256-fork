/*
 * test_dmr_rctl_cap.c — хостові тести кільця захоплення сирих burst (dmr_rctl_cap.c).
 * Перевіряє саме логіку кільця: arm скидає сеанс, стоп-коли-повне береже ПОЧАТОК і
 * рахує dropped, формат dump. Без STM32/SPI — звичайний gcc.
 *
 *   gcc -O2 -Wall -Wextra -DENABLE_AES -DENABLE_DMR_DATA -I ../application/include \
 *       -o test_dmr_rctl_cap test_dmr_rctl_cap.c ../application/source/functions/dmr_rctl_cap.c
 */
#include "functions/dmr_rctl_cap.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, name) do { \
	if (cond) { printf("  PASS %s\n", name); } \
	else { printf("  FAIL %s\n", name); fails++; } \
} while (0)

static void mkburst(uint8_t *b, uint8_t fill)
{
	for (int i = 0; i < DMR_RCTL_CAP_BURST_LEN; i++) { b[i] = (uint8_t)(fill + i); }
}

int main(void)
{
	uint8_t out[512];
	uint8_t b[DMR_RCTL_CAP_BURST_LEN];

	/* До arm захоплення вимкнене, dump порожній. */
	dmrRctlCapReset();
	CHECK(dmrRctlCapArmed() == 0, "після reset не armed");
	mkburst(b, 0x10);
	dmrRctlCapBurst(3, b);                       /* не armed -> ігнорується */
	int n = dmrRctlCapDump(out, sizeof out);
	CHECK(n == 4 && out[0] == 0 && out[3] == 0, "не armed: count=0, armed=0");

	/* Arm, потім три burst різних типів. */
	dmrRctlCapArm(1);
	CHECK(dmrRctlCapArmed() == 1, "після arm armed=1");
	mkburst(b, 0x30); dmrRctlCapBurst(3, b);     /* CSBK */
	mkburst(b, 0x60); dmrRctlCapBurst(6, b);
	mkburst(b, 0x70); dmrRctlCapBurst(7, b);
	n = dmrRctlCapDump(out, sizeof out);
	CHECK(out[0] == 3, "захоплено 3 burst");
	CHECK(out[1] == 0 && out[2] == 0, "dropped=0");
	CHECK(out[3] == 1, "armed=1 у dump");
	/* Перший запис: seq=1, type=3, data[0]=0x30. */
	CHECK(out[4] == 1 && out[5] == 3 && out[6] == 0x30, "перший burst: seq/type/data");
	/* Третій запис лежить зі зсувом 4 + 2*(2+12). */
	{
		int off = 4 + 2 * (2 + DMR_RCTL_CAP_BURST_LEN);
		CHECK(out[off] == 3 && out[off + 1] == 7 && out[off + 2] == 0x70, "третій burst: seq/type/data");
	}

	/* Повторний arm ПОЧИНАЄ чистий сеанс. */
	dmrRctlCapArm(1);
	n = dmrRctlCapDump(out, sizeof out);
	CHECK(out[0] == 0, "повторний arm очищає кільце");

	/* Стоп-коли-повне: заливаємо більше за SLOTS, зберігається ПОЧАТОК, dropped рахується. */
	dmrRctlCapArm(1);
	for (int i = 0; i < DMR_RCTL_CAP_SLOTS + 5; i++)
	{
		mkburst(b, (uint8_t)i);
		dmrRctlCapBurst(3, b);
	}
	n = dmrRctlCapDump(out, sizeof out);
	CHECK(out[0] == DMR_RCTL_CAP_SLOTS, "кільце заповнене рівно на SLOTS");
	CHECK(((out[1] << 8) | out[2]) == 5, "5 burst відкинуто (dropped)");
	/* Перший збережений — найперший поданий (data[0]==0), а не останній. */
	CHECK(out[6] == 0x00, "збережено ПОЧАТОК послідовності, не кінець");

	/* Disarm зупиняє захоплення, але лишає вже зібране до наступного arm/reset. */
	dmrRctlCapArm(0);
	CHECK(dmrRctlCapArmed() == 0, "disarm -> armed=0");
	mkburst(b, 0xAA); dmrRctlCapBurst(3, b);     /* ігнорується */
	n = dmrRctlCapDump(out, sizeof out);
	CHECK(out[0] == DMR_RCTL_CAP_SLOTS, "після disarm нові burst не додаються");

	printf(fails ? "\nПРОВАЛЕНО: %d\n" : "\nУсі тести пройдено\n", fails);
	return fails ? 1 : 0;
}

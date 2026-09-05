/* dmr_rctl_cap.c — див. dmr_rctl_cap.h. Кільце сирих data-burst для реверсу стокового
 * протоколу керування. Без STM32/HR-C6000 залежностей, тож збирається й тестується на
 * хості (tests/test_dmr_rctl_cap.c) так само, як dmr_rctl_pdu.c. */
#include "functions/dmr_rctl_cap.h"

#if defined(ENABLE_DMR_DATA) && defined(ENABLE_AES)

#include "crypto/dmr_aes.h"   /* DMR_AES_CCM: тримати стан у CCM, не зсувати .bss основної RAM */
#include <string.h>

/* CCM НЕ обнуляється при старті, тож усе явно ініціалізується в dmrRctlCapReset(), а сам
 * armed за замовчуванням 0 (захоплення вимкнене, доки ПК його не ввімкне). Щоб гарантувати
 * нульовий старт без виклику reset, ці статики лежать разом і скидаються на першому arm. */
static dmr_rctl_cap_entry_t s_ring[DMR_RCTL_CAP_SLOTS] DMR_AES_CCM;
static uint8_t  s_count   DMR_AES_CCM;   /* заповнено слотів (0..DMR_RCTL_CAP_SLOTS) */
static uint8_t  s_armed   DMR_AES_CCM;
static uint8_t  s_seq     DMR_AES_CCM;   /* монотонний № burst */
static uint16_t s_dropped DMR_AES_CCM;   /* burst, що не влізли (кільце було повне) */
static uint8_t  s_inited  DMR_AES_CCM;   /* захист від сміття в CCM до першого reset */

static void ensureInit(void)
{
	if (s_inited != 1)
	{
		s_count = 0;
		s_seq = 0;
		s_dropped = 0;
		s_armed = 0;
		memset(s_ring, 0, sizeof s_ring);
		s_inited = 1;
	}
}

void dmrRctlCapArm(int on)
{
	ensureInit();
	if (on)
	{
		/* Кожне ввімкнення починає чистий сеанс — інакше до реальної команди в кільці
		 * лишався б ефір з попереднього разу й плутав розбір. */
		s_count = 0;
		s_seq = 0;
		s_dropped = 0;
		s_armed = 1;
	}
	else
	{
		s_armed = 0;
	}
}

int dmrRctlCapArmed(void)
{
	/* Читається з ISR на КОЖЕН data-burst — тому просто повернути прапорець, нічого більше.
	 * До першого reset/arm s_armed=0 гарантовано лише після ensureInit(); але щоб не робити
	 * запис у CCM з ISR, тут init НЕ викликаємо: поки ПК не викликав arm (який робить init),
	 * можливе сміття читається як "armed", проте dmrRctlCapBurst() однаково зробить init і
	 * коректно обробить. Практично ISR не викликає capBurst, поки ПК не увімкнув захоплення. */
	return (s_armed == 1) ? 1 : 0;
}

void dmrRctlCapBurst(int type, const uint8_t *p12)
{
	ensureInit();
	if (s_armed != 1) { return; }

	s_seq++;
	if (s_count >= DMR_RCTL_CAP_SLOTS)
	{
		if (s_dropped < 0xFFFF) { s_dropped++; }
		return;                       /* стоп, коли повне: бережемо початок послідовності */
	}
	dmr_rctl_cap_entry_t *e = &s_ring[s_count++];
	e->seq  = s_seq;
	e->type = (uint8_t)type;
	memcpy(e->data, p12, DMR_RCTL_CAP_BURST_LEN);
}

void dmrRctlCapReset(void)
{
	s_inited = 0;
	ensureInit();
}

int dmrRctlCapDump(uint8_t *out, int maxLen)
{
	ensureInit();
	if (out == 0 || maxLen < 4) { return 0; }

	int n = 0;
	out[n++] = s_count;
	out[n++] = (uint8_t)(s_dropped >> 8);
	out[n++] = (uint8_t)(s_dropped & 0xFF);
	out[n++] = s_armed;

	for (int i = 0; i < s_count; i++)
	{
		if (n + 2 + DMR_RCTL_CAP_BURST_LEN > maxLen) { break; }  /* не вилізти за буфер USB */
		out[n++] = s_ring[i].seq;
		out[n++] = s_ring[i].type;
		memcpy(&out[n], s_ring[i].data, DMR_RCTL_CAP_BURST_LEN);
		n += DMR_RCTL_CAP_BURST_LEN;
	}
	return n;
}

#endif /* ENABLE_DMR_DATA && ENABLE_AES */

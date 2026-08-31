/* dmr_rctl_pdu.c — see dmr_rctl_pdu.h for the design/trust-model notes. */
#include "dmr_rctl_pdu.h"
#include <string.h>

#define RCTL_MAGIC0 ((uint8_t)'R')
#define RCTL_MAGIC1 ((uint8_t)'C')

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void dmr_rctl_pack(const dmr_rctl_msg_t *m, uint8_t out[DMR_RCTL_PDU_LEN])
{
    memset(out, 0, DMR_RCTL_PDU_LEN);
    out[0] = RCTL_MAGIC0;
    out[1] = RCTL_MAGIC1;
    out[2] = m->cmd;
    out[3] = 0; /* reserved */
    put_le32(out + 4,  m->issuerId);
    put_le32(out + 8,  m->seq);
    put_le32(out + 12, m->arg);
}

int dmr_rctl_unpack(const uint8_t in[DMR_RCTL_PDU_LEN], dmr_rctl_msg_t *out)
{
    if ((in[0] != RCTL_MAGIC0) || (in[1] != RCTL_MAGIC1)) { return 0; }
    out->cmd      = in[2];
    out->issuerId = get_le32(in + 4);
    out->seq      = get_le32(in + 8);
    out->arg      = get_le32(in + 12);
    return 1;
}

void dmr_rctl_gate_init(dmr_rctl_gate_t *g, uint8_t enabled, const uint32_t *allowedIds, uint8_t numAllowed)
{
    memset(g, 0, sizeof *g);
    g->enabled = enabled;
    if (numAllowed > DMR_RCTL_MAX_ALLOWED) { numAllowed = DMR_RCTL_MAX_ALLOWED; }
    g->numAllowed = numAllowed;
    for (uint8_t i = 0; i < numAllowed; i++) { g->allowedId[i] = allowedIds[i]; }
}

int dmr_rctl_gate_check(dmr_rctl_gate_t *g, uint32_t issuerId, uint32_t seq)
{
    /* Порожній/невизначений allowlist навмисно НЕ означає "дозволити всім" - лише сам ключ
     * шифрування не є достатнім доказом права видавати команди керування (на відміну від
     * голосу/SMS, де мета - зв'язок, а не примусова дія над чужою рацією). Enabled=1 без
     * жодного налаштованого ID означає "увімкнено, але ще нікому не довіряю". */
    if (!g->enabled || (g->numAllowed == 0)) { return 0; }

    int idx = -1;
    for (uint8_t i = 0; i < g->numAllowed; i++)
    {
        if (g->allowedId[i] == issuerId) { idx = (int)i; break; }
    }
    if (idx < 0) { return 0; }

    if (seq <= g->lastSeq[idx]) { return 0; } /* replay or out-of-order: reject, state untouched */

    g->lastSeq[idx] = seq;
    return 1;
}

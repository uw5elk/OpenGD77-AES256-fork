/* dmr_rctl_pdu.c — see dmr_rctl_pdu.h for the design/trust-model notes. */
#include "crypto/dmr_rctl_pdu.h"
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

void dmr_rctl_gate_init(dmr_rctl_gate_t *g, uint8_t enabled)
{
    memset(g, 0, sizeof *g);
    g->enabled = enabled;
}

int dmr_rctl_gate_check(dmr_rctl_gate_t *g, uint32_t issuerId, uint32_t seq)
{
    /* enabled=0 -> ніхто, enabled=1 -> будь-хто з правильним канальним ключем (див.
     * коментар у dmr_rctl_pdu.h -- модель узгоджена з користувачем 2026-09-03, за
     * зразком Motorola/Hytera: єдиний бінарний перемикач, без окремого allowlist). */
    if (!g->enabled) { return 0; }

    for (uint8_t i = 0; i < g->used; i++)
    {
        if (g->issuerId[i] == issuerId)
        {
            if (seq <= g->lastSeq[i]) { return 0; } /* replay or out-of-order: reject, state untouched */
            g->lastSeq[i] = seq;
            return 1;
        }
    }

    /* Новий видавець (ще немає запису в кеші anti-replay) -- перша команда від нього
     * завжди приймається, поки gate увімкнено; далі відстежуємо його seq окремо. */
    uint8_t slot;
    if (g->used < DMR_RCTL_REPLAY_CACHE)
    {
        slot = g->used++;
    }
    else
    {
        slot = g->nextEvict;
        g->nextEvict = (uint8_t)((g->nextEvict + 1) % DMR_RCTL_REPLAY_CACHE);
    }
    g->issuerId[slot] = issuerId;
    g->lastSeq[slot] = seq;
    return 1;
}

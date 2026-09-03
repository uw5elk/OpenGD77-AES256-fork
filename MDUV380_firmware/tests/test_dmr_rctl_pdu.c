/*
 * test_dmr_rctl_pdu.c — host unit tests for the remote-control PDU pack/unpack and the
 * enabled/disabled gate + anti-replay cache (pure logic, no STM32 dependencies).
 * Build & run on a PC (no radio needed):
 *     gcc -O2 -Wall -o test_dmr_rctl_pdu test_dmr_rctl_pdu.c dmr_rctl_pdu.c && ./test_dmr_rctl_pdu
 */
#include "dmr_rctl_pdu.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("  PASS %s\n", name); } \
    else { printf("  FAIL %s\n", name); fails++; } \
} while (0)

int main(void)
{
    printf("dmr_rctl_pdu self-test\n");

    /* 1) pack/unpack round-trip */
    dmr_rctl_msg_t m;
    m.cmd = DMR_RCTL_CMD_CHECK_REQ;
    m.issuerId = 0x123456;
    m.seq = 42;
    m.arg = 0;
    uint8_t buf[DMR_RCTL_PDU_LEN];
    dmr_rctl_pack(&m, buf);
    dmr_rctl_msg_t m2;
    int ok = dmr_rctl_unpack(buf, &m2);
    CHECK(ok == 1, "unpack recognizes valid magic");
    CHECK((m2.cmd == m.cmd) && (m2.issuerId == m.issuerId) && (m2.seq == m.seq) && (m2.arg == m.arg),
          "round-trip fields match");

    /* 2) a large 32-bit issuer/seq/arg round-trips correctly (no truncation) */
    dmr_rctl_msg_t m4;
    m4.cmd = DMR_RCTL_CMD_CHECK_ACK;
    m4.issuerId = 0xFFFFFFFFu;
    m4.seq = 0xDEADBEEFu;
    m4.arg = 0x80000001u;
    uint8_t buf4[DMR_RCTL_PDU_LEN];
    dmr_rctl_pack(&m4, buf4);
    dmr_rctl_msg_t m4b;
    dmr_rctl_unpack(buf4, &m4b);
    CHECK((m4b.issuerId == m4.issuerId) && (m4b.seq == m4.seq) && (m4b.arg == m4.arg),
          "32-bit fields round-trip without truncation");

    /* 3) corrupted magic is rejected (e.g. wrong AES key decrypted to noise) */
    uint8_t bad[DMR_RCTL_PDU_LEN];
    memcpy(bad, buf, sizeof bad);
    bad[0] = 0x00;
    dmr_rctl_msg_t m3;
    CHECK(dmr_rctl_unpack(bad, &m3) == 0, "unpack rejects bad magic");

    /* ---- gate: бінарна модель (2026-09-03) -- enabled=1 приймає БУДЬ-ЯКОГО нового
     * видавця, enabled=0 не приймає НІКОГО; anti-replay й далі per-issuer через кеш. */

    /* 4) disabled gate rejects everyone, even a never-seen issuer */
    dmr_rctl_gate_t g;
    dmr_rctl_gate_init(&g, 0);
    CHECK(dmr_rctl_gate_check(&g, 0x111111, 1) == 0, "disabled gate rejects any issuer");

    /* 5) enabled gate accepts ANY new issuer immediately (no pre-approval needed) */
    dmr_rctl_gate_t g2;
    dmr_rctl_gate_init(&g2, 1);
    CHECK(dmr_rctl_gate_check(&g2, 0x999999, 1) == 1, "enabled gate accepts a never-before-seen issuer");

    /* 6) enabled + strictly increasing seq from the same (now-tracked) issuer accepted */
    dmr_rctl_gate_t g3;
    dmr_rctl_gate_init(&g3, 1);
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 1) == 1, "accepts first command from a new issuer");
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 2) == 1, "accepts strictly increasing seq");

    /* 7) replay (same or lower seq) rejected once the issuer is tracked */
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 2) == 0, "rejects replayed seq (equal)");
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 1) == 0, "rejects replayed seq (lower)");

    /* 8) a different issuer is unaffected -- and, since it's new, accepted at any seq */
    CHECK(dmr_rctl_gate_check(&g3, 0x222222, 1) == 1, "second issuer starts its own seq stream");
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 3) == 1, "first issuer's stream unaffected by second's");

    /* 9) a rejected (replayed) attempt does not disturb the stored high-water mark */
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 3) == 0, "seq 3 now itself counts as replayed");
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 4) == 1, "next real seq after a replay attempt still accepted");

    /* 10) disabling mid-session (gate re-init, as dmrRctlConfigReload() would do) rejects again */
    dmr_rctl_gate_init(&g3, 0);
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 5) == 0, "re-init with enabled=0 rejects a previously-tracked issuer too");

    /* 11) replay cache: more than DMR_RCTL_REPLAY_CACHE distinct issuers safely evict
     * the oldest (round-robin) instead of overrunning the fixed-size arrays. */
    dmr_rctl_gate_t g4;
    dmr_rctl_gate_init(&g4, 1);
    for (uint32_t i = 0; i < DMR_RCTL_REPLAY_CACHE; i++)
    {
        CHECK(dmr_rctl_gate_check(&g4, 0x1000 + i, 1) == 1, "cache fills up to capacity without overrun");
    }
    CHECK(g4.used == DMR_RCTL_REPLAY_CACHE, "cache reports itself full at capacity");
    /* one more distinct issuer beyond capacity -- must not crash/overrun, evicts slot 0 */
    CHECK(dmr_rctl_gate_check(&g4, 0x1000 + DMR_RCTL_REPLAY_CACHE, 1) == 1,
          "(N+1)th distinct issuer accepted, evicting the oldest tracked entry");
    CHECK(g4.used == DMR_RCTL_REPLAY_CACHE, "cache size stays capped after eviction");
    /* the evicted issuer (0x1000) is now "forgotten" -- treated as new again, even at seq 1
     * (a documented trade-off of the bounded cache, not a bug: see dmr_rctl_pdu.h) */
    CHECK(dmr_rctl_gate_check(&g4, 0x1000, 1) == 1,
          "evicted issuer is re-admitted as 'new' (documented bounded-cache trade-off)");

    if (fails)
    {
        printf("\n%d FAILURES\n", fails);
    }
    else
    {
        printf("\nALL TESTS PASSED\n");
    }
    return fails ? 1 : 0;
}

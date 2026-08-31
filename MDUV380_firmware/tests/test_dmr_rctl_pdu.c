/*
 * test_dmr_rctl_pdu.c — host unit tests for the remote-control PDU pack/unpack and
 * allowlist/anti-replay gate (pure logic, no STM32 dependencies).
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

    /* 4) gate: disabled rejects everything, even an allowed issuer */
    dmr_rctl_gate_t g;
    uint32_t allow[2];
    allow[0] = 0x111111;
    allow[1] = 0x222222;
    dmr_rctl_gate_init(&g, 0, allow, 2);
    CHECK(dmr_rctl_gate_check(&g, 0x111111, 1) == 0, "disabled gate rejects allowed issuer");

    /* 5) gate: enabled + empty allowlist rejects everyone (no implicit trust-by-key) */
    dmr_rctl_gate_t g2;
    dmr_rctl_gate_init(&g2, 1, allow, 0);
    CHECK(dmr_rctl_gate_check(&g2, 0x111111, 1) == 0, "enabled+empty allowlist rejects everyone");

    /* 6) gate: enabled + allowlist accepts a listed issuer with increasing seq */
    dmr_rctl_gate_t g3;
    dmr_rctl_gate_init(&g3, 1, allow, 2);
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 1) == 1, "accepts first command from allowed issuer");
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 2) == 1, "accepts strictly increasing seq");

    /* 7) gate: replay (same or lower seq) rejected */
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 2) == 0, "rejects replayed seq (equal)");
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 1) == 0, "rejects replayed seq (lower)");

    /* 8) gate: issuer not in allowlist rejected */
    CHECK(dmr_rctl_gate_check(&g3, 0x999999, 1) == 0, "rejects issuer not on allowlist");

    /* 9) gate: two allowed issuers track sequence independently */
    CHECK(dmr_rctl_gate_check(&g3, 0x222222, 1) == 1, "second issuer starts its own seq stream");
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 3) == 1, "first issuer's stream unaffected by second's");

    /* 10) gate: a rejected (replayed) attempt does not disturb the stored high-water mark */
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 3) == 0, "seq 3 now itself counts as replayed");
    CHECK(dmr_rctl_gate_check(&g3, 0x111111, 4) == 1, "next real seq after a replay attempt still accepted");

    /* 11) allowlist longer than DMR_RCTL_MAX_ALLOWED is safely clamped, not overrun */
    uint32_t manyIds[16];
    for (int i = 0; i < 16; i++) { manyIds[i] = (uint32_t)(0x1000 + i); }
    dmr_rctl_gate_t g4;
    dmr_rctl_gate_init(&g4, 1, manyIds, 16);
    CHECK(g4.numAllowed == DMR_RCTL_MAX_ALLOWED, "oversized allowlist clamped to DMR_RCTL_MAX_ALLOWED");
    CHECK(dmr_rctl_gate_check(&g4, manyIds[0], 1) == 1, "first (kept) id after clamping still works");

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

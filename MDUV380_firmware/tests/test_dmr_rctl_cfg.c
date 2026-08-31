/*
 * test_dmr_rctl_cfg.c — хостові тести dmr_rctl_cfg.c проти mock_codeplug.c (без
 * STM32/SPI-флешу). Перевіряє саме "місток" (завантаження/парсинг блоку "RCTL",
 * fail-closed на відсутній/битий блок, reload) — сама allowlist/anti-replay логіка
 * вже перевірена окремо в test_dmr_rctl_pdu.c.
 *
 * Збірка й запуск на ПК:
 *   gcc -O2 -Wall -Wextra -DENABLE_AES -DENABLE_DMR_DATA \
 *       -I ../application/include \
 *       -o test_dmr_rctl_cfg test_dmr_rctl_cfg.c mock_codeplug.c \
 *       ../application/source/functions/dmr_rctl_cfg.c \
 *       ../application/source/crypto/dmr_rctl_pdu.c \
 *   && ./test_dmr_rctl_cfg
 */
#include "functions/dmr_rctl_cfg.h"
#include "mock_codeplug.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("  PASS %s\n", name); } \
    else { printf("  FAIL %s\n", name); fails++; } \
} while (0)

/* Той самий байтовий розклад, що dmr_rctl_cfg.c очікує у флеші: magic(4) + version(1)
 * + enabled(1) + numAllowed(1) + reserved(1) + allowedId[8]*4. Тест пакує його вручну,
 * а не через приватну структуру з .c-файлу — так тест ще й слугує документацією
 * реального формату блоку для CHIRP-сторони. */
static int pack_block(uint8_t *out, int outCap, uint8_t version, uint8_t enabled,
                       uint8_t numAllowed, const uint32_t *ids, int numIds)
{
    memset(out, 0, (size_t)outCap);
    out[0] = 'R'; out[1] = 'C'; out[2] = 'T'; out[3] = 'L';
    out[4] = version;
    out[5] = enabled;
    out[6] = numAllowed;
    out[7] = 0; /* reserved */
    int off = 8;
    for (int i = 0; i < numIds; i++)
    {
        out[off + 0] = (uint8_t)(ids[i] & 0xFF);
        out[off + 1] = (uint8_t)((ids[i] >> 8) & 0xFF);
        out[off + 2] = (uint8_t)((ids[i] >> 16) & 0xFF);
        out[off + 3] = (uint8_t)((ids[i] >> 24) & 0xFF);
        off += 4;
    }
    return off;
}

int main(void)
{
    printf("dmr_rctl_cfg self-test\n");
    uint8_t buf[64];
    int len;

    /* 1) немає жодного блоку -> fail closed: вимкнено, gate теж вимкнений/порожній */
    mock_codeplug_clear();
    CHECK(dmrRctlConfigEnabled() == 0, "no block -> config disabled");
    dmr_rctl_gate_t *g1 = dmrRctlGate();
    CHECK(g1->enabled == 0, "no block -> gate.enabled == 0");
    CHECK(g1->numAllowed == 0, "no block -> gate.numAllowed == 0");
    CHECK(dmr_rctl_gate_check(g1, 0x111111, 1) == 0, "no block -> gate rejects everyone");

    /* 2) валідний блок: enabled=1, два ID */
    dmrRctlConfigReload();
    uint32_t ids[2] = { 0x111111u, 0x222222u };
    len = pack_block(buf, (int)sizeof buf, 1, 1, 2, ids, 2);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    CHECK(dmrRctlConfigEnabled() == 1, "valid enabled block -> config enabled");
    dmr_rctl_gate_t *g2 = dmrRctlGate();
    CHECK(g2->numAllowed == 2, "valid block -> numAllowed parsed correctly");
    CHECK(dmr_rctl_gate_check(g2, 0x111111u, 1) == 1, "valid block -> allowed issuer accepted");
    CHECK(dmr_rctl_gate_check(g2, 0x999999u, 1) == 0, "valid block -> issuer not on list rejected");
    CHECK(dmr_rctl_gate_check(g2, 0x111111u, 1) == 0, "valid block -> replay of seq 1 rejected");

    /* 3) той самий блок, але enabled=0 -> config/gate вимкнені, попри непорожній список */
    dmrRctlConfigReload();
    len = pack_block(buf, (int)sizeof buf, 1, 0, 2, ids, 2);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    CHECK(dmrRctlConfigEnabled() == 0, "enabled=0 in block -> config disabled despite non-empty list");
    dmr_rctl_gate_t *g3 = dmrRctlGate();
    CHECK(dmr_rctl_gate_check(g3, 0x111111u, 1) == 0, "enabled=0 in block -> gate rejects allowed issuer too");

    /* 4) побита магічна мітка -> трактується як відсутній блок (fail closed) */
    dmrRctlConfigReload();
    len = pack_block(buf, (int)sizeof buf, 1, 1, 2, ids, 2);
    buf[0] = 0x00; /* псуємо магію */
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    CHECK(dmrRctlConfigEnabled() == 0, "corrupt magic -> config disabled");

    /* 5) numAllowed у флеші перевищує DMR_RCTL_MAX_ALLOWED (побитий/чужий блок) ->
     *    безпечно обрізається gate-логікою, без виходу за межі allowedId[8]. */
    dmrRctlConfigReload();
    uint32_t manyIds[8] = { 0x1000, 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007 };
    len = pack_block(buf, (int)sizeof buf, 1, 1, 250 /* явно битий numAllowed */, manyIds, 8);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    dmr_rctl_gate_t *g5 = dmrRctlGate();
    CHECK(g5->numAllowed == DMR_RCTL_MAX_ALLOWED, "corrupt oversized numAllowed clamped, no overrun");
    CHECK(dmr_rctl_gate_check(g5, manyIds[0], 1) == 1, "first id after clamping still usable");

    /* 6) короткий (старіший/урізаний) блок: лише до numAllowed, без масиву ID -> поля
     *    за межею фактично прочитаних байт лишаються нульовими (той самий bounded-read
     *    захист, що в MSGC dmr_sms.c), а не сміттям із попереднього тесту. */
    dmrRctlConfigReload();
    uint8_t shortBuf[8];
    shortBuf[0] = 'R'; shortBuf[1] = 'C'; shortBuf[2] = 'T'; shortBuf[3] = 'L';
    shortBuf[4] = 1;   /* version */
    shortBuf[5] = 1;   /* enabled */
    shortBuf[6] = 1;   /* numAllowed каже "1", але масиву ID в переданих байтах нема */
    shortBuf[7] = 0;
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, shortBuf, (int)sizeof shortBuf);
    dmr_rctl_gate_t *g6 = dmrRctlGate();
    CHECK(g6->numAllowed == 1, "truncated block -> numAllowed still parsed");
    CHECK(g6->allowedId[0] == 0, "truncated block -> missing allowedId defaults to 0, not garbage");

    /* 7) reload підхоплює новий блок замість кешованого попереднього стану */
    dmrRctlConfigReload();
    uint32_t oneId[1] = { 0x333333u };
    len = pack_block(buf, (int)sizeof buf, 1, 1, 1, oneId, 1);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    dmr_rctl_gate_t *g7a = dmrRctlGate();
    CHECK(dmr_rctl_gate_check(g7a, 0x333333u, 1) == 1, "before reload: new issuer accepted");
    /* тепер "CHIRP переписує кодплаг" на інший список без reload -> кеш ще старий */
    uint32_t otherId[1] = { 0x444444u };
    len = pack_block(buf, (int)sizeof buf, 1, 1, 1, otherId, 1);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    CHECK(dmrRctlConfigEnabled() == 1, "stale cache still reports enabled (not yet reloaded)");
    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x444444u, 1) == 0, "stale cache: new issuer not yet visible");
    dmrRctlConfigReload();
    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x444444u, 1) == 1, "after reload: new issuer visible");
    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x333333u, 1) == 0, "after reload: old issuer no longer allowed");

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

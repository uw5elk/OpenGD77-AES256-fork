/*
 * test_dmr_rctl_cfg.c — хостові тести dmr_rctl_cfg.c проти mock_codeplug.c (без
 * STM32/SPI-флешу). Перевіряє саме "місток" (завантаження/парсинг блоку "RCTL",
 * fail-closed на відсутній/битий блок, reload) — сама enabled/anti-replay логіка
 * вже перевірена окремо в test_dmr_rctl_pdu.c.
 *
 * Формат блоку ВИПРАВЛЕНО 2026-09-03 (пряма вказівка користувача, за зразком
 * Motorola/Hytera): без окремого allowlist, лише magic+version+enabled+reserved (8 Б).
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
 * + enabled(1) + reserved(2) = 8 Б. Тест пакує його вручну, а не через приватну
 * структуру з .c-файлу — так тест ще й слугує документацією реального формату блоку
 * для CHIRP/PC-сторони (tools/rctl_config.py). */
static int pack_block(uint8_t *out, int outCap, uint8_t version, uint8_t enabled)
{
    memset(out, 0, (size_t)outCap);
    out[0] = 'R'; out[1] = 'C'; out[2] = 'T'; out[3] = 'L';
    out[4] = version;
    out[5] = enabled;
    out[6] = 0; /* reserved */
    out[7] = 0; /* reserved */
    return 8;
}

int main(void)
{
    printf("dmr_rctl_cfg self-test\n");
    uint8_t buf[64];
    int len;

    /* 1) немає жодного блоку -> fail closed: вимкнено, gate теж вимкнений */
    mock_codeplug_clear();
    CHECK(dmrRctlConfigEnabled() == 0, "no block -> config disabled");
    dmr_rctl_gate_t *g1 = dmrRctlGate();
    CHECK(g1->enabled == 0, "no block -> gate.enabled == 0");
    CHECK(dmr_rctl_gate_check(g1, 0x111111, 1) == 0, "no block -> gate rejects everyone");

    /* 2) валідний блок: enabled=1 -> БУДЬ-ЯКИЙ новий видавець приймається одразу */
    dmrRctlConfigReload();
    len = pack_block(buf, (int)sizeof buf, 2, 1);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    CHECK(dmrRctlConfigEnabled() == 1, "valid enabled block -> config enabled");
    dmr_rctl_gate_t *g2 = dmrRctlGate();
    CHECK(dmr_rctl_gate_check(g2, 0x111111u, 1) == 1, "valid enabled block -> any new issuer accepted");
    CHECK(dmr_rctl_gate_check(g2, 0x999999u, 1) == 1, "valid enabled block -> a DIFFERENT new issuer also accepted (no allowlist)");
    CHECK(dmr_rctl_gate_check(g2, 0x111111u, 1) == 0, "valid enabled block -> replay of seq 1 (same issuer) rejected");

    /* 3) той самий блок, але enabled=0 -> config/gate вимкнені для всіх */
    dmrRctlConfigReload();
    len = pack_block(buf, (int)sizeof buf, 2, 0);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    CHECK(dmrRctlConfigEnabled() == 0, "enabled=0 in block -> config disabled");
    dmr_rctl_gate_t *g3 = dmrRctlGate();
    CHECK(dmr_rctl_gate_check(g3, 0x111111u, 1) == 0, "enabled=0 in block -> gate rejects everyone");

    /* 4) побита магічна мітка -> трактується як відсутній блок (fail closed) */
    dmrRctlConfigReload();
    len = pack_block(buf, (int)sizeof buf, 2, 1);
    buf[0] = 0x00; /* псуємо магію */
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    CHECK(dmrRctlConfigEnabled() == 0, "corrupt magic -> config disabled");

    /* 5) короткий (урізаний) блок: лише magic+version, без байта enabled -> той самий
     *    bounded-read захист, що в MSGC dmr_sms.c -- поле за межею фактично прочитаних
     *    байт лишається нулем (fail closed), а не сміттям із попереднього тесту. */
    dmrRctlConfigReload();
    uint8_t shortBuf[5];
    shortBuf[0] = 'R'; shortBuf[1] = 'C'; shortBuf[2] = 'T'; shortBuf[3] = 'L';
    shortBuf[4] = 2;   /* version; байта enabled фізично немає в переданих даних */
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, shortBuf, (int)sizeof shortBuf);
    CHECK(dmrRctlConfigEnabled() == 0, "truncated block (no enabled byte) -> defaults to disabled, not garbage");

    /* 6) reload підхоплює новий стан замість кешованого попереднього */
    dmrRctlConfigReload();
    len = pack_block(buf, (int)sizeof buf, 2, 1);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x333333u, 1) == 1, "before reload: enabled, issuer accepted");
    /* тепер "CHIRP/PC-утиліта переписує кодплаг" на enabled=0 без reload -> кеш ще старий */
    len = pack_block(buf, (int)sizeof buf, 2, 0);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    CHECK(dmrRctlConfigEnabled() == 1, "stale cache still reports enabled (not yet reloaded)");
    dmrRctlConfigReload();
    CHECK(dmrRctlConfigEnabled() == 0, "after reload: disabled state visible");
    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x333333u, 1) == 0, "after reload: previously-tracked issuer now rejected too");

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

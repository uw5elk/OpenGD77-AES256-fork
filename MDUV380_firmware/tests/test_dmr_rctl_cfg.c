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

    /* 7) dmrRctlConfigSetEnabled() -- новий шлях запису ПРЯМО З РАЦІЇ (menuRCTLConfig.c),
     *    а не лише з ПК/CHIRP. Перевіряємо round-trip через справжній мок-флеш (не лише
     *    повернене значення), і що ефект видно одразу (setter сам викликає reload). */
    mock_codeplug_clear();
    CHECK(dmrRctlConfigEnabled() == 0, "T7: до запису -- вимкнено (блоку ще немає)");
    CHECK(dmrRctlConfigSetEnabled(1) == 1, "T7: запис enabled=1 з \"рації\" вдався");
    CHECK(dmrRctlConfigEnabled() == 1, "T7: одразу після запису -- увімкнено (без ручного reload)");
    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x444444u, 1) == 1, "T7: після запису -- gate приймає нового видавця");
    CHECK(mock_codeplug_write_count() == 1, "T7: рівно один фізичний запис у флеш");

    uint8_t rb[8];
    CHECK(codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, rb, (int)sizeof rb), "T7: блок читається назад");
    /* Формат ВЕРСІЇ 3 (2026-09-05): другий reserved-байт став маскою дозволів allow,
     * розмір блока лишився 8 Б. Тест і далі слугує документацією формату для CPS-сторони
     * (tools/rctl_config.py): magic(4) + version(1) + enabled(1) + allow(1) + reserved(1). */
    CHECK(memcmp(rb, "RCTL", 4) == 0 && rb[4] == 4 && rb[5] == 1 && rb[7] == 0,
          "T7: записаний блок відповідає формату v4 magic+version+enabled+allow+monitorSecs (тут 0=типова)");

    CHECK(dmrRctlConfigSetEnabled(0) == 1, "T7: повторний запис enabled=0 вдався (оновлення того самого блоку)");
    CHECK(dmrRctlConfigEnabled() == 0, "T7: після другого запису -- знову вимкнено");
    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x444444u, 2) == 0, "T7: вимкнено -- той самий видавець більше не проходить");
    CHECK(mock_codeplug_write_count() == 2, "T7: другий виклик -- ще один фізичний запис (оновлення блоку, не новий)");

    /* ================= T8: anti-replay ПЕРЕЖИВАЄ ПЕРЕЗАВАНТАЖЕННЯ =================
     *
     * Це головна перевірка виправлення 2026-09-04. До нього кеш жив лише в ОЗП, тож
     * записаний з ефіру кадр проходив повторно щоразу після ввімкнення рації, і рація
     * АВТОМАТИЧНО виходила в ефір з відповіддю -- готовий пеленг для супротивника.
     *
     * "Перезавантаження" тут -- dmrRctlConfigReload(): ОЗП чисте, вміст мок-флешу
     * лишається. Саме те, що відбувається при ввімкненні живлення. */
    mock_codeplug_clear();
    len = pack_block(buf, (int)sizeof buf, 2, 1);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    dmrRctlConfigReload();

    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x777777u, 100) == 1, "T8: перша команда від видавця приймається");
    CHECK(dmrRctlGatePersist() == 1, "T8: лічильники записано у флеш");

    dmrRctlConfigReload();   /* <-- ПЕРЕЗАВАНТАЖЕННЯ рації */

    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x777777u, 100) == 0,
          "T8: ТОЙ САМИЙ кадр після перезавантаження ВІДХИЛЕНО (replay закрито)");
    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x777777u, 99) == 0,
          "T8: старіший кадр після перезавантаження теж відхилено");
    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x777777u, 101) == 1,
          "T8: НОВІША команда від того самого видавця проходить (зв'язок не зламано)");

    /* ================= T9: номер відправки монотонний через перезавантаження =======
     *
     * Без цього збереження лічильників на ЦІЛІ перетворило б дірку в безпеці на повну
     * відмову зв'язку: запитувач після свого перезавантаження починав би з малого
     * номера, а ціль пам'ятала б високий -- і відхиляла б його НАЗАВЖДИ. */
    mock_codeplug_clear();
    dmrRctlConfigReload();

    uint32_t a = dmrRctlNextTxSeq();
    uint32_t b = dmrRctlNextTxSeq();
    CHECK((a != 0) && (b == a + 1), "T9: номери зростають на 1");

    dmrRctlConfigReload();   /* <-- ПЕРЕЗАВАНТАЖЕННЯ запитувача */

    uint32_t c = dmrRctlNextTxSeq();
    CHECK(c > b, "T9: після перезавантаження номер БІЛЬШИЙ за виданий до нього");

    /* ================= T10: запис у флеш пачками, а не на кожну команду ============ */
    mock_codeplug_clear();
    dmrRctlConfigReload();
    (void)dmrRctlNextTxSeq();                 /* перший виклик резервує діапазон -> 1 запис */
    int writesAfterFirst = mock_codeplug_write_count();
    for (int i = 0; i < 30; i++) { (void)dmrRctlNextTxSeq(); }
    CHECK(writesAfterFirst == 1, "T10: перший номер коштує рівно одного запису у флеш");
    CHECK(mock_codeplug_write_count() == 1, "T10: наступні 30 номерів не пишуть у флеш узагалі");

    /* ================= T11: побитий блок стану -> порожній кеш, без падіння ======== */
    mock_codeplug_clear();
    len = pack_block(buf, (int)sizeof buf, 2, 1);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    {
        uint8_t junk[76];
        memset(junk, 0xA5, sizeof junk);       /* немає magic "RCTS" */
        mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_STATE, junk, (int)sizeof junk);
    }
    dmrRctlConfigReload();
    CHECK(dmr_rctl_gate_check(dmrRctlGate(), 0x888888u, 7) == 1,
          "T11: побитий блок стану -> кеш порожній, робота триває");
    CHECK(dmrRctlNextTxSeq() != 0, "T11: побитий блок стану -> номер відправки все одно видається");

    /* ============ T12-T16: ОКРЕМІ ДОЗВОЛИ НА КОЖНУ КОМАНДУ (2026-09-05) ==========
     *
     * Модель за зразком Motorola/Hytera: доступ має будь-яка станція мережі, і на
     * ВІДКРИТОМУ каналі теж, а вирішує цільова рація -- окремим дозволом на кожну команду.
     * На відкритому каналі це єдина межа: підтвердити відправника там нічим. */

    /* T12: свіжий блок -> не дозволено НІЧОГО, навіть при увімкненому доступі */
    mock_codeplug_clear();
    dmrRctlConfigReload();
    CHECK(dmrRctlConfigSetEnabled(1) == 1, "T12: доступ увімкнено");
    CHECK(dmrRctlAllowMask() == 0, "T12: свіжий блок -- маска порожня");
    CHECK(dmrRctlCommandAllowed(DMR_RCTL_CMD_CHECK_REQ) == 0, "T12: радіоперевірка ЗАБОРОНЕНА за замовчуванням");

    /* T13: вмикаємо лише радіоперевірку -- решта лишається забороненою */
    CHECK(dmrRctlConfigSetAllow(DMR_RCTL_ALLOW_CHECK) == 1, "T13: маску записано");
    CHECK(dmrRctlCommandAllowed(DMR_RCTL_CMD_CHECK_REQ) == 1, "T13: радіоперевірка дозволена");
    CHECK(dmrRctlCommandAllowed(DMR_RCTL_CMD_STUN) == 0, "T13: блокування НЕ дозволене");
    CHECK(dmrRctlCommandAllowed(DMR_RCTL_CMD_MONITOR_START) == 0, "T13: прослуховування НЕ дозволене");
    CHECK(dmrRctlCommandAllowed(DMR_RCTL_CMD_REVIVE) == 0, "T13: розблокування НЕ дозволене");

    /* T14: головний перемикач перекриває все, але виставлені біти НЕ губляться */
    CHECK(dmrRctlConfigSetEnabled(0) == 1, "T14: доступ вимкнено");
    CHECK(dmrRctlCommandAllowed(DMR_RCTL_CMD_CHECK_REQ) == 0, "T14: вимкнений доступ забороняє навіть дозволену команду");
    CHECK(dmrRctlAllowMask() == 0, "T14: робоча маска порожня, поки доступ вимкнено");
    CHECK(dmrRctlConfigAllowRaw() == DMR_RCTL_ALLOW_CHECK, "T14: але сира маска збережена (меню не втратить налаштування)");

    /* T15: старий блок версії 2 -> лише радіоперевірка, а не всі права.
     * Оновлення прошивки не має мовчки роздавати дозволи, яких власник не вмикав. */
    mock_codeplug_clear();
    len = pack_block(buf, (int)sizeof buf, 2, 1);
    mock_codeplug_set_block(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, buf, len);
    dmrRctlConfigReload();
    CHECK(dmrRctlCommandAllowed(DMR_RCTL_CMD_CHECK_REQ) == 1, "T15: блок v2 -> радіоперевірка дозволена");
    CHECK(dmrRctlCommandAllowed(DMR_RCTL_CMD_STUN) == 0, "T15: блок v2 -> блокування НЕ дозволене");
    CHECK(dmrRctlCommandAllowed(DMR_RCTL_CMD_MONITOR_START) == 0, "T15: блок v2 -> прослуховування НЕ дозволене");

    /* T16: невідомий код команди -> заборонено (fail closed) */
    mock_codeplug_clear();
    dmrRctlConfigReload();
    dmrRctlConfigSetEnabled(1);
    dmrRctlConfigSetAllow(DMR_RCTL_ALLOW_ALL);
    CHECK(dmrRctlCommandAllowed(DMR_RCTL_CMD_CHECK_REQ) == 1, "T16: при повній масці перевірка дозволена");
    CHECK(dmrRctlCommandAllowed(99) == 0, "T16: невідома команда заборонена навіть при повній масці");

    /* ================= T17: тривалість моніторингу (версія 4) ================= */
    mock_codeplug_clear();
    CHECK(dmrRctlMonitorSecs() == 30, "T17: без блока -> типова 30 с");
    CHECK(dmrRctlConfigSetMonitorSecs(60) == 1, "T17: запис 60 с вдався");
    CHECK(dmrRctlMonitorSecs() == 60, "T17: після запису -> 60 с (без reload)");
    CHECK(dmrRctlConfigSetMonitorSecs(200) == 1, "T17: запис понад межу вдався");
    CHECK(dmrRctlMonitorSecs() == 120, "T17: понад межу -> обрізано до 120 с");
    dmrRctlConfigSetAllow(DMR_RCTL_ALLOW_MONITOR);
    CHECK(dmrRctlMonitorSecs() == 120, "T17: запис дозволів не скинув тривалість");
    CHECK((dmrRctlConfigAllowRaw() & DMR_RCTL_ALLOW_MONITOR) != 0, "T17: запис тривалості не скинув дозволи");

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

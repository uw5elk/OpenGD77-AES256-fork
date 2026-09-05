/*
 * test_dmr_rctl_frame.c — хостові тести dmr_rctl_frame.c (чисте побудова TX-кадру RCTL,
 * без STM32-залежностей). Перевіряє, що зібрана черга бургстів дає узгоджений ефірний
 * кадр: правильна кількість/типи бургстів, коректна адресація в CSBK-преамбулі, а
 * головне -- що PDU, зібраний з двох rate-1/2-блоків, чисто проходить CRC32 і
 * розшифровується/розпаковується назад у вихідне повідомлення (round-trip).
 *
 * Збірка й запуск на ПК (з каталогу tests/):
 *   gcc -O2 -Wall -Wextra -I ../application/include \
 *       -o test_dmr_rctl_frame test_dmr_rctl_frame.c \
 *       ../application/source/crypto/dmr_rctl_frame.c \
 *       ../application/source/crypto/dmr_rctl_pdu.c \
 *       ../application/source/crypto/dmr_aes.c \
 *       && ./test_dmr_rctl_frame
 */
#include "crypto/dmr_rctl_frame.h"
#include "crypto/dmr_rctl_pdu.h"
#include "crypto/dmr_aes.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("  PASS %s\n", name); } \
    else { printf("  FAIL %s\n", name); fails++; } \
} while (0)

int main(void)
{
    printf("dmr_rctl_frame self-test\n");

    uint8_t key[32];
    for (int i = 0; i < 32; i++) { key[i] = (uint8_t)(i * 7 + 3); }

    dmr_rctl_msg_t m;
    m.cmd = DMR_RCTL_CMD_CHECK_REQ;
    m.issuerId = 0xABCDEF;
    m.seq = 12345;
    m.arg = 0;

    uint32_t dst = 0x112233;
    uint32_t src = 0xABCDEF;
    uint8_t  keyId = 3;

    uint8_t q[DMR_RCTL_TX_BURST_COUNT * 13];
    int n = dmr_rctl_build_tx_bursts(dst, src, keyId, key, &m, q);
    CHECK(n == DMR_RCTL_TX_BURST_COUNT, "burst count == DMR_RCTL_TX_BURST_COUNT (10)");

    /* burst 0..5: CSBK preamble, addressed to dst/src, individual (not group) call */
    for (int i = 0; i < 6; i++)
    {
        const uint8_t *b = q + i * 13;
        char name[64]; snprintf(name, sizeof name, "preamble[%d] type == CSBK (0x30)", i);
        CHECK(b[0] == 0x30, name);
        uint32_t bdst = ((uint32_t)b[1 + 4] << 16) | ((uint32_t)b[1 + 5] << 8) | b[1 + 6];
        uint32_t bsrc = ((uint32_t)b[1 + 7] << 16) | ((uint32_t)b[1 + 8] << 8) | b[1 + 9];
        snprintf(name, sizeof name, "preamble[%d] dst matches target", i);
        CHECK(bdst == dst, name);
        snprintf(name, sizeof name, "preamble[%d] src matches issuer", i);
        CHECK(bsrc == src, name);
        snprintf(name, sizeof name, "preamble[%d] group-call bit clear (individual)", i);
        CHECK((b[1 + 2] & 0xC0) != 0xC0, name);
    }
    CHECK(q[6 * 13 + 0] == 0x60, "burst 6 type == Data Header (0x60, Unconfirmed)");
    CHECK(q[7 * 13 + 0] == 0x60, "burst 7 type == Data Header (0x60, ENC ext)");
    CHECK(q[7 * 13 + 1 + 3] == keyId, "ENC ext header carries keyId");
    CHECK(q[8 * 13 + 0] == 0x70, "burst 8 type == Rate-1/2 (0x70)");
    CHECK(q[9 * 13 + 0] == 0x70, "burst 9 type == Rate-1/2 (0x70)");

    /* reassemble the 24-byte wire PDU from the two rate-1/2 blocks and round-trip it */
    uint8_t pdu[24];
    memcpy(pdu, q + 8 * 13 + 1, 12);
    memcpy(pdu + 12, q + 9 * 13 + 1, 12);

    uint32_t want = ((uint32_t)pdu[20] << 24) | ((uint32_t)pdu[21] << 16) |
                    ((uint32_t)pdu[22] << 8) | (uint32_t)pdu[23];
    CHECK(dmr_rctl_crc32(pdu, 24) == want, "reassembled PDU CRC32 valid");

    uint8_t ct[16];
    memcpy(ct, pdu, 16);
    aes256_ecb_decrypt(key, ct);
    dmr_rctl_msg_t m2;
    int ok = dmr_rctl_unpack(ct, &m2);
    CHECK(ok == 1, "decrypted block unpacks (magic 'RC' recognised)");
    CHECK(m2.cmd == m.cmd && m2.issuerId == m.issuerId && m2.seq == m.seq && m2.arg == m.arg,
          "round-trip fields match the original message");

    /* wrong key must NOT reproduce the same fields (sanity: encryption actually matters) */
    uint8_t wrongKey[32];
    memcpy(wrongKey, key, 32);
    wrongKey[0] ^= 0xFF;
    uint8_t ct2[16];
    memcpy(ct2, pdu, 16);
    aes256_ecb_decrypt(wrongKey, ct2);
    dmr_rctl_msg_t m3;
    int ok2 = dmr_rctl_unpack(ct2, &m3);
    CHECK(!(ok2 == 1 && m3.issuerId == m.issuerId && m3.seq == m.seq), "wrong key does not reproduce the message");

    /* ===== ВІДКРИТИЙ КАНАЛ: key == NULL більше не помилка (2026-09-05) =====
     *
     * Раніше тут перевірялось `nNull == -1`: без ключа кадр не будувався взагалі, тобто на
     * каналі без шифрування віддалене керування не працювало. Вимога змінилась -- як у
     * Motorola/Hytera, команди мають ходити і у відкритій мережі, а доступ вирішує цільова
     * рація своїми дозволами, а не наявність шифру.
     *
     * Відкритий кадр НЕ несе ENC-заголовка, тому бургстів на один менше, і саме за цим
     * приймач розрізняє, розшифровувати кадр чи читати як є. */
    uint8_t qc[DMR_RCTL_TX_BURST_COUNT * 13];
    int nClear = dmr_rctl_build_tx_bursts(dst, src, keyId, NULL, &m, qc);
    CHECK(nClear == (DMR_RCTL_TX_BURST_COUNT - 1), "відкритий кадр: на один бургст менше (без ENC-заголовка)");
    CHECK(qc[6 * 13 + 0] == 0x60, "відкритий: бургст 6 -- Data Header");
    CHECK(qc[7 * 13 + 0] == 0x70, "відкритий: бургст 7 -- одразу Rate-1/2, ENC-заголовка немає");
    CHECK(qc[8 * 13 + 0] == 0x70, "відкритий: бургст 8 -- Rate-1/2");

    uint8_t pduC[24];
    memcpy(pduC, qc + 7 * 13 + 1, 12);
    memcpy(pduC + 12, qc + 8 * 13 + 1, 12);
    uint32_t wantC = ((uint32_t)pduC[20] << 24) | ((uint32_t)pduC[21] << 16) |
                     ((uint32_t)pduC[22] << 8) | (uint32_t)pduC[23];
    CHECK(dmr_rctl_crc32(pduC, 24) == wantC, "відкритий: CRC32 зібраного PDU правильний");

    dmr_rctl_msg_t m4;
    CHECK(dmr_rctl_unpack(pduC, &m4) == 1, "відкритий: PDU читається БЕЗ розшифровки");
    CHECK(m4.cmd == m.cmd && m4.issuerId == m.issuerId && m4.seq == m.seq && m4.arg == m.arg,
          "відкритий: поля збігаються з вихідним повідомленням");

    /* І навпаки: шифрований кадр НЕ має читатись як відкритий -- інакше приймач плутав би
     * режими, а ключ не давав би нічого. */
    dmr_rctl_msg_t m5;
    CHECK(dmr_rctl_unpack(pdu, &m5) == 0, "шифрований кадр не читається як відкритий");

    printf(fails == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", fails);
    return fails ? 1 : 0;
}

/*
 * dmr_rctl_pdu.h — pure PDU pack/unpack + on/off gate + anti-replay for the "remote
 * control" idea in PLANS.md section 3 (radio check / remote monitor / stun-revive).
 *
 * Deliberately has NO STM32/HR-C6000/OpenGD77 dependency, so it can be built and
 * tested on a host exactly like crypto/dmr_aes.c (see tests/test_dmr_rctl_pdu.c).
 * The radio-side glue (loading the "RCTL" custom-data config block, keying a
 * CSBK-led data call via dmrDataTxLoad(), wiring the RX burst hook) lives in
 * functions/dmr_rctl_tx.c / dmr_rctl_cfg.c.
 *
 * Модель довіри (ВИПРАВЛЕНО 2026-09-03, за прямою вказівкою користувача: "якщо
 * ввімкнено -- можуть керувати всі, якщо ні -- то ніхто, так роблять і Motorola,
 * і Hytera"). Замінює попередню (2026-08-31) модель з окремим per-issuer
 * allowlist -- та зайва: єдина реальна межа довіри тут і так той самий AES-ключ
 * каналу, що й голос/SMS, а не окремий список дозволених ID:
 *   - command PDU шифрується AES-256-ECB тим самим ключем, що й голос на каналі
 *     (caller сам шифрує/розшифровує -- цей файл працює лише з 16-байтним
 *     розшифрованим plaintext);
 *   - `enabled` -- єдиний перемикач: 1 = приймати команди від БУДЬ-ЯКОГО видавця
 *     (тобто від будь-кого, хто підібрав/має правильний канальний ключ), 0 =
 *     не приймати НІ ВІД КОГО. Немає окремого списку "довірених ID";
 *   - anti-replay лишається, але тепер per-issuer в межах невеликого кешу
 *     "нещодавно бачених видавців" (DMR_RCTL_REPLAY_CACHE слотів), а не
 *     прив'язаний до конфігурованого allowlist -- перший запит від НОВОГО
 *     видавця завжди приймається (за enabled=1), наступні від того самого
 *     видавця мають нести строго більший seq. При переповненні кешу
 *     найстаріший запис витісняється по колу (round-robin) -- це свідомий
 *     компроміс: видавець, витіснений із кешу, при повторній появі знову
 *     трактується як "новий", тож дуже старий записаний-і-повторно-надісланий
 *     кадр від НЬОГО міг би пройти ще раз. Для невеликого флоту (кеш
 *     розрахований на кілька одночасно активних видавців команд) це прийнятно.
 */
#ifndef DMR_RCTL_PDU_H
#define DMR_RCTL_PDU_H

#include <stdint.h>

#define DMR_RCTL_PDU_LEN          16   /* exactly one AES-256-ECB block: no padding logic needed */
#define DMR_RCTL_REPLAY_CACHE      8   /* скільки РІЗНИХ видавців одночасно відстежуємо проти replay */

typedef enum
{
    DMR_RCTL_CMD_CHECK_REQ    = 1,  /* "чи ти тут?" - ціль одразу відповідає CHECK_ACK */
    DMR_RCTL_CMD_CHECK_ACK    = 2,
    DMR_RCTL_CMD_MONITOR_START = 3, /* НЕ реалізовано - див. застереження в PLANS.md */
    DMR_RCTL_CMD_MONITOR_STOP  = 4, /* НЕ реалізовано */
    DMR_RCTL_CMD_STUN          = 5, /* НЕ реалізовано */
    DMR_RCTL_CMD_REVIVE        = 6, /* НЕ реалізовано */
} dmr_rctl_cmd_t;

/* Дозволи на КОЖНУ команду окремо (2026-09-05, за зразком Motorola/Hytera, де радіоперевірка,
 * прослуховування, блокування й розблокування вмикаються незалежно одне від одного).
 *
 * Це НЕ те саме, що шифрування. Ключ каналу вирішує, чи команда взагалі читається; ці біти
 * вирішують, чи рація погоджується її ВИКОНАТИ. На відкритому каналі, де ключа немає й
 * підтвердити відправника нічим, вони лишаються єдиною межею -- тому вимкнене за
 * замовчуванням має бути все, крім того, що свідомо ввімкнули. */
#define DMR_RCTL_ALLOW_CHECK    (1u << 0)   /* радіоперевірка (відповісти "я тут") */
#define DMR_RCTL_ALLOW_MONITOR  (1u << 1)   /* віддалене прослуховування -- НЕ реалізовано */
#define DMR_RCTL_ALLOW_STUN     (1u << 2)   /* блокування рації -- НЕ реалізовано */
#define DMR_RCTL_ALLOW_REVIVE   (1u << 3)   /* розблокування рації -- НЕ реалізовано */
#define DMR_RCTL_ALLOW_ALL      (DMR_RCTL_ALLOW_CHECK | DMR_RCTL_ALLOW_MONITOR | \
                                 DMR_RCTL_ALLOW_STUN | DMR_RCTL_ALLOW_REVIVE)

typedef struct
{
    uint8_t  cmd;       /* dmr_rctl_cmd_t */
    uint32_t issuerId;  /* DMR ID видавця команди */
    uint32_t seq;       /* монотонний лічильник видавця - захист від replay */
    uint32_t arg;       /* корисне навантаження, залежне від cmd (напр. заряд/RSSI в CHECK_ACK) */
} dmr_rctl_msg_t;

/* Pack the 16-byte plaintext PDU. Caller does the AES-256-ECB encrypt afterwards
 * (same key/helper as dmr_sms.c uses for its PDUs). */
void dmr_rctl_pack(const dmr_rctl_msg_t *m, uint8_t out16[DMR_RCTL_PDU_LEN]);

/* Unpack an already-decrypted 16-byte PDU. Returns 1 on a structurally valid PDU
 * (magic bytes match), 0 otherwise (garbage / wrong key produced noise). Does NOT
 * check `enabled` or the replay cache - call dmr_rctl_gate_check() for that. */
int dmr_rctl_unpack(const uint8_t in16[DMR_RCTL_PDU_LEN], dmr_rctl_msg_t *out);

/* ---- увімк/вимк + захист від replay ---------------------------------------- */
typedef struct
{
    uint8_t  enabled;                              /* 0 = приймання команд вимкнено (default) */
    uint8_t  used;                                  /* скільки слотів кешу зайнято (0..CACHE) */
    uint8_t  nextEvict;                             /* round-robin індекс витіснення при заповненні */
    uint32_t issuerId[DMR_RCTL_REPLAY_CACHE];
    uint32_t lastSeq[DMR_RCTL_REPLAY_CACHE];        /* останній прийнятий seq для цього видавця */
} dmr_rctl_gate_t;

/* (Re)initialise the gate from the loaded "RCTL" config block (лише прапорець
 * enabled -- жодного allowlist). Скидає кеш anti-replay. */
void dmr_rctl_gate_init(dmr_rctl_gate_t *g, uint8_t enabled);

/* Returns 1 if this (issuerId, seq) is accepted: gate enabled AND (видавець
 * новий для кешу АБО seq строго більший за останній прийнятий від НЬОГО ЖЕ) --
 * і записує/оновлює seq у кеші. Returns 0 інакше (вимкнено, або replay/
 * out-of-order від уже відомого видавця) і НЕ змінює стан кешу (щоб відхилений/
 * повторений кадр не міг збити відлік справжньому видавцю). */
int dmr_rctl_gate_check(dmr_rctl_gate_t *g, uint32_t issuerId, uint32_t seq);

#endif /* DMR_RCTL_PDU_H */

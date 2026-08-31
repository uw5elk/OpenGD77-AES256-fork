/*
 * dmr_rctl_pdu.h — pure PDU pack/unpack + allowlist/anti-replay gate for the "remote
 * control" idea in PLANS.md section 3 (radio check / remote monitor / stun-revive).
 *
 * Deliberately has NO STM32/HR-C6000/OpenGD77 dependency, so it can be built and
 * tested on a host exactly like crypto/dmr_aes.c (see tests/test_dmr_rctl_pdu.c).
 * The radio-side glue (loading the "RCTL" custom-data config block, keying a
 * CSBK-led data call via dmrDataTxLoad(), wiring the RX burst hook) is NOT part of
 * this file and has not been written/wired yet — see PLANS.md for the status.
 *
 * Trust model (agreed 2026-08-31, see PLANS.md section 3):
 *   - the command PDU is AES-256-ECB encrypted with the SAME key as voice on the
 *     channel (caller does the encrypt/decrypt — this file only deals with the
 *     16-byte plaintext once decrypted);
 *   - a radio only ACTS on a command if remote control is explicitly enabled AND
 *     the issuer's DMR ID is on its (non-empty) allowlist — both opt-in, default
 *     off/empty;
 *   - a per-issuer monotonically increasing sequence counter blocks replay of a
 *     captured-and-rebroadcast command.
 */
#ifndef DMR_RCTL_PDU_H
#define DMR_RCTL_PDU_H

#include <stdint.h>

#define DMR_RCTL_PDU_LEN     16   /* exactly one AES-256-ECB block: no padding logic needed */
#define DMR_RCTL_MAX_ALLOWED 8    /* max issuer IDs an "RCTL" custom-data block can list */

typedef enum
{
    DMR_RCTL_CMD_CHECK_REQ    = 1,  /* "чи ти тут?" - ціль одразу відповідає CHECK_ACK */
    DMR_RCTL_CMD_CHECK_ACK    = 2,
    DMR_RCTL_CMD_MONITOR_START = 3, /* НЕ реалізовано - див. застереження в PLANS.md */
    DMR_RCTL_CMD_MONITOR_STOP  = 4, /* НЕ реалізовано */
    DMR_RCTL_CMD_STUN          = 5, /* НЕ реалізовано */
    DMR_RCTL_CMD_REVIVE        = 6, /* НЕ реалізовано */
} dmr_rctl_cmd_t;

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
 * check the allowlist or replay counter - call dmr_rctl_gate_check() for that. */
int dmr_rctl_unpack(const uint8_t in16[DMR_RCTL_PDU_LEN], dmr_rctl_msg_t *out);

/* ---- допуск і захист від replay ------------------------------------------- */
typedef struct
{
    uint8_t  enabled;                          /* 0 = приймання команд вимкнено (default) */
    uint8_t  numAllowed;                       /* 0..DMR_RCTL_MAX_ALLOWED */
    uint32_t allowedId[DMR_RCTL_MAX_ALLOWED];
    uint32_t lastSeq[DMR_RCTL_MAX_ALLOWED];    /* останній прийнятий seq на кожен дозволений ID */
} dmr_rctl_gate_t;

/* (Re)initialise the gate from the loaded "RCTL" config block. numAllowed is
 * clamped to DMR_RCTL_MAX_ALLOWED. Resets all replay counters to 0. */
void dmr_rctl_gate_init(dmr_rctl_gate_t *g, uint8_t enabled, const uint32_t *allowedIds, uint8_t numAllowed);

/* Returns 1 if this (issuerId, seq) is accepted: gate enabled AND allowlist
 * non-empty AND issuerId is on it AND seq is strictly greater than the last
 * accepted seq for that issuer - and records seq as the new high-water mark.
 * Returns 0 otherwise and leaves all state unchanged (so a rejected/replayed
 * frame can never be used to desync a legitimate issuer's counter). */
int dmr_rctl_gate_check(dmr_rctl_gate_t *g, uint32_t issuerId, uint32_t seq);

#endif /* DMR_RCTL_PDU_H */

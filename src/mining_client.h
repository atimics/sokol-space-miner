/*
 * mining_client.h -- Client-side RATi mining loop (Phase 1).
 *
 * Subscribes to SIM_EVENT_PICKUP (tractored fragment). The burst
 * size scales with the fragment's ore tonnage, so bigger hauls yield
 * more candidates. Each candidate is a SHA-256 pseudokey whose
 * base58 prefix is classified for grade (common → fine → rare →
 * RATi → commissioned).
 *
 * Phase-1 entropy is client-local: each player seeds their own burst
 * from ship state + asteroid_id + world time. Server does not verify
 * finds this round — the economy is trust-on-first-sell. V1.5 makes
 * the pickup seed authoritative so stations can cross-check.
 */
#ifndef CLIENT_MINING_H
#define CLIENT_MINING_H

#include <stdbool.h>
#include <stdint.h>

#include "mining.h"     /* shared/mining.h — grade enum, payout, primitives */
#include "game_sim.h"   /* world_t */

typedef struct {
    mining_keypair_t keypair;
    uint8_t  grade;            /* mining_grade_t */
    uint16_t asteroid_id;
    uint32_t world_tick_ms;    /* when mined (world_time * 1000) */
} mined_keypair_t;

typedef struct {
    bool player_ready;
    mining_keypair_t player;
    char player_callsign[8];

    mined_keypair_t holdings[MINING_HOLDINGS_MAX];
    int holdings_count;
    int holdings_by_grade[MINING_GRADE_COUNT];
    int total_value_cached;

    /* HUD feedback for the most recent burst's best find. */
    float recent_find_timer;            /* seconds remaining to show */
    mining_grade_t recent_find_grade;
    char recent_find_callsign[8];

    /* Monotonic — never rewound, so re-mining the same asteroid still
     * produces distinct candidates. */
    uint32_t burst_nonce_cursor;
} mining_client_t;

/* Initialise player keypair + holdings (reads localStorage in wasm). */
void mining_client_init(void);

/* Run a burst sized by the fragment's ore tonnage. Called on every
 * SIM_EVENT_PICKUP the local player triggers. */
void mining_client_on_pickup(const world_t *w,
                             int local_player_slot,
                             float ore_tons);

/* Per-frame timers (fades the recent-find HUD badge). */
void mining_client_tick(float dt);

/* Read-only accessor for HUD / station UI. */
const mining_client_t *mining_client_get(void);

#endif /* CLIENT_MINING_H */

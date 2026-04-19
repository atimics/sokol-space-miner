/*
 * mining_client.h -- Client-side RATi session state.
 *
 * "RATi IS the ore." There is no separate keypair inventory — the
 * grade roll is authoritative on the server at smelt time, and its
 * payout is credited directly to the player's station ledger.
 *
 * This module is now just:
 *   - the player's pubkey (session identity shown as callsign)
 *   - per-session stats on grade strikes (fed by SIM_EVENT_SELL)
 *
 * The old holdings[] pool, sell-batch wire, and classify-on-pickup
 * burst have all been removed.
 */
#ifndef CLIENT_MINING_H
#define CLIENT_MINING_H

#include <stdbool.h>
#include <stdint.h>

#include "mining.h"     /* shared/mining.h — grade enum, payout_multiplier */

typedef struct {
    bool player_ready;
    mining_keypair_t player;
    char player_callsign[8];

    /* Per-session stats: how many smelts of each grade this session,
     * and the total bonus credits earned above the baseline ore price. */
    int strikes_by_grade[MINING_GRADE_COUNT];
    int bonus_cr_total;

    /* HUD feedback for the most recent rare strike. */
    float recent_strike_timer;
    mining_grade_t recent_strike_grade;
    int recent_strike_bonus;
} mining_client_t;

/* Initialise player keypair (reads localStorage in wasm). */
void mining_client_init(void);

/* Record a strike reported by SIM_EVENT_SELL. */
void mining_client_record_strike(mining_grade_t grade, int bonus_cr);

/* Per-frame timers (fades the recent-strike HUD badge). */
void mining_client_tick(float dt);

/* Read-only accessor for HUD / station UI. */
const mining_client_t *mining_client_get(void);

#endif /* CLIENT_MINING_H */

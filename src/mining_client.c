/*
 * mining_client.c -- Client-side RATi Phase 1 mining loop.
 */
#include "mining_client.h"
#include "game_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

static mining_client_t g_mining;

/* ------------------------------------------------------------------ */
/* Player keypair persistence                                          */
/* ------------------------------------------------------------------ */

/* localStorage key holds 64-byte (priv || pub) hex — cheap to persist
 * and easy to migrate when we swap the PRF. Holdings are not persisted
 * in Phase 1; a roguelike session starts with an empty pool. */
#define LS_KEY_PLAYER "signal.mining.player"

#ifdef __EMSCRIPTEN__
static void bytes_to_hex(const uint8_t *in, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = H[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static bool hex_to_bytes(const char *in, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char hi = in[i * 2], lo = in[i * 2 + 1];
        if (!hi || !lo) return false;
        int h = (hi >= '0' && hi <= '9') ? (hi - '0')
              : (hi >= 'a' && hi <= 'f') ? (hi - 'a' + 10)
              : (hi >= 'A' && hi <= 'F') ? (hi - 'A' + 10) : -1;
        int l = (lo >= '0' && lo <= '9') ? (lo - '0')
              : (lo >= 'a' && lo <= 'f') ? (lo - 'a' + 10)
              : (lo >= 'A' && lo <= 'F') ? (lo - 'A' + 10) : -1;
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}
#endif

static bool load_player_keypair(mining_keypair_t *out) {
#ifdef __EMSCRIPTEN__
    const char *s = emscripten_run_script_string(
        "(() => { try { return localStorage.getItem('" LS_KEY_PLAYER "') || ''; } catch(e) { return ''; } })()");
    if (!s || strlen(s) < 128) return false;
    uint8_t buf[64];
    if (!hex_to_bytes(s, buf, 64)) return false;
    memcpy(out->priv, buf, 32);
    memcpy(out->pub, buf + 32, 32);
    return true;
#else
    (void)out;
    return false;
#endif
}

static void save_player_keypair(const mining_keypair_t *kp) {
#ifdef __EMSCRIPTEN__
    char hex[129];
    uint8_t combined[64];
    memcpy(combined, kp->priv, 32);
    memcpy(combined + 32, kp->pub, 32);
    bytes_to_hex(combined, 64, hex);
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "try { localStorage.setItem('" LS_KEY_PLAYER "', '%s'); } catch(e) {}",
             hex);
    emscripten_run_script(cmd);
#else
    (void)kp;
#endif
}

static void generate_player_keypair(mining_keypair_t *out) {
    /* Seed from time + process-specific entropy. Good enough for a
     * roguelike character seed; V1.5 replaces with a real CSPRNG. */
    uint8_t seed[32];
    uint64_t t = (uint64_t)time(NULL);
    uintptr_t addr = (uintptr_t)out;
    for (int i = 0; i < 32; i++) {
        seed[i] = (uint8_t)((t >> ((i & 7) * 8)) ^ (rand() & 0xFF) ^ (addr >> (i & 15)));
    }
    mining_keypair_from_random_seed(seed, out);
}

/* ------------------------------------------------------------------ */
/* Pickup event → mining burst (size scales with ore tonnage)          */
/* ------------------------------------------------------------------ */

static void build_inputs_from_world(const world_t *w,
                                    int local_player_slot,
                                    mining_fracture_inputs_t *out) {
    memset(out, 0, sizeof(*out));
    out->fractured_by = (uint8_t)local_player_slot;

    if (local_player_slot >= 0 && local_player_slot < MAX_PLAYERS) {
        const ship_t *s = &w->players[local_player_slot].ship;
        out->ship_pos_x_q = mining_q100_(s->pos.x);
        out->ship_pos_y_q = mining_q100_(s->pos.y);
        out->ship_angle_q = mining_q1000_(s->angle);
        out->outward_dir_q = mining_q1000_(s->angle);
    }
    out->world_time_ms = (uint64_t)(w->time * 1000.0);
}

void mining_client_on_pickup(const world_t *w,
                             int local_player_slot,
                             float ore_tons) {
    if (!g_mining.player_ready) return;
    if (g_mining.holdings_count >= MINING_HOLDINGS_MAX) return;
    if (ore_tons <= 0.0f) return;

    int burst_size = (int)(ore_tons * (float)MINING_CANDIDATES_PER_TON + 0.5f);
    if (burst_size <= 0) return;

    mining_fracture_inputs_t inputs;
    build_inputs_from_world(w, local_player_slot, &inputs);

    uint8_t seed[32];
    mining_fracture_seed_compute(&inputs, seed);

    mining_grade_t best = MINING_GRADE_COMMON;
    char best_callsign[8] = {0};

    for (int i = 0; i < burst_size; i++) {
        if (g_mining.holdings_count >= MINING_HOLDINGS_MAX) break;

        mining_keypair_t kp;
        mining_keypair_derive(seed, g_mining.player.pub,
                              g_mining.burst_nonce_cursor++,
                              &kp);

        char callsign[8];
        mining_callsign_from_pubkey(kp.pub, callsign);
        mining_grade_t grade = mining_classify_base58(callsign);

        mined_keypair_t *slot = &g_mining.holdings[g_mining.holdings_count++];
        slot->keypair = kp;
        slot->grade = (uint8_t)grade;
        slot->asteroid_id = 0;
        slot->world_tick_ms = (uint32_t)inputs.world_time_ms;

        g_mining.holdings_by_grade[grade]++;
        g_mining.total_value_cached += mining_payout_for_grade(grade);

        if (grade > best) {
            best = grade;
            memcpy(best_callsign, callsign, sizeof(best_callsign));
        }
    }

    /* Badge only rare finds — common ore floods the HUD otherwise. */
    if (best >= MINING_GRADE_RARE) {
        g_mining.recent_find_timer = 3.0f;
        g_mining.recent_find_grade = best;
        memcpy(g_mining.recent_find_callsign, best_callsign,
               sizeof(g_mining.recent_find_callsign));
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

void mining_client_init(void) {
    memset(&g_mining, 0, sizeof(g_mining));
    if (!load_player_keypair(&g_mining.player)) {
        generate_player_keypair(&g_mining.player);
        save_player_keypair(&g_mining.player);
    }
    mining_callsign_from_pubkey(g_mining.player.pub, g_mining.player_callsign);
    g_mining.player_ready = true;
}

void mining_client_tick(float dt) {
    if (g_mining.recent_find_timer > 0.0f) {
        g_mining.recent_find_timer -= dt;
        if (g_mining.recent_find_timer < 0.0f) g_mining.recent_find_timer = 0.0f;
    }
}

const mining_client_t *mining_client_get(void) {
    return &g_mining;
}

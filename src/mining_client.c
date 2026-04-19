/*
 * mining_client.c -- Client-side RATi session state.
 *
 * See mining_client.h for the architectural rationale. This TU holds
 * the local player's pubkey (which drives their callsign) and a
 * running tally of grade strikes that SIM_EVENT_SELL tells us about.
 */
#include "mining_client.h"

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
    uint8_t seed[32];
    uint64_t t = (uint64_t)time(NULL);
    uintptr_t addr = (uintptr_t)out;
    for (int i = 0; i < 32; i++) {
        seed[i] = (uint8_t)((t >> ((i & 7) * 8)) ^ (rand() & 0xFF) ^ (addr >> (i & 15)));
    }
    mining_keypair_from_random_seed(seed, out);
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

void mining_client_record_strike(mining_grade_t grade, int bonus_cr) {
    if (grade < 0 || grade >= MINING_GRADE_COUNT) return;
    g_mining.strikes_by_grade[grade]++;
    g_mining.bonus_cr_total += bonus_cr;
    if (grade >= MINING_GRADE_RARE) {
        g_mining.recent_strike_timer = 3.0f;
        g_mining.recent_strike_grade = grade;
        g_mining.recent_strike_bonus = bonus_cr;
    }
}

void mining_client_tick(float dt) {
    if (g_mining.recent_strike_timer > 0.0f) {
        g_mining.recent_strike_timer -= dt;
        if (g_mining.recent_strike_timer < 0.0f) g_mining.recent_strike_timer = 0.0f;
    }
}

const mining_client_t *mining_client_get(void) {
    return &g_mining;
}

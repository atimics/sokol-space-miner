/*
 * test_highscore_replay.c — chain-log-as-canonical-leaderboard tests.
 *
 * Covers Phase 1 (death events round-trip through chain log into the
 * highscore view) and Phase 2 (world_id / build_id / killed_by columns
 * are projected correctly when BUILD_INFO + WORLD_INFO operator posts
 * precede the deaths).
 */
#include "test_harness.h"

#include "chain_log.h"
#include "highscore.h"
#include "station_authority.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#  include <dirent.h>
#else
#  include <direct.h>
#endif

static void hs_test_setup(const char *suffix) {
    char path[256];
    snprintf(path, sizeof(path), "%s_hs_%s", TMP("hs"), suffix);
    chain_log_set_dir(path);
    /* Best-effort mkdir; chain_log_emit also ensures the directory. */
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0775);
#endif
    /* Wipe any residue from previous runs of the same test. */
#ifndef _WIN32
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
            remove(full);
        }
        closedir(d);
    }
#endif
}

static void hs_test_teardown(void) {
    chain_log_set_dir(NULL);
}

/* Emit a CHAIN_EVT_DEATH on station 0 with the given fields. */
static void emit_death(world_t *w,
                       const char *callsign,
                       const uint8_t session_token[8],
                       const uint8_t killer_token[8],
                       float credits_earned) {
    chain_payload_death_t d;
    memset(&d, 0, sizeof(d));
    if (session_token) memcpy(d.victim_session_token, session_token, 8);
    if (callsign) {
        size_t n = strlen(callsign);
        if (n > 8) n = 8;
        memcpy(d.victim_callsign, callsign, n);
    }
    if (killer_token) memcpy(d.killer_token, killer_token, 8);
    d.credits_earned = credits_earned;
    d.epoch_tick = (uint64_t)(w->time * 120.0);
    (void)chain_log_emit(w, &w->stations[0], CHAIN_EVT_DEATH,
                         &d, (uint16_t)sizeof(d));
}

/* Emit a BUILD_INFO operator post (kind=3, text=hex SHA). */
static void emit_build_info(world_t *w, const char *hex) {
    size_t hl = strlen(hex);
    if (hl > 64) hl = 64;
    uint8_t payload[38 + 64];
    memset(payload, 0, sizeof(payload));
    payload[0] = 3;
    sha256_bytes((const uint8_t *)hex, hl, &payload[4]);
    payload[36] = (uint8_t)(hl & 0xFF);
    payload[37] = (uint8_t)((hl >> 8) & 0xFF);
    memcpy(&payload[38], hex, hl);
    (void)chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                         payload, (uint16_t)(38 + hl));
}

/* Emit a WORLD_INFO operator post (kind=4, text=belt_seed LE | hex). */
static void emit_world_info(world_t *w, uint32_t seed_u32, const char *hex) {
    size_t hl = strlen(hex);
    if (hl > 60) hl = 60;
    uint8_t text[64];
    text[0] = (uint8_t)(seed_u32 & 0xFF);
    text[1] = (uint8_t)((seed_u32 >> 8) & 0xFF);
    text[2] = (uint8_t)((seed_u32 >> 16) & 0xFF);
    text[3] = (uint8_t)((seed_u32 >> 24) & 0xFF);
    memcpy(&text[4], hex, hl);
    size_t tl = 4 + hl;
    uint8_t payload[38 + 64];
    memset(payload, 0, sizeof(payload));
    payload[0] = 4;
    sha256_bytes(text, tl, &payload[4]);
    payload[36] = (uint8_t)(tl & 0xFF);
    payload[37] = (uint8_t)((tl >> 8) & 0xFF);
    memcpy(&payload[38], text, tl);
    (void)chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                         payload, (uint16_t)(38 + tl));
}

TEST(test_chain_event_death_payload_size) {
    /* Compile-time pin already in chain_log.h; runtime memcmp roundtrip
     * confirms the layout matches what we serialize. */
    chain_payload_death_t a;
    memset(&a, 0, sizeof(a));
    for (int i = 0; i < 32; i++) a.victim_pubkey[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 8; i++) a.victim_session_token[i] = (uint8_t)(i + 100);
    memcpy(a.victim_callsign, "ABCD-123", 8);
    for (int i = 0; i < 8; i++) a.killer_token[i] = (uint8_t)(i + 200);
    a.cause = 2;
    a.epoch_tick = 0xDEADBEEFCAFEBABEULL;
    a.credits_earned = 1234.5f;
    a.credits_spent = 500.0f;
    a.ore_mined = 78.25f;
    a.asteroids_fractured = 17;

    uint8_t buf[sizeof(a)];
    memcpy(buf, &a, sizeof(a));
    chain_payload_death_t b;
    memcpy(&b, buf, sizeof(b));

    ASSERT(memcmp(&a, &b, sizeof(a)) == 0);
    ASSERT(sizeof(chain_payload_death_t) == 88);
}

TEST(test_highscore_replay_from_chain) {
    hs_test_setup("replay_basic");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 7700u;
    world_reset(w);

    uint8_t tok_a[8] = { 0xAA, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t tok_b[8] = { 0xBB, 1, 2, 3, 4, 5, 6, 7 };
    emit_death(w, "ALPHA-1", tok_a, NULL, 100.0f);
    emit_death(w, "BRAVO-2", tok_b, NULL, 250.0f);
    emit_death(w, "ALPHA-1", tok_a, NULL, 150.0f);  /* upgrade */

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());

    ASSERT_EQ_INT(t.count, 2);
    /* BRAVO-2 leads (250), ALPHA-1's best is 150. */
    ASSERT(memcmp(t.entries[0].callsign, "BRAVO-2", 7) == 0);
    ASSERT_EQ_FLOAT(t.entries[0].credits_earned, 250.0f, 0.001f);
    ASSERT(memcmp(t.entries[1].callsign, "ALPHA-1", 7) == 0);
    ASSERT_EQ_FLOAT(t.entries[1].credits_earned, 150.0f, 0.001f);

    hs_test_teardown();
}

TEST(test_highscores_survive_world_reset) {
    hs_test_setup("survive_reset");

    /* World A: belt_seed=11111. */
    WORLD_HEAP wa = calloc(1, sizeof(world_t));
    ASSERT(wa != NULL);
    wa->rng = 11111u;
    world_reset(wa);
    /* Emit WORLD_INFO so the entry carries an explicit world_id. */
    emit_world_info(wa, wa->belt_seed, "aabbccdd");
    uint8_t tok_a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    emit_death(wa, "AYE", tok_a, NULL, 500.0f);

    /* World B: belt_seed=22222 → distinct station-0 pubkey → distinct
     * chain log file on disk. World A's file lingers as an orphan. */
    WORLD_HEAP wb = calloc(1, sizeof(world_t));
    ASSERT(wb != NULL);
    wb->rng = 22222u;
    world_reset(wb);
    emit_world_info(wb, wb->belt_seed, "11223344");
    uint8_t tok_b[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    emit_death(wb, "BEE", tok_b, NULL, 800.0f);

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());

    ASSERT_EQ_INT(t.count, 2);
    bool found_aye = false, found_bee = false;
    uint32_t aye_world = 0, bee_world = 0;
    for (int i = 0; i < t.count; i++) {
        if (memcmp(t.entries[i].callsign, "AYE", 3) == 0) {
            found_aye = true;
            aye_world = t.entries[i].world_id;
        }
        if (memcmp(t.entries[i].callsign, "BEE", 3) == 0) {
            found_bee = true;
            bee_world = t.entries[i].world_id;
        }
    }
    ASSERT(found_aye && found_bee);
    ASSERT(aye_world == 11111u);
    ASSERT(bee_world == 22222u);
    ASSERT(aye_world != bee_world);

    hs_test_teardown();
}

TEST(test_killer_callsign_resolved) {
    hs_test_setup("killer_resolve");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 8800u;
    world_reset(w);

    /* Player A dies first (so the map collects A's session_token →
     * "AAAAA-1" mapping). Then player B dies with killer_token = A's
     * token; replay should resolve killed_by to "AAAAA-1". */
    uint8_t tok_a[8] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8 };
    uint8_t tok_b[8] = { 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8 };
    emit_death(w, "AAAAA-1", tok_a, NULL,  50.0f);
    emit_death(w, "BBBBB-2", tok_b, tok_a, 200.0f);

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());

    ASSERT_EQ_INT(t.count, 2);
    /* BBBBB-2 leads (200). Its killed_by should be "AAAAA-1". */
    ASSERT(memcmp(t.entries[0].callsign, "BBBBB-2", 7) == 0);
    char kb[9] = {0};
    memcpy(kb, t.entries[0].killed_by, 8);
    ASSERT_STR_EQ(kb, "AAAAA-1");

    /* AAAAA-1 has no killer; killed_by stays zero. */
    static const uint8_t zero[8] = {0};
    ASSERT(memcmp(t.entries[1].killed_by, zero, 8) == 0);

    hs_test_teardown();
}

TEST(test_build_info_tagged) {
    hs_test_setup("build_info");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9900u;
    world_reset(w);

    /* Emit BUILD_INFO BEFORE the death — the replay walker maintains
     * a build cursor that tags each subsequent death event. */
    emit_build_info(w, "deadbeef");
    uint8_t tok[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    emit_death(w, "CHARLIE", tok, NULL, 999.0f);

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());

    ASSERT_EQ_INT(t.count, 1);
    /* "deadbeef" → 0xdeadbeef in the build_id u32. */
    ASSERT(t.entries[0].build_id == 0xdeadbeefu);

    hs_test_teardown();
}

void register_highscore_replay_tests(void) {
    TEST_SECTION("\nhighscore replay tests:\n");
    RUN(test_chain_event_death_payload_size);
    RUN(test_highscore_replay_from_chain);
    RUN(test_highscores_survive_world_reset);
    RUN(test_killer_callsign_resolved);
    RUN(test_build_info_tagged);
}

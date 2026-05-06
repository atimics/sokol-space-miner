/*
 * inspect_anim.c -- Stable per-row content fingerprint for the inspect
 * snapshot pane scramble animation. Lives in its own tiny TU so the
 * test binary can link it without pulling in sokol / HUD deps.
 *
 * The animation re-fires only when the fingerprint changes; identical
 * frames re-render the row without re-triggering the settle. See
 * test_inspect_anim.c.
 */
#include "net.h"
#include "inspect_anim.h"

uint64_t hud_row_signature(const NetInspectSnapshotRow *row) {
    /* FNV-1a over the fields whose change means a different row.
     * Volatile presentation-time data only — no allocation, no hashing
     * library, deterministic across builds. */
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *bytes[] = {
        row->cargo_pub, row->receipt_head,
        row->origin_station, row->latest_station,
    };
    for (size_t b = 0; b < sizeof(bytes)/sizeof(bytes[0]); b++) {
        for (int i = 0; i < 32; i++) {
            h ^= bytes[b][i];
            h *= 0x100000001b3ull;
        }
    }
    uint8_t scalars[] = {
        row->commodity, row->grade, row->chain_len, row->flags,
        (uint8_t)(row->quantity & 0xff), (uint8_t)((row->quantity >> 8) & 0xff),
        (uint8_t)(row->event_id & 0xff),
        (uint8_t)((row->event_id >> 8) & 0xff),
    };
    for (size_t i = 0; i < sizeof(scalars); i++) {
        h ^= scalars[i];
        h *= 0x100000001b3ull;
    }
    if (h == 0) h = 1;
    return h;
}

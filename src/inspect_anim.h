/*
 * inspect_anim.h -- Public surface for the inspect-pane row animation
 * helpers. Decoupled from client.h so signal_test (which doesn't link
 * sokol / hud.c) can verify the row-signature contract.
 */
#ifndef INSPECT_ANIM_H
#define INSPECT_ANIM_H

#include <stdint.h>
#include "net.h"  /* NetInspectSnapshotRow */

/* Per-row content fingerprint. Identical content must produce identical
 * fingerprints across frames; any non-trivial change retriggers the
 * scramble-resolve animation. */
uint64_t hud_row_signature(const NetInspectSnapshotRow *row);

#endif

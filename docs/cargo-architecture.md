# Cargo Architecture: matter, identity, and what counts as a crate

**Status:** draft for review
**Audience:** anyone touching the cargo / manifest / chain-log code
**Supersedes:** the implicit "we'll crateify everything" assumption that
shaped the original slice plan in PR #526. That plan had the right
substrate work but the wrong target.

## TL;DR

Matter in Signal exists in **three** states — not one. **Crate identity
is born at the smelt/craft boundary**, not at the moment of mining. The
substrate already implements this correctly; the things we were about
to "fix" weren't actually broken. The real gaps are in the **chain log's
witness coverage of pre-crate matter** and in the **player-facing
display** of provenance the substrate already records.

## The conceptual model

Matter moves through Signal in three states. Each has a different
identity story, lives in a different part of the codebase, and answers
to a different invariant.

```
            ┌─────────────────┐
            │   FRAGMENT      │   in-space, physical, fragment_pub
            │   (in space)    │   ── identity by content hash, position
            └────────┬────────┘   ── tracked as asteroid_t in world.asteroids[]
                     │
                     │  tow → arrive at hopper → smelt
                     │
                     ▼
            ┌─────────────────┐
            │   BULK FLOAT    │   ephemeral working buffer
            │   (at station)  │   ── no identity, no chain entry
            └────────┬────────┘   ── _inventory_cache[ORE], smelter integration
                     │
                     │  furnace produces output
                     │
                     ▼
            ┌─────────────────┐
            │     CRATE       │   named, content-addressed, provenance-bearing
            │   (anywhere)    │   ── cargo_unit_t with parent_merkle
            └─────────────────┘   ── lives in ship.manifest / station.manifest
```

### State 1 — Fragment (pre-crate, in space)

A fractured piece of an asteroid. Lives as an `asteroid_t` in the
world. Has a `fragment_pub[32]` content hash that uniquely identifies
*this specific chunk of mineral* across the universe. Has physics —
position, velocity, mass.

**Critically, a fragment is not in any manifest.** It's not a
`cargo_unit_t`. It exists as a physical object in space that ships can
tow via `ship.towed_fragments[10]` (an array of int16 indices into the
asteroid array, max 10 with upgrades). When you tow a fragment you're
not "putting it in your hold" — you're dragging a physical object
through space behind your ship.

A fragment carries its own provenance via `fragment_pub` and
`fracture_seed`. That provenance survives forever in the chain log if
the fragment is ever smelted (the resulting ingot's `parent_merkle ==
fragment_pub`). If the fragment is destroyed in the void without being
smelted, its identity is lost — there's nothing to inherit it.

### State 2 — Bulk Float (transient, at-station-only)

This is the form we were about to migrate, until we realized it's a
working buffer, not a stockpile.

`station._inventory_cache[c]` for `c < COMMODITY_RAW_ORE_COUNT` (i.e.
the three raw ore slots) holds floats representing ore that has *just
been deposited at a hopper* and *is about to be smelted*. The typical
lifetime of a value in this float is a few sim ticks — deposit, brief
buffer, consume.

There are two ways ore lands here:

1. **Player tow → hopper drop.** A player tows a ferrite fragment into
   the radius of a hopper module. The fragment is consumed
   atomically — the hopper float briefly accumulates the fragment's
   ore amount, and within ticks the furnace pulls from that float to
   produce an ingot. (Actually, looking at the code, the most common
   path is even tighter than that: the fragment is consumed and the
   ingot is minted in the same operation, with `parent_merkle =
   fragment_pub`. The float never holds the ore for long.)
2. **NPC autopilot deposit.** NPCs run a hauler/miner loop that can
   deposit ore amounts into hoppers without going through the
   tow-physics layer. This is the path that *actually* makes the float
   non-transient — NPCs can pile ore in faster than furnaces drain it,
   and the float can sit at meaningful values.

**Bulk float has no crate identity by design.** It represents
undifferentiated material in transit through the smelter's working
volume. Putting it in the manifest would be like assigning a serial
number to the iron filings sitting in a foundry's input chute — the
abstraction doesn't fit the real-world thing.

### State 3 — Crate (named, identity-bearing, persistent)

`cargo_unit_t` — 80 bytes, content-addressed via `pub`,
provenance-attached via `parent_merkle`. The substrate of all named
matter in Signal: ingots, frames, lasers, tractors, repair kits.

Crates live in manifests:
- `ship.manifest.units[]` (cap 32 by default)
- `station.manifest.units[]` (cap 256 by default)

Crates are **created** at one of three boundaries:
- `hash_ingot(commodity, grade, fragment_pub, idx)` — smelt
  produces an ingot whose `parent_merkle = fragment_pub`. Identity
  is born here.
- `hash_product(recipe_id, inputs[], idx)` — fab/craft consumes
  multiple input crates and produces a new output crate whose
  `parent_merkle = merkle_root(sorted_input_pubs)`. Identity
  inherits from inputs.
- `hash_legacy_migrate_unit(origin, commodity, idx)` — synthesizes
  a placeholder crate for finished goods loaded from pre-manifest
  saves. `parent_merkle` is zero (no provable parents).

Once a crate exists, its identity never changes. It can move between
manifests (`EVT_TRANSFER`), be consumed as an input to another craft
(`EVT_CRAFT`), or be destroyed silently (e.g. consumed in repair). The
chain log preserves its existence forever via the events that
referenced its `pub`.

## The smelt boundary: where identity is born

This is the central insight that the original slice plan missed.

**Below the smelt boundary** (fragment, bulk float): matter is
characterized but not crateified. A fragment has identity in the form of
`fragment_pub` but is not a `cargo_unit_t`; bulk float doesn't even have
that. They're matter waiting to become a thing.

**At the smelt boundary**: the furnace performs an irreversible
transformation. Input matter (a fragment, or units of bulk float)
becomes output matter (an ingot crate). The crate's `parent_merkle`
captures *what was consumed*. From this moment forward the matter is
crate-form: it has a name, a provenance graph, and a chain-log
trail.

**Above the smelt boundary**: every transformation produces another
crate. Frames are crates whose parents are ingot crates. Lasers are
crates whose parents are ingot crates. Repair kits are crates whose
parents are frames + lasers (which are themselves crates). The
provenance DAG can be walked backward from any leaf to its
fragment-shaped roots.

This is the right factoring because it matches the real-world
intuition: a ferrite ingot has a specific shape, a specific weight, a
specific bar code; "12 units of raw ore" is a quantity in a ledger,
not a thing you can put your hand on.

## Mapping the model to code

> **Note:** the file:line citations below were pulled from a focused
> code-walk. The Explore agent's deeper survey will sharpen these and
> add ones I missed; treat this section as the canonical reference
> after that pass folds in.

### Fragment

| Concern | Where |
|---|---|
| Storage | `world.asteroids[]` array. Each `asteroid_t` carries `fragment_pub[32]` and `fracture_seed[32]` (`shared/types.h:538-552`). |
| Player tow list | `ship.towed_fragments[10]` of int16 asteroid indices, plus `towed_count` (`shared/types.h:208-209`). |
| Fragment population | Asteroids spawn from belt generation (`shared/belt.c`, `src/asteroid_field.c`). Fragments are created when a parent asteroid is fractured in `server/sim_asteroid.c`. |
| Fragment consumption | `server/sim_production.c:730+` — when a towed fragment arrives at a hopper, it is removed from `towed_fragments[]` and the ingot is minted in the same step. |

### Bulk float

| Concern | Where |
|---|---|
| Storage | `station._inventory_cache[c]` for `c < COMMODITY_RAW_ORE_COUNT` (`shared/types.h:327`). The `_` prefix and "private; use accessors" comment indicate this is no longer treated as authoritative; for finished goods it's a derived cache, but for raw ore it's still where the value lives. |
| Deposit path 1 (player tow) | The atomic fragment-to-ingot path mostly bypasses the float. It briefly increments `_inventory_cache[output_ingot]` (line 748) — note that's the *ingot* commodity, not ore. The ore commodity slot is barely touched on this path. |
| Deposit path 2 (NPC) | NPC autopilot in `server/sim_ai.c` deposits ore amounts directly into `_inventory_cache[ORE]` when a hauler arrives at a hopper. (Specific function TBD; agent will pin down.) |
| Smelt consumption | `server/sim_production.c:240-280` — the hopper-style smelt path. Furnace tick reads `_inventory_cache[ore]`, computes `consume`, drains the float, mints ingots. |
| Wire visibility | `NET_MSG_STATION_MANIFEST` summarizes manifest contents to clients. Whether ore floats are summarized for display TBD; agent will confirm. |

### Crate

| Concern | Where |
|---|---|
| Type | `cargo_unit_t` (`shared/types.h:138-160`). 80 bytes. As of slice 0, byte 7 is `quantity` (u8). |
| Storage | `manifest_t` (`shared/types.h:151-155`) — `cargo_unit_t units[]` plus count/cap. Held by `ship_t.manifest` and `station_t.manifest`. |
| Creation | The three hash helpers in `src/manifest.c`: `hash_ingot` (line 481), `hash_product` (line 503), `hash_legacy_migrate_unit` (line 536). All set `quantity = 1` as of slice 0. |
| Mutation | `manifest_push`, `manifest_remove`, `manifest_consume_by_commodity` in `src/manifest.c`. |
| Chain witnessing | `chain_log_emit(EVT_SMELT)` in `server/sim_production.c:314` for ingot mint. `chain_log_emit(EVT_CRAFT)` at `:160` for fab/craft. `chain_log_emit(EVT_TRANSFER)` in `server/cargo_receipt_issue.c:58` for inter-holder moves. |

## What slice 0 actually bought us

PR #526 added a `quantity` field (u8) to `cargo_unit_t` in the byte
formerly used as `_pad`. The original justification was "ore crates can
pool many fragments under one provenance signature." Per the model
above, that use case **does not exist** — ore doesn't become a crate.

The field is still useful, just for a different reason. **It's a hook
for future bulk-mode operations on crates that genuinely warrant
identity but where individual addressability would balloon the
manifest.** Two plausible future use cases:

1. **Anonymous ingot stockpiles.** A station that has smelted 80
   anonymous ferrite ingots from this morning's mining run might
   reasonably represent that as one crate with `quantity = 80`,
   `prefix_class = ANONYMOUS`, `parent_merkle = ANONYMOUS_BATCH`,
   instead of 80 separate crates. The provenance is "anonymous batch
   from epoch X" — fine to compress. The 80-crate explosion was a
   legitimate concern; quantity solves it for the shape of matter
   where it actually applies.

2. **Bulk repair kits / consumables.** A repair kit that's "100 kits
   from this fab batch" could compress similarly. Anonymous,
   per-batch identity, quantity > 1.

So slice 0's work isn't wasted. It just turns out the field's natural
use is for compressing *finished* goods of low individual value, not
for representing *raw* ore that doesn't want crate identity at all.

## What the substrate doesn't yet give us

The actual gaps are not in the data model. They're in:

### Gap 1 — Chain log doesn't witness fragment lifecycle

Today, a fragment can be towed across half the universe, dropped at a
hopper, and smelted into an ingot — and only the *final* event
(`EVT_SMELT` for the ingot) is recorded in the chain log. The fragment's
journey is invisible.

This means heritage queries like "find a frame whose ferrite was towed
by 0F3H-CH from Belt-7 in epoch 4000" can't be answered, even though
the substrate has the fragment_pub and the towing relationship at
runtime — they just aren't witnessed.

**The fix is chain-log work, not crate work.** Add three new chain
event types (`EVT_FRAGMENT_TOW`, `EVT_FRAGMENT_DEPOSIT`,
`EVT_FRAGMENT_LOST`) and emit them at the corresponding sim transitions.
Fragment_pub stays the source of identity; the chain log just adds
witness coverage to the in-flight phase.

### Gap 2 — NPC-deposited ore mints ingots with broken lineage

When the smelter consumes from `_inventory_cache[ORE]` (the bulk-float
deposit path), the resulting ingot is minted without a real
`parent_merkle` — there's no source fragment to attribute to. Looking at
`server/sim_production.c:303-313`, the comment confirms this:

> "We don't know the source fragment_pub for hopper-path smelts (#280
> fragment-tow path is separate); use zero so verifiers can
> distinguish."

So the chain log already distinguishes "fragment-attributed ingot"
(parent_merkle = fragment_pub) from "hopper-attributed ingot"
(parent_merkle = 0). That's good — it means the substrate is honest
about the gap. But it also means **a chunk of the ingot supply has
zero-merkle lineage**, which limits the heritage-contract design space.

The fix is either:
- Have NPC autopilot also use the tow-physics path (so all ore arrives
  via fragments and inherits real lineage), or
- Mint a synthetic "NPC-batch" lineage anchor when the NPC deposits
  bulk into a hopper, and use *that* as `parent_merkle` for ingots
  smelted from the resulting float

The first is more uniform; the second is cheaper. Either way, this is
sim/AI work, not data-model work.

### Gap 3 — Players can't see provenance at all

The HUD shows hold contents as a commodity-and-grade summary. There's no
display surface that says "this ferrite ingot came from Belt-7's
fracture by 0F3H-CH at tick 4422." The substrate has the data; the UI
ignores it.

The fix is the "lineage-as-name" mechanic from the prior design
discussion. Cargo rows render with a one-line provenance haiku
generated from `cargo_unit_t.parent_merkle` walked against the chain
log. No new screens, no new keys, just a string format change.

## Recommended next moves

Ranked by value-per-effort:

**1. Player-visible lineage display (one week, low risk).**
Surface what the substrate already records. Render a single-line
provenance summary on cargo rows and station inventory rows. Pulls
from `cargo_unit_t` fields (origin_station, mined_block, prefix_class)
that already exist. Zero data-model change. The first thing players
will *feel* from years of substrate work.

**2. Chain events for fragment lifecycle (one to two weeks, medium
risk).** Add `EVT_FRAGMENT_TOW`, `EVT_FRAGMENT_DEPOSIT`,
`EVT_FRAGMENT_LOST`. Emit at the corresponding sim transitions. The
chain-log volume will rise (one event per fragment per state change),
so worth measuring against the binary-fuse rock-ledger machinery
already in place. This is what unlocks heritage contracts that
reference in-flight ore movements.

**3. Heal NPC-deposited ore lineage (one week, medium risk).**
Pick an approach (synthetic batch anchor vs route NPCs through the
tow-physics path). Backfill `parent_merkle` for the broken-lineage
ingot path. Now every ingot in the universe has provable parents.

**4. Heritage contract templates (two weeks, low risk).**
With (1)-(3) in place, contracts can filter on `parent_merkle` chains.
Build the contract-emission templates that reference real chain-log
history. This is the player-facing payoff: the universe's history
becomes the quest content.

What's *not* on this list:

- ❌ Migrate `_inventory_cache[ORE]` to manifest crates. The float is
  intentionally a working buffer. Not a problem; don't fix.
- ❌ Add ore-merge / ore-split logic. Ore doesn't have crate identity
  to merge or split.
- ❌ Add `EVT_TRANSFER` for ore deposits as raw float movement. The
  fragment-lifecycle events (gap 1) are the right unit of work; bulk
  ore transfers between locations don't happen meaningfully today.

## Out of scope (and why)

| Idea | Why we ruled it out |
|---|---|
| Fragments-as-cargo_unit_t | Fragments live in space with physics. Crates live in manifests as data. Conflating them duplicates identity (fragment_pub already exists) and violates the spatial/abstract divide. |
| Quantity > 1 on ore crates | There are no ore crates. Fragments are individual; bulk float is anonymous. |
| EVT_SPLIT / EVT_MERGE for ore | No crate identity to split or merge. |
| One unified "container" type that wraps all of {fragment, float, crate} | The three states have genuinely different identity stories. Forcing a single type makes the union as expensive as the maximum of all three, with conditionals everywhere. The current factoring is right. |
| Chain events for every bulk float mutation | Bulk float is meant to be ephemeral. Witnessing every tick-level integration value would explode chain log volume without adding semantic information. Witness fragment-lifecycle transitions instead. |

## What slice 0's quantity field IS for

Restated since this is the most likely thing to be misremembered:

The `quantity` field added in PR #526 is for **batch-level
identity-bearing crates** that compress multiple anonymous units of
the same provenance signature into one manifest entry. It is **not**
for raw ore.

Concrete near-term use case: anonymous ingot stockpiles at stations
where individual ingot identity carries no value beyond "this batch
came from this furnace at this epoch." A station with 80 anonymous
ferrite ingots from this morning's smelting can be one crate with
`quantity = 80`, `prefix_class = ANONYMOUS`, `parent_merkle =
batch_hash`. Saves 79 manifest entries; loses nothing meaningful.

When this becomes worth implementing depends on how often the
finished-goods manifest hits its 256-entry cap in practice. Worth
instrumenting before optimizing.

## Appendix: file map

| File | Role |
|---|---|
| `shared/types.h` | All struct definitions: `asteroid_t`, `ship_t`, `station_t`, `cargo_unit_t`, `manifest_t`, `commodity_t` enum |
| `shared/manifest.h` | Crate API: push/remove/find, hash_*, migration helpers |
| `src/manifest.c` | Crate implementation |
| `server/sim_production.c` | The smelt boundary lives here. Both fragment-tow and hopper-float smelt paths. |
| `server/sim_ai.c` | NPC autopilot, including ore-deposit logic that populates the hopper float |
| `server/sim_save.c` | Save format, including the manifest persistence and migration paths |
| `server/chain_log.h` / `chain_log.c` | Append-only signed event log per station |
| `server/cargo_receipt_issue.c` | EVT_TRANSFER emission |
| `shared/belt.c` / `src/asteroid_field.c` / `server/sim_asteroid.c` | Fragment generation and fracture |

---

## Decision

**Adopt the three-state model as canonical.** Update `CLAUDE.md` to
reference it as the cargo architecture's foundational vocabulary.

**Stop the original slice plan from PR #526.** The quantity field
landed and is genuinely useful for a different purpose; the rest of the
slices (1-5) targeted a problem that doesn't exist. The new roadmap is
the four moves above (display → fragment events → NPC lineage heal →
heritage contracts).

**No further data-model changes to ore.** Ore stays as fragments and
bulk float. The work is in chain-log coverage and player-facing
display.

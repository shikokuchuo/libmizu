/* SPSC channel: one producer, one consumer, one ring per direction, over
   a single host-created region mapped read-write by both sides. The hot
   path is entirely in user space — cached index snapshots, batched
   publication, a spill arena for mid-size payloads — with directed
   parker wakes at the edges and event-driven peer-death detection. The
   region layout is the REI_OFF_* table in rei.h. This file is the
   transport: ring mechanics, the arena, wakes, and the arena-frame
   resolution. Payload framing policy is the binding's (stage/read on
   the handle). */

#include <stdlib.h>
#include <stdio.h>
#include "internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#endif

/* Consumer head publication cadence: publish every K messages, on
   drain-empty, and before parking. The sender sees at most K slots less
   free space than truly exists and its keeper release trails by at most K
   slots — both benign. */
#define REI_HEAD_PUBLISH_K 32

// Channel state -------------------------------------------------------------------

/* One direction's ring. Shared pointers alias the mapped region;
   everything below them is process-local — producer-local cursors on the
   side that produces, consumer-local on the side that consumes; neither
   is ever mirrored into shared memory. */
typedef struct rei_chan_ring_s {
  _Atomic int64_t *tail;         /* shared: producer-published */
  _Atomic int64_t *head;         /* shared: consumer-published */
  unsigned char *slots;
  unsigned char *arena;          /* NULL when arena_size == 0 */
  uint64_t arena_size;
  uint64_t mask;
  uint32_t cap;
  uint32_t slot;
  /* producer-local */
  int64_t ltail;                 /* next slot to write */
  int64_t ptail;                 /* last published tail */
  int64_t cached_head;
  int64_t reaped_head;
  uint64_t aalloc, afree;        /* monotonic arena byte cursors */
  uint64_t *aend;                /* per-slot aalloc after that send */
  /* consumer-local */
  int64_t lhead;                 /* next slot to read */
  int64_t phead;                 /* last published head */
  int64_t cached_tail;
  uint32_t unpublished;
} rei_chan_ring;

struct rei_channel_s {
  struct rei_handle_s h;         /* first member: the seam view */
  rei_shm shm;                   /* our mapping; unmapped only in release */
  rei_preamble pre;
  unsigned char *base;
  int side;                      /* REI_ENTITY_HOST or REI_ENTITY_PEER */
  int spin;
  int released;                  /* full teardown ran; handle is dead */
  int verdict_dead;              /* sticky flock-confirmed peer death */
  int names_unlinked;            /* survivor cleanup already ran */
  int pk_ok;
  uint32_t inline_max;
  /* process-local adaptive recv-wait spin budget (ns): halved when an
     episode's spin comes up empty, grown on a completed wait via
     rei_spin_learn; never shared */
  uint64_t wait_budget_ns;
  /* tx keepers pinned and not yet reaped: the gate that lets keeperless
     traffic (the immediate kinds, self-contained streams) skip the
     reap's shared head load entirely */
  int64_t keep_out;

  _Atomic uint32_t *ready;
  _Atomic uint32_t *closedw;     /* bit 1 = host closed, bit 2 = peer closed */
  _Atomic uint64_t *peer_pid;
  _Atomic uint32_t *self_parked, *peer_parked;
  _Atomic uint32_t *self_reg, *peer_reg;

  rei_parker self_pk;            /* we park here; the peer unparks it */
  rei_parker peer_pk;            /* we unpark this */
  intptr_t live_self, live_peer; /* kept liveness fds/handles; 0 = not open */
  char live_self_path[1024];
  char live_peer_path[1024];
  _Atomic int peer_dead;         /* death-listener flag: wake trigger only */
  rei_death_watch *watch;

  /* Payload lifetime: the per-slot retain table (malloc'd, cap entries);
     the producer spill free list + lent-region ledger and the consumer
     SHM_RAW mapping cache ride the handle base (spill.c) */
  rei_keeper *keepers;

  rei_chan_ring tx, rx;
};

// Small helpers -------------------------------------------------------------------

static void chan_unlink_region_name(const char *name) {
#if defined(_WIN32)
  (void) name;                   /* kernel object: no unlink step */
#elif defined(__linux__)
  char path[64];
  snprintf(path, sizeof(path), "/dev/shm%s", name);
  unlink(path);
#else
  shm_unlink(name);
#endif
}

static uint32_t chan_closed_bit(const rei_channel *c) {
  return c->side == REI_ENTITY_HOST ? 1u : 2u;
}

static rei_status chan_intr(rei_channel *c) {
  rei_err_record(&c->h, REI_ERRCAT_INTERRUPTED, "interrupted");
  return REI_ERR;
}

// Wiring --------------------------------------------------------------------------

/* Build all region pointers from a validated preamble. Local cursors are
   initialised from the shared indices (zero on a fresh region; correct
   either way). */
static void chan_wire(rei_channel *c, const rei_preamble *p) {
  unsigned char *b = (unsigned char *) c->shm.addr;
  c->pre = *p;
  c->base = b;
  c->inline_max = p->slot - (uint32_t) sizeof(rei_slot_hdr);
  c->spin = (*(uint32_t *) (b + REI_OFF_FLAGS) & REI_FLAG_SPIN) != 0;

  c->ready = (_Atomic uint32_t *) (b + REI_OFF_READY);
  c->closedw = (_Atomic uint32_t *) (b + REI_OFF_CLOSED);
  c->peer_pid = (_Atomic uint64_t *) (b + REI_OFF_PEER_PID);

  int self = c->side, peer = 1 - c->side;
  c->self_parked =
    (_Atomic uint32_t *) (b + REI_ENTITY_OFFSET(self) + REI_ENTITY_PARKED);
  c->peer_parked =
    (_Atomic uint32_t *) (b + REI_ENTITY_OFFSET(peer) + REI_ENTITY_PARKED);
  c->self_reg =
    (_Atomic uint32_t *) (b + REI_ENTITY_OFFSET(self) + REI_ENTITY_REG);
  c->peer_reg =
    (_Atomic uint32_t *) (b + REI_ENTITY_OFFSET(peer) + REI_ENTITY_REG);

  uint64_t ring_bytes = (uint64_t) p->cap * p->slot;
  rei_chan_ring hp = {0}, ph = {0};
  hp.tail = (_Atomic int64_t *) (b + REI_OFF_HP_TAIL);
  hp.head = (_Atomic int64_t *) (b + REI_OFF_HP_HEAD);
  ph.tail = (_Atomic int64_t *) (b + REI_OFF_PH_TAIL);
  ph.head = (_Atomic int64_t *) (b + REI_OFF_PH_HEAD);
  hp.slots = b + REI_FIXED_LAYOUT_SIZE;
  ph.slots = hp.slots + ring_bytes;
  if (p->arena_size > 0) {
    hp.arena = ph.slots + ring_bytes;
    ph.arena = hp.arena + p->arena_size;
  }
  hp.arena_size = ph.arena_size = p->arena_size;
  hp.cap = ph.cap = p->cap;
  hp.slot = ph.slot = p->slot;
  hp.mask = ph.mask = (uint64_t) p->cap - 1;

  c->tx = c->side == REI_ENTITY_HOST ? hp : ph;
  c->rx = c->side == REI_ENTITY_HOST ? ph : hp;

  c->tx.ltail = c->tx.ptail = c->tx.cached_head = c->tx.reaped_head =
    atomic_load_explicit(c->tx.tail, memory_order_acquire);
  c->rx.lhead = c->rx.phead = c->rx.cached_tail =
    atomic_load_explicit(c->rx.head, memory_order_acquire);
}

/* Both liveness files live in the host-chosen directory recorded in the
   preamble; neither side ever resolves the path independently (a launcher
   that scrubs TMPDIR must not be able to split the death protocol). */
static int chan_live_paths(rei_channel *c, const char *livedir) {
  const char *suffix = c->shm.name + strlen(REI_PREFIX_LITERAL);
  int n1 = snprintf(c->live_self_path, sizeof(c->live_self_path),
                    "%s/rei_%s.live.%s", livedir, suffix,
                    c->side == REI_ENTITY_HOST ? "host" : "peer");
  int n2 = snprintf(c->live_peer_path, sizeof(c->live_peer_path),
                    "%s/rei_%s.live.%s", livedir, suffix,
                    c->side == REI_ENTITY_HOST ? "peer" : "host");
  return (n1 > 0 && (size_t) n1 < sizeof(c->live_self_path) &&
          n2 > 0 && (size_t) n2 < sizeof(c->live_peer_path)) ? 0 : -1;
}

// Peer-death ----------------------------------------------------------------------

/* The listener's flag is only ever a wake trigger; this fd-scoped probe is
   the verdict, and acquiring the dead side's lock serializes survivor
   cleanup. The verdict is sticky: once confirmed, no re-probe. */
static void chan_survivor_unlink(rei_channel *c) {
  if (c->names_unlinked) return;
  c->names_unlinked = 1;
  if (c->side == REI_ENTITY_HOST) {
    rei_region_unlink(&c->shm);
  } else {
    chan_unlink_region_name(c->shm.name);
  }
  if (c->live_self_path[0] != '\0') remove(c->live_self_path);
  if (c->live_peer_path[0] != '\0') remove(c->live_peer_path);
}

static int chan_probe_dead(rei_channel *c) {
  if (c->verdict_dead) return 1;
  if (c->live_peer == 0) return 0;
  if (rei_live_try(c->live_peer) != REI_LIVE_ACQUIRED) return 0;
  c->verdict_dead = 1;
  chan_survivor_unlink(c);
  /* the dead peer's view finalizers never ran: force-reclaim the lent
     regions (REFHELD ones leak + unlink — the holder set is wider) */
  rei_ledger_force(&c->h.fl, -1);
  return 1;
}

// Release -------------------------------------------------------------------------

/* Full teardown, idempotent. The order is load-bearing: the death watch and
   parkers reference the mapping (the watch's unpark target is the epoch word
   inside it), so both stop before the munmap. The retain table, free list,
   ledger, and mapping cache release their regions first — they are
   independent regions, but the teardown is the handle's. */
static void chan_release(rei_channel *c, int unlink_names) {
  if (c->released) return;
  c->released = 1;
  if (c->watch != NULL) {
    rei_death_watch_stop(c->watch);
    c->watch = NULL;
  }
  if (c->pk_ok) {
    rei_parker_detach(&c->self_pk);
    rei_parker_detach(&c->peer_pk);
    c->pk_ok = 0;
  }
  if (unlink_names) chan_survivor_unlink(c);
  /* the region name is always released by the host at teardown — the
     survivor unlink above, or here (the binding finalizer's role) */
  if (c->side == REI_ENTITY_HOST && !c->names_unlinked) {
    c->names_unlinked = 1;
    rei_region_unlink(&c->shm);
  }
  rei_stage_rollback(&c->h);
  rei_keepers_teardown(&c->h, c->keepers, c->pre.cap);
  rei_spill_fl_teardown(&c->h.fl);
  rei_oc_teardown(&c->h.oc);
  if (c->shm.addr != NULL) rei_shm_close_stack(&c->shm, 0);
  c->base = NULL;
  if (c->live_self != 0) {
    rei_live_close(c->live_self);
    c->live_self = 0;
  }
  if (c->live_peer != 0) {
    rei_live_close(c->live_peer);
    c->live_peer = 0;
  }
}

/* A failed create/attach: release everything the partial setup made (the
   host side unlinks; the peer side leaves the host's region alone). */
static void chan_failed(rei_channel *c, int host) {
  chan_release(c, host);
  free(c->tx.aend);
  free(c->keepers);
  free(c);
}

// Keeper reap ---------------------------------------------------------------------

/* Walk the shared head forward, clearing keepers the consumer has drained
   and advancing the arena free cursor past their chunks. Also refreshes the
   producer's cached head, so the full check and the reap ride one load.
   Head-advanced is the channel's consumer-done signal (the read runs
   before publishing), so a drained SHM_RAW keeper's region surrenders to
   the free list here — the only sender-side release point: the send-slot
   overwrite never sees a live keeper, since the ring's full check keeps
   ltail within cap of the reaped head.
   force: the caller needs the head refresh itself (the send full-check,
   the arena-full retry, pre-spill staging) — the load runs regardless.
   Otherwise the reap is keeper/arena/ledger maintenance only, and traffic
   that pins nothing (the immediate kinds, self-contained streams), holds
   no arena bytes, and has no lent regions outstanding skips the
   cross-core head load entirely. */
static void chan_reap(rei_channel *c, int force) {
  rei_chan_ring *r = &c->tx;
  if (!force && c->keep_out == 0 && r->aalloc == r->afree &&
      c->h.fl.led_n == 0)
    return;
  int64_t head = atomic_load_explicit(r->head, memory_order_acquire);
  r->cached_head = head;
  if (head <= r->reaped_head) return;
  for (int64_t i = r->reaped_head; i < head; i++) {
    uint32_t at = (uint32_t) ((uint64_t) i & r->mask);
    if (c->keepers[at].kind != REI_KEEP_FREE) {
      rei_keeper_release(&c->h, c->keepers, at);
      c->keep_out--;
    }
  }
  r->afree = r->aend[(uint64_t) (head - 1) & r->mask];
  r->reaped_head = head;
  /* quota-bounded ledger sweep on the busy path (spill.c): an O(outstanding)
     scan here would tax the hot loop under held-view workloads */
  rei_ledger_sweep(&c->h.fl, 4);
}

/* The binding-facing pre-spill reap (rei.h): a region checkout sees the
   freshest consumer-done surrenders. A no-op on pool handles. */
void rei_stage_reap(rei_handle *h) {
  if (h->htype == REI_HTYPE_CHANNEL)
    chan_reap((rei_channel *) h, 1);
}

// Spill arena ---------------------------------------------------------------------

/* Producer-local FIFO byte-ring: bump-allocated in slot order, freed in slot
   order by the reap, contiguous always (a chunk that would straddle the end
   pads to the start; the pad is accounted to the monotonic cursor and freed
   with the chunk). The consumer writes no arena state. */
static int chan_arena_alloc(rei_channel *c, uint64_t n, uint64_t *out) {
  rei_chan_ring *r = &c->tx;
  if (r->arena == NULL || n == 0 || n > r->arena_size) return 0;
  for (int attempt = 0; ; attempt++) {
    uint64_t off = r->aalloc % r->arena_size;
    uint64_t pad = off + n <= r->arena_size ? 0 : r->arena_size - off;
    if (r->aalloc + pad + n - r->afree <= r->arena_size) {
      r->aalloc += pad;
      *out = r->aalloc % r->arena_size;
      r->aalloc += n;
      return 1;
    }
    if (attempt > 0) return 0;
    chan_reap(c, 1); /* full -> reap -> retry, then give up */
  }
}

/* The binding-facing arena service (rei.h): the stage_fn's chunk
   reservation, returning the chunk pointer so the arena base stays
   core-private. NULL on a pool handle (no arena). */
void *rei_stage_arena_alloc(rei_handle *h, size_t n, uint64_t *off) {
  if (h->htype != REI_HTYPE_CHANNEL) return NULL;
  rei_channel *c = (rei_channel *) h;
  if (!chan_arena_alloc(c, n, off)) return NULL;
  return c->tx.arena + *off;
}

// Send ----------------------------------------------------------------------------

static rei_status chan_send1(rei_channel *c, void *obj) {
  rei_chan_ring *r = &c->tx;
  rei_stage_rollback(&c->h);   /* reclaim a previous stage's abandoned
                                  checkout before staging anew */

  if (atomic_load_explicit(c->closedw, memory_order_acquire) != 0)
    return REI_CLOSED;
  if (c->verdict_dead) return REI_PEER_GONE;

  if (r->ltail - r->cached_head >= (int64_t) r->cap) {
    chan_reap(c, 1);
    if (r->ltail - r->cached_head >= (int64_t) r->cap)
      /* full is off the hot path and exactly where "peer stopped draining"
         needs disambiguating */
      return chan_probe_dead(c) ? REI_PEER_GONE : REI_FULL;
  }

  uint64_t idx = (uint64_t) r->ltail & r->mask;
  unsigned char *sl = r->slots + idx * r->slot;
  rei_slot_hdr *hdr = (rei_slot_hdr *) sl;
  unsigned char *payload = sl + sizeof(rei_slot_hdr);
  /* The tier dispatch is the binding's: frame obj as (hdr, payload). The
     slot stays unpublished until ltail advances below, and the core
     mutates no shared state before stage returns, so a mid-stage abandon
     leaves the handle consistent. */
  if (c->h.binding.stage(obj, hdr, payload, c->inline_max, &c->h) != 0) {
    rei_err_record(&c->h, REI_ERRCAT_STAGE, "staging failed");
    return REI_ERR;
  }

  /* Commit the retain entry — the spill and pin kinds retain a region
     and/or a token; the self-contained kinds (NIL, RAWVEC, STR1,
     self-contained streams) retain nothing. The full-check invariant
     guarantees the slot's previous entry was already reaped, so a
     self-contained stage writes nothing. */
  if (c->h.fl.staging_kind != REI_KEEP_FREE) {
    rei_keeper_commit(&c->h, c->keepers, (uint32_t) idx);
    c->keep_out++;
  }
  r->aend[idx] = r->aalloc;
  r->ltail++;
  return REI_OK;
}

/* Publication: shared stores are an order of magnitude dearer than local
   ones, so the shared tail moves only here — after every send, once per
   batch. The wake-register OR is transition-only and the unpark
   parked-gated, so the steady-state publish is a release store plus two
   loads and no syscall. */
static void chan_flush(rei_channel *c) {
  rei_chan_ring *r = &c->tx;
  if (r->ptail == r->ltail) return;
  atomic_store_explicit(r->tail, r->ltail, memory_order_release);
  r->ptail = r->ltail;
  if (!(atomic_load_explicit(c->peer_reg, memory_order_relaxed) & 1u))
    atomic_fetch_or_explicit(c->peer_reg, 1u, memory_order_release);
  if (!c->spin) {
    /* Dekker with the consumer's announce: its parked-flag store and our
       tail store are each fenced before the cross-check, so either it sees
       the data in its re-check or we see the flag. */
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(c->peer_parked, memory_order_relaxed) != 0)
      rei_unpark(&c->peer_pk);
  }
}

// Recv ----------------------------------------------------------------------------

static int chan_rx_avail(rei_channel *c) {
  rei_chan_ring *r = &c->rx;
  if (r->lhead < r->cached_tail) return 1;
  r->cached_tail = atomic_load_explicit(r->tail, memory_order_acquire);
  return r->lhead < r->cached_tail;
}

/* Publication is what signals the sender "keepers may drop"; the tx-keeper
   reap piggybacks on the same cadence — one extra shared load per batch. */
static void chan_publish_head(rei_channel *c) {
  rei_chan_ring *r = &c->rx;
  if (r->phead != r->lhead) {
    atomic_store_explicit(r->head, r->lhead, memory_order_release);
    r->phead = r->lhead;
  }
  r->unpublished = 0;
  chan_reap(c, 0);
}

/* Resolve an arena-referencing frame to its byte range, then invoke the
   binding's read_fn: the callback always receives a dereferenceable
   range and never learns arena mechanics (the base and the FIFO reclaim
   stay core-private). ARENA carries the chunk offset in aux and the
   stream length in the payload, channel RAWSPILL the offset in the
   payload; both ranges are bounds-checked against the arena here. */
static rei_status chan_read(rei_channel *c, const unsigned char *sl,
                            void **out) {
  const rei_slot_hdr *hdr = (const rei_slot_hdr *) sl;
  const unsigned char *payload = sl + sizeof(rei_slot_hdr);
  const unsigned char *bytes;
  size_t limit;

  if (hdr->kind == REI_KIND_ARENA) {
    uint64_t off = hdr->aux, n;
    memcpy(&n, payload, sizeof(n));
    if (c->rx.arena == NULL || off > c->rx.arena_size ||
        n > c->rx.arena_size - off) {
      rei_err_record(&c->h, REI_ERRCAT_OTHER, "corrupt payload slot");
      return REI_ERR;
    }
    /* already mapped: no open, no syscall */
    bytes = c->rx.arena + off;
    limit = (size_t) n;
  } else if (hdr->kind == REI_KIND_RAWSPILL) {
    uint64_t off;
    memcpy(&off, payload, sizeof(off));
    if (c->rx.arena == NULL || off > c->rx.arena_size ||
        hdr->len > c->rx.arena_size - off) {
      rei_err_record(&c->h, REI_ERRCAT_OTHER, "corrupt payload slot");
      return REI_ERR;
    }
    bytes = c->rx.arena + off;
    limit = hdr->len;
  } else {
    bytes = payload;
    limit = c->inline_max;
  }

  rei_read_ctx ctx = c->h.read_tmpl;
  ctx.outcome = REI_RS_OK;

  void *obj = c->h.binding.read(hdr, bytes, limit, &ctx);
  if (obj == NULL) {
    rei_err_record(&c->h, REI_ERRCAT_OTHER,
                   ctx.gone ? "payload region vanished" :
                              "payload read failed");
    return REI_ERR;
  }
  *out = obj;
  return REI_OK;
}

/* Learn the budget from a completed wait; t_wait < 0 on the
   never-waited hot path (no clock read, no update there). */
static void chan_wait_learn(rei_channel *c, double t_wait) {
  if (t_wait < 0) return;
  c->wait_budget_ns = rei_spin_learn((rei_now() - t_wait) * 1e9,
                                     (uint64_t) c->wait_budget_ns,
                                     (uint64_t) REI_SPIN_BUDGET_NS);
}

/* Block until a message is available at rx.lhead (REI_OK) or a verdict.
   Drain-before-verdict: closed and peer-death are reported only through an
   empty ring, with one final tail refresh after the flag read so a
   publish-then-signal sequence is never inverted. */
static rei_status chan_wait_msg(rei_channel *c, double timeout_ms) {
  double deadline = -1;
  double t_wait = -1;
  chan_reap(c, 0);             /* recv is a reap trigger: the quiet-sender
                                  case pins at most cap payloads otherwise */
  for (;;) {
    if (chan_rx_avail(c)) {
      chan_wait_learn(c, t_wait);
      return REI_OK;
    }

    /* our wake-register bit: clear, then re-check — the producer's OR
       follows its tail publish, so data ORed before the clear is caught */
    if (atomic_load_explicit(c->self_reg, memory_order_relaxed) & 1u) {
      atomic_fetch_and_explicit(c->self_reg, ~1u, memory_order_acq_rel);
      if (chan_rx_avail(c)) {
        chan_wait_learn(c, t_wait);
        return REI_OK;
      }
    }

    if (atomic_load_explicit(c->closedw, memory_order_acquire) != 0)
      return chan_rx_avail(c) ? REI_OK : REI_CLOSED;
    if (c->verdict_dead)
      return chan_rx_avail(c) ? REI_OK : REI_PEER_GONE;
    if (atomic_load_explicit(&c->peer_dead, memory_order_acquire) &&
        chan_probe_dead(c))
      return chan_rx_avail(c) ? REI_OK : REI_PEER_GONE;

    if (timeout_ms == 0) {
      chan_publish_head(c);
      return REI_TIMEOUT;
    }
    /* one clock read serves the deadline compute and the spin bound */
    double now = rei_now();
    if (t_wait < 0) t_wait = now;
    if (deadline < 0 && timeout_ms > 0)
      deadline = now + timeout_ms / 1000;

    /* time-boxed spin before the park announce, clamped to the recv
       deadline: sub-µs publish gaps are absorbed without the park/wake
       syscall pair on either side */
    double until = now + (double) c->wait_budget_ns / 1e9;
    if (deadline >= 0 && deadline < until) until = deadline;
    int caught;
    REI_SPIN_WAIT(chan_rx_avail(c), until, REI_SPIN_CLOCK_EVERY_LIGHT,
                  caught);
    if (caught) {
      chan_wait_learn(c, t_wait);
      return REI_OK;
    }

    if (c->spin) {
      /* pure-spin mode: the producer skips wakes, so never park */
      if (rei_check_interrupt(&c->h.binding)) return chan_intr(c);
      if (deadline >= 0 && rei_now() >= deadline) {
        chan_publish_head(c);
        return REI_TIMEOUT;
      }
      continue;
    }

    /* the spin came up empty: decay toward the floor */
    c->wait_budget_ns = c->wait_budget_ns / 2 < REI_SPIN_FLOOR_NS ?
      REI_SPIN_FLOOR_NS : c->wait_budget_ns / 2;

    /* Park: snapshot -> announce -> re-check -> sleep bounded. Snapshot-
       before-announce means any unpark that observes the flag bumps the
       epoch after our snapshot and the sleep returns immediately. A stale
       flag left by an abandoned wait costs one wasted wake. */
    uint32_t e = rei_parker_snapshot(&c->self_pk);
    atomic_store_explicit(c->self_parked, 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    if (chan_rx_avail(c) ||
        atomic_load_explicit(c->closedw, memory_order_acquire) != 0 ||
        atomic_load_explicit(&c->peer_dead, memory_order_acquire) != 0) {
      atomic_store_explicit(c->self_parked, 0u, memory_order_relaxed);
      continue;
    }
    chan_publish_head(c);     /* always publish before parking */
    long ms = REI_INTERRUPT_BOUND_MS;
    if (deadline >= 0) {
      double rem = deadline - rei_now();
      if (rem <= 0) {
        atomic_store_explicit(c->self_parked, 0u, memory_order_relaxed);
        return REI_TIMEOUT;
      }
      long rem_ms = (long) (rem * 1000) + 1;
      if (rem_ms < ms) ms = rem_ms;
    }
    rei_park_bracket(&c->h.binding, 1);
    rei_park(&c->self_pk, e, ms);
    rei_park_bracket(&c->h.binding, 0);
    atomic_store_explicit(c->self_parked, 0u, memory_order_relaxed);
    if (rei_check_interrupt(&c->h.binding)) return chan_intr(c);
    /* availability first: a wake that delivered a message returns OK at
       the loop top regardless of the clock, so skip the read for it */
    if (deadline >= 0 && !chan_rx_avail(c) && rei_now() >= deadline)
      return REI_TIMEOUT;
  }
}

/* Materialize the message at rx.lhead (chan_wait_msg returned REI_OK),
   then advance and publish per the batching rule. Read-before-publish
   is the whole correctness story for large-message transport: publication
   is what lets the sender's reap drop the keeper pinning every region this
   message references. A failed read does not advance: the slot stays at
   the head. */
static rei_status chan_consume1(rei_channel *c, void **out) {
  rei_chan_ring *r = &c->rx;
  rei_status st = chan_read(c, r->slots + ((uint64_t) r->lhead & r->mask) * r->slot,
                            out);
  if (st != REI_OK) return st;
  r->lhead++;
  r->unpublished++;
  if (r->unpublished >= REI_HEAD_PUBLISH_K || !chan_rx_avail(c))
    chan_publish_head(c);
  return REI_OK;
}

// Create (host) -------------------------------------------------------------------

static int rei_pow2(uint64_t v) {
  return v != 0 && (v & (v - 1)) == 0;
}

rei_status rei_channel_create(rei_channel **out,
                              const rei_channel_opts *opts,
                              const rei_binding *b) {
  *out = NULL;
  if (b == NULL || b->stage == NULL || b->read == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "a channel binding needs stage and read callbacks");
    return REI_ERR;
  }
  uint64_t cap = opts->capacity;
  uint64_t slot = opts->slot_size;
  uint64_t arena = opts->arena_size;
  if (!rei_pow2(cap) || cap < 2 || cap > ((uint64_t) 1 << 24)) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "capacity must be a power of two between 2 and 2^24");
    return REI_ERR;
  }
  if (!rei_pow2(slot) || slot < 64 || slot > ((uint64_t) 1 << 20)) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "slot_size must be a power of two between 64 and 2^20");
    return REI_ERR;
  }
  if (arena % 64 != 0 || arena > ((uint64_t) 1.1e12)) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "arena_size must be a non-negative multiple of 64");
    return REI_ERR;
  }
  if (opts->drop_size != 0 && opts->drop == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "a nonzero drop_size needs drop bytes");
    return REI_ERR;
  }
  const char *livedir = rei_live_dir();
  if (livedir == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "cannot resolve liveness lock directory");
    return REI_ERR;
  }
  size_t livedir_len = strlen(livedir);
  if (livedir_len > 900) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "liveness directory path too long");
    return REI_ERR;
  }

  uint64_t ring_bytes = cap * slot;
  uint64_t drop_off = REI_FIXED_LAYOUT_SIZE + 2 * ring_bytes + 2 * arena;
  uint64_t livedir_off = REI_ALIGN64(drop_off + opts->drop_size);
  uint64_t total = livedir_off + livedir_len;
  if (total > ((uint64_t) 1 << 46)) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "channel region too large");
    return REI_ERR;
  }

  rei_channel *c = calloc(1, sizeof(*c));
  if (c == NULL) {
    rei_err_record_tls(REI_ERRCAT_NOMEMORY, "allocation failure");
    return REI_ERR;
  }
  int rc = rei_shm_create_populate(&c->shm, (size_t) total);
  if (rc != REI_ERRCAT_NONE) {
    const char *summary, *hint;
    rei_err_describe((rei_errcat) rc, &summary, &hint);
    rei_err_record_tls((rei_errcat) rc,
                       "cannot create channel region (%llu bytes): %s%s%s",
                       (unsigned long long) total, summary,
                       hint[0] != '\0' ? ". " : "", hint);
    free(c);
    return REI_ERR;
  }
  c->h.htype = REI_HTYPE_CHANNEL;
  c->h.binding = *b;
  rei_read_tmpl_init(&c->h);
  c->side = REI_ENTITY_HOST;
  c->wait_budget_ns = REI_SPIN_BUDGET_NS;

  rei_preamble p = {
    .magic = REI_MAGIC,
    .version = REI_ABI_VERSION,
    .cap = (uint32_t) cap,
    .slot = (uint32_t) slot,
    .host_pid = (uint64_t) rei_self_pid(),
    .arena_size = arena,
    .drop_offset = drop_off,
    .drop_size = opts->drop_size,
    .livedir_offset = livedir_off,
    .livedir_size = livedir_len,
  };
  unsigned char *base = (unsigned char *) c->shm.addr;
  rei_preamble_write(base, &p);
  if (opts->flags & REI_FLAG_SPIN)
    *(uint32_t *) (base + REI_OFF_FLAGS) = REI_FLAG_SPIN;
  if (opts->drop_size != 0)
    memcpy(base + drop_off, opts->drop, (size_t) opts->drop_size);
  memcpy(base + livedir_off, livedir, livedir_len);
  chan_wire(c, &p);
  c->tx.aend = calloc((size_t) cap, sizeof(uint64_t));
  c->keepers = calloc((size_t) cap, sizeof(rei_keeper));
  if (c->tx.aend == NULL || c->keepers == NULL) {
    rei_err_record_tls(REI_ERRCAT_NOMEMORY, "allocation failure");
    chan_failed(c, 1);
    return REI_ERR;
  }

  if (chan_live_paths(c, livedir) != 0) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "liveness file path too long");
    chan_failed(c, 1);
    return REI_ERR;
  }
  if (rei_live_open(c->live_self_path, &c->live_self) != 0 ||
      rei_live_try(c->live_self) != REI_LIVE_ACQUIRED) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "cannot lock host liveness file '%s'",
                       c->live_self_path);
    chan_failed(c, 1);
    return REI_ERR;
  }
  if (rei_live_open(c->live_peer_path, &c->live_peer) != 0) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "cannot create peer liveness file '%s'",
                       c->live_peer_path);
    chan_failed(c, 1);
    return REI_ERR;
  }

  /* Windows: the named parker events must exist before any peer opens them */
  if (rei_parker_attach(&c->self_pk,
                        (_Atomic uint32_t *) (base + REI_ENTITY_OFFSET(0)),
                        c->shm.name, REI_ENTITY_HOST, 1) != 0 ||
      rei_parker_attach(&c->peer_pk,
                        (_Atomic uint32_t *) (base + REI_ENTITY_OFFSET(1)),
                        c->shm.name, REI_ENTITY_PEER, 1) != 0) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "cannot attach channel parkers");
    chan_failed(c, 1);
    return REI_ERR;
  }
  c->pk_ok = 1;

  *out = c;
  return REI_OK;
}

/* The join token is the region name past the namespace prefix:
   "<pid hex>_<counter hex>". */
rei_status rei_channel_token(const rei_channel *c, char *buf, size_t cap) {
  if (c == NULL || c->released) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "channel handle is closed");
    return REI_ERR;
  }
  const char *suffix = c->shm.name + strlen(REI_PREFIX_LITERAL);
  int n = snprintf(buf, cap, "%s", suffix);
  if (n <= 0 || (size_t) n >= cap) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "token buffer too small");
    return REI_ERR;
  }
  return REI_OK;
}

/* Startup rendezvous: park on the host parker re-checking the ready word;
   the peer unparks after setting it. On success the death listener starts
   watching the pid the peer wrote into the control block. REI_TIMEOUT on
   deadline expiry — the caller walks the channel back (destroy). */
rei_status rei_channel_ready_wait(rei_channel *c, double timeout_ms) {
  if (c == NULL || c->released) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "channel handle is closed");
    return REI_ERR;
  }
  double deadline = timeout_ms < 0 ? -1 : rei_now() + timeout_ms / 1000;
  for (;;) {
    uint32_t e = rei_parker_snapshot(&c->self_pk);
    if (atomic_load_explicit(c->ready, memory_order_acquire) != 0) break;
    long ms = REI_INTERRUPT_BOUND_MS;
    if (deadline >= 0) {
      double rem = deadline - rei_now();
      if (rem <= 0) return REI_TIMEOUT;
      long rem_ms = (long) (rem * 1000) + 1;
      if (rem_ms < ms) ms = rem_ms;
    }
    rei_park_bracket(&c->h.binding, 1);
    rei_park(&c->self_pk, e, ms);
    rei_park_bracket(&c->h.binding, 0);
    if (rei_check_interrupt(&c->h.binding)) return chan_intr(c);
  }
  uint64_t pid = atomic_load_explicit(c->peer_pid, memory_order_acquire);
  c->watch = rei_death_watch_start((long) pid, &c->peer_dead, &c->self_pk);
  if (c->watch == NULL) {
    rei_err_record(&c->h, REI_ERRCAT_OTHER,
                   "cannot watch peer process %llu",
                   (unsigned long long) pid);
    return REI_ERR;
  }
  return REI_OK;
}

// Attach (peer) -------------------------------------------------------------------

rei_status rei_channel_attach(rei_channel **out, const char *token,
                              const rei_binding *b) {
  *out = NULL;
  if (b == NULL || b->stage == NULL || b->read == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "a channel binding needs stage and read callbacks");
    return REI_ERR;
  }
  if (!rei_token_valid(token)) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "malformed region-name suffix");
    return REI_ERR;
  }
  char name[REI_NAME_MAX];
  int nn = snprintf(name, sizeof(name), "%s%s", REI_PREFIX_LITERAL, token);
  if (nn <= 0 || (size_t) nn >= sizeof(name)) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "malformed region-name suffix");
    return REI_ERR;
  }

  rei_channel *c = calloc(1, sizeof(*c));
  if (c == NULL) {
    rei_err_record_tls(REI_ERRCAT_NOMEMORY, "allocation failure");
    return REI_ERR;
  }
  if (rei_shm_open_rw_stack(&c->shm, name, 1) != 0) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "cannot open channel region '%s'",
                       name);
    free(c);
    return REI_ERR;
  }
  c->h.htype = REI_HTYPE_CHANNEL;
  c->h.binding = *b;
  rei_read_tmpl_init(&c->h);
  c->side = REI_ENTITY_PEER;
  c->wait_budget_ns = REI_SPIN_BUDGET_NS;

  /* validate before touching any other field */
  rei_preamble p;
  const char *err = rei_preamble_validate(c->shm.addr, c->shm.size, &p);
  if (err == NULL && (p.livedir_size == 0 || p.livedir_size > 900))
    err = "liveness directory path is missing or too long";
  if (err != NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "invalid channel region: %s", err);
    rei_shm_close_stack(&c->shm, 0);
    free(c);
    return REI_ERR;
  }

  chan_wire(c, &p);
  c->tx.aend = calloc(p.cap, sizeof(uint64_t));
  c->keepers = calloc(p.cap, sizeof(rei_keeper));
  if (c->tx.aend == NULL || c->keepers == NULL) {
    rei_err_record_tls(REI_ERRCAT_NOMEMORY, "allocation failure");
    chan_failed(c, 0);
    return REI_ERR;
  }

  char livedir[1024];
  memcpy(livedir, c->base + p.livedir_offset, (size_t) p.livedir_size);
  livedir[p.livedir_size] = '\0';
  if (chan_live_paths(c, livedir) != 0) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "liveness file path too long");
    chan_failed(c, 0);
    return REI_ERR;
  }
  if (rei_live_open(c->live_self_path, &c->live_self) != 0 ||
      rei_live_try(c->live_self) != REI_LIVE_ACQUIRED) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "cannot lock peer liveness file '%s'",
                       c->live_self_path);
    chan_failed(c, 0);
    return REI_ERR;
  }
  if (rei_live_open(c->live_peer_path, &c->live_peer) != 0) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "cannot open host liveness file '%s'",
                       c->live_peer_path);
    chan_failed(c, 0);
    return REI_ERR;
  }
  if (rei_live_try(c->live_peer) == REI_LIVE_ACQUIRED) {
    c->verdict_dead = 1;
    chan_survivor_unlink(c);
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "host died before the channel was established");
    chan_failed(c, 0);
    return REI_ERR;
  }

  if (rei_parker_attach(&c->self_pk,
                        (_Atomic uint32_t *) (c->base + REI_ENTITY_OFFSET(1)),
                        c->shm.name, REI_ENTITY_PEER, 0) != 0 ||
      rei_parker_attach(&c->peer_pk,
                        (_Atomic uint32_t *) (c->base + REI_ENTITY_OFFSET(0)),
                        c->shm.name, REI_ENTITY_HOST, 0) != 0) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "cannot attach channel parkers");
    chan_failed(c, 0);
    return REI_ERR;
  }
  c->pk_ok = 1;

  /* watch the pid the host wrote into the preamble: a parent-pid read is
     unreliable through a shell exec chain */
  c->watch = rei_death_watch_start((long) p.host_pid, &c->peer_dead,
                                   &c->self_pk);
  if (c->watch == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "cannot watch host process %llu",
                       (unsigned long long) p.host_pid);
    chan_failed(c, 0);
    return REI_ERR;
  }

  *out = c;
  return REI_OK;
}

/* The peer bootstrap payload staged at create: borrowed bytes, valid
   until destroy. The peer's binding consumes them before ready_set — the
   host holds everything they reference alive exactly until ready. */
void rei_channel_drop(const rei_channel *c, const uint8_t **bytes,
                      uint64_t *n) {
  if (c == NULL || c->released) {
    *bytes = NULL;
    *n = 0;
    return;
  }
  *bytes = c->base + c->pre.drop_offset;
  *n = c->pre.drop_size;
}

rei_status rei_channel_ready_set(rei_channel *c) {
  if (c == NULL || c->released) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "channel handle is closed");
    return REI_ERR;
  }
  atomic_store_explicit(c->peer_pid, (uint64_t) rei_self_pid(),
                        memory_order_release);
  atomic_store_explicit(c->ready, 1u, memory_order_release);
  rei_unpark(&c->peer_pk);
  return REI_OK;
}

// Verbs ---------------------------------------------------------------------------

rei_status rei_channel_send(rei_channel *c, void *obj) {
  if (c == NULL || c->released) return REI_CLOSED;
  rei_status st = chan_send1(c, obj);
  chan_flush(c);
  chan_reap(c, 0);
  return st;
}

/* One call, one flush, one wake check, one reap; stops at the first
   message the ring refuses and reports the count accepted (probe why
   with a single send). A mid-batch stage/interrupt failure returns
   REI_ERR with the accepted prefix published. */
rei_status rei_channel_send_batch(rei_channel *c, void **objs, size_t n,
                                  size_t *accepted_out) {
  if (c == NULL || c->released) {
    *accepted_out = 0;
    return REI_CLOSED;
  }
  size_t i = 0;
  rei_status st = REI_OK;
  for (; i < n; i++) {
    st = chan_send1(c, objs[i]);
    if (st != REI_OK) break;
  }
  chan_flush(c);
  chan_reap(c, 0);
  *accepted_out = i;
  return st == REI_ERR ? REI_ERR : REI_OK;
}

rei_status rei_channel_recv(rei_channel *c, void **obj_out,
                            double timeout_ms) {
  if (c == NULL || c->released) return REI_CLOSED;
  rei_status st = chan_wait_msg(c, timeout_ms);
  if (st != REI_OK) return st;
  return chan_consume1(c, obj_out);
}

/* Up to cap messages under a single park cycle and a single batched head
   publication. Waits only for the first message; whatever else has already
   been published comes along, and the status discipline matches recv. */
rei_status rei_channel_recv_batch(rei_channel *c, void **objs, size_t cap,
                                  size_t *n_out, double timeout_ms) {
  if (c == NULL || c->released) {
    *n_out = 0;
    return REI_CLOSED;
  }
  if (cap < 1) {
    rei_err_record(&c->h, REI_ERRCAT_OTHER, "n must be at least 1");
    return REI_ERR;
  }
  rei_status st = chan_wait_msg(c, timeout_ms);
  if (st != REI_OK) {
    *n_out = 0;
    return st;
  }

  int64_t avail = c->rx.cached_tail - c->rx.lhead;
  size_t count = avail < (int64_t) cap ? (size_t) avail : cap;
  size_t i = 0;
  for (; i < count; i++) {
    st = chan_consume1(c, &objs[i]);
    if (st != REI_OK) break;
  }
  *n_out = i;
  return st;
}

// Close protocol ------------------------------------------------------------------

/* The peer half of the protocol: flush, set our bit, wake the other side.
   No rendezvous — process exit releases everything else. A no-op on an
   already-released handle. */
rei_status rei_channel_close_signal(rei_channel *c) {
  if (c == NULL || c->released) return REI_OK;
  chan_flush(c);
  atomic_fetch_or_explicit(c->closedw, chan_closed_bit(c),
                           memory_order_seq_cst);
  rei_unpark(&c->peer_pk);
  return REI_OK;
}

rei_status rei_channel_close(rei_channel *c, double timeout_ms) {
  if (c == NULL || c->released) return REI_OK;   /* close is idempotent */
  uint32_t own = chan_closed_bit(c), other = 3u ^ own;

  /* 1. flush — close never silently discards sent messages */
  chan_flush(c);
  /* 2. signal */
  atomic_fetch_or_explicit(c->closedw, own, memory_order_seq_cst);
  rei_unpark(&c->peer_pk);

  /* 3. rendezvous: the other side's bit, or its death confirmed */
  double deadline = timeout_ms < 0 ? -1 : rei_now() + timeout_ms / 1000;
  for (;;) {
    uint32_t e = rei_parker_snapshot(&c->self_pk);
    if ((atomic_load_explicit(c->closedw, memory_order_acquire) & other) !=
        0 || c->verdict_dead || chan_probe_dead(c)) {
      /* 4. release on rendezvous; on timeout the table is retained and
         destroy re-runs this check */
      chan_release(c, 1);
      return REI_OK;
    }
    long ms = REI_INTERRUPT_BOUND_MS;
    if (deadline >= 0) {
      double rem = deadline - rei_now();
      if (rem <= 0) return REI_TIMEOUT;
      long rem_ms = (long) (rem * 1000) + 1;
      if (rem_ms < ms) ms = rem_ms;
    }
    rei_park_bracket(&c->h.binding, 1);
    rei_park(&c->self_pk, e, ms);
    rei_park_bracket(&c->h.binding, 0);
    if (rei_check_interrupt(&c->h.binding)) return chan_intr(c);
  }
}

// Introspection -------------------------------------------------------------------

/* Reports peer *process* liveness (the fd-scoped lock verdict): an orderly
   close with the process still running is alive; a released handle is not.
   The probe can mutate (sticky verdict, survivor unlink, ledger force) —
   logically a read, hence the cast. */
int rei_channel_alive(const rei_channel *c) {
  if (c == NULL || c->released || c->verdict_dead) return 0;
  return !chan_probe_dead((rei_channel *) c);
}

rei_status rei_channel_info_get(const rei_channel *c, rei_channel_info *out) {
  if (c == NULL || c->released) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "channel handle is closed");
    return REI_ERR;
  }
  memset(out, 0, sizeof(*out));
  out->size = (uint32_t) sizeof(*out);
  out->name = c->shm.name;
  out->side = c->side;
  out->capacity = c->pre.cap;
  out->slot_size = c->pre.slot;
  out->arena_size = c->pre.arena_size;
  out->inline_max = c->inline_max;
  out->spin = c->spin;
  out->ready =
    (int32_t) atomic_load_explicit(c->ready, memory_order_acquire);
  out->closed =
    (int32_t) atomic_load_explicit(c->closedw, memory_order_acquire);
  out->peer_pid =
    (int64_t) atomic_load_explicit(c->peer_pid, memory_order_acquire);
  out->tx_sent = (uint64_t) c->tx.ltail;
  out->tx_published = (uint64_t) c->tx.ptail;
  out->tx_consumed =
    (uint64_t) atomic_load_explicit(c->tx.head, memory_order_acquire);
  out->rx_consumed = (uint64_t) c->rx.lhead;
  out->rx_published = (uint64_t) c->rx.phead;
  /* handle-local spill-reuse machinery; the zc view cache counters are
     binding-owned (the view cache rides the binding) — the core fills 0 */
  out->fl_entries = c->h.fl.n;
  out->fl_hits = c->h.fl.hits;
  out->open_hits = c->h.oc.hits;
  out->open_misses = c->h.oc.misses;
  out->ledger_entries = c->h.fl.led_n;
  out->zc_open_hits = 0;
  out->zc_open_misses = 0;
  return REI_OK;
}

rei_errcat rei_channel_errcat(const rei_channel *c) {
  return c != NULL ? c->h.errcat : rei_last_error_category();
}

const char *rei_channel_error(const rei_channel *c) {
  return c != NULL ? c->h.errmsg : rei_last_error_message();
}

/* The GC-finalizer target: idempotent, never blocks. On a handle that
   never rendezvoused, signal close so a live peer drains and exits
   instead of hanging on a vanished handle, then release what the
   (non-blocking) rendezvous check completes — the death verdict and
   rei_shm_reap backstop the rest. */
void rei_channel_destroy(rei_channel *c) {
  if (c == NULL) return;
  if (!c->released) {
    int unlink_names = 0;
    if (c->base != NULL) {
      atomic_fetch_or_explicit(c->closedw, chan_closed_bit(c),
                               memory_order_seq_cst);
      if (c->pk_ok) rei_unpark(&c->peer_pk);
      /* the close protocol's rendezvous re-check: unlink only once the
         peer has set its bit or its death is confirmed — a live peer's
         in-flight drain is never raced */
      uint32_t cw = atomic_load_explicit(c->closedw, memory_order_acquire);
      uint32_t other = 3u ^ chan_closed_bit(c);
      unlink_names = (cw & other) != 0 || c->verdict_dead ||
        (c->live_peer != 0 &&
         rei_live_try(c->live_peer) == REI_LIVE_ACQUIRED);
    }
    chan_release(c, unlink_names);
  }
  free(c->tx.aend);
  free(c->keepers);
  free(c);
}

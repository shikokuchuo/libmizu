/* Work-stealing task pool: per-submitter SPSC injection rings, per-worker
   Chase-Lev deques, and result slots, over one pool region mapped
   read-write by every participant. External submissions are
   SPSC-published into the submitting slot's own ring; workers consume
   them — and steal from each other's deques — with the same copy-then-CAS
   claim: both sides crash-atomic, with no claim state a mid-operation
   death can wedge. A worker looks for work in tier order (fairness tick,
   own deque bottom, random-victim steal, injection scan) and parks
   through the announce-then-rescan handshake, which is the sole guarantee
   against lost wakeups — there is no watchdog behind it. Results publish
   through a CAS'd status word per slot; payload lifetime is bridged by
   keepers on both sides of every queue (submitter task keepers released
   at collect or slot reuse; worker result keepers released when the slot
   leaves OK/ERR). The region layout is the wire-format section of rei.h.

   The worker loop is the transport: claim/steal mechanics, the parker
   protocol, and the result-slot CAS/wake live here. The task frame
   decode, the eval, and the error flattening are the binding's, invoked
   through binding.exec and publishing through the result sink
   (rei_result_publish*); the trace hook is a per-handle registration
   (rei_pool_set_trace) the core emit sites call. */

#include <stdlib.h>
#include <stdio.h>
#include "internal.h"

// Pool state ----------------------------------------------------------------------

/* A death watch's callback target (the death-listener parcel arms the
   controller's per-worker watches). */
struct rei_reap_ctx_s {
  rei_pool *pool;
  uint32_t slot;
};

struct rei_pool_s {
  struct rei_handle_s h;         /* first member: the seam view */
  rei_shm shm;                   /* our mapping; unmapped only in release */
  rei_pool_hdr hdr;
  unsigned char *base;
  int role;                      /* REI_ROLE_* */
  int released;
  long self_pid;                 /* fork guard */
  int wk_slot;                   /* our worker slot (-1 unless worker) */
  int sub_slot;                  /* our submitter slot (-1 unless we submit) */
  uint32_t inline_entry;         /* slot - sizeof(rei_entry_hdr) */
  uint32_t inline_rs;            /* slot - sizeof(rei_rs_hdr) */
  rei_trace_fn trace;            /* task-lifecycle hook (NULL = disabled) */
  void *trace_ctx;

  rei_wk_slot *wk;
  rei_sub_slot *sub;
  _Atomic uint64_t *inj_ready;
  _Atomic uint64_t *full_waiters;
  _Atomic uint32_t *shutdown;
  _Atomic uint64_t *parked_workers;
  _Atomic uint32_t *help_wanted;
  unsigned char *rings;
  unsigned char *results;

  rei_parker *pks;               /* every entity: workers, then submitters */
  int pk_ok;

  intptr_t live_self;            /* our held lock (worker / submitter slot) */
  intptr_t live_sub;             /* a worker's nested-submitter slot lock */
  intptr_t live_owner;           /* kept fd on the owner file; 0 = not open */
  intptr_t *live_all;            /* controller: kept probe fds, wk then sub */
  char livedir[1024];

  /* controller-only: per-worker death watches whose C callbacks run the
     reap off the calling thread */
  rei_death_watch **wk_watch;
  _Atomic int *wk_dead;
  struct rei_reap_ctx_s *reap_ctx;

  /* submitter-local */
  uint32_t rs_cursor;
  uint64_t task_counter;
  int64_t inj_ltail;             /* producer-local tail */
  int64_t inj_cached_head;       /* head is monotonic: stale only
                                    under-reports space; refreshed on
                                    apparent-full */

  /* Payload lifetime: the retain tables. keepers is the submitter's task
     table (rs_count entries) or the worker's result table (result_slots);
     sub_keepers the worker's nested-submit task table (rs_count,
     allocated on first nested submit). The producer spill free list +
     lent-region ledger and the consumer mapping cache ride the handle
     base (spill.c). */
  rei_keeper *keepers;
  uint32_t keepers_n;
  rei_keeper *sub_keepers;
  uint32_t sub_keepers_n;

  /* worker-local */
  unsigned char *scratch;        /* slot-sized claim copy buffer */
  uint32_t scan_start;           /* rotating ring-scan start */
  uint64_t claims;               /* fairness-tick counter (% 61) */
  uint64_t rng;                  /* xorshift state for victim selection */
  int announced;                 /* park announce (mask bit + park_state)
                                    not yet restored: gates the entry heal */
  int help_depth;                /* nested-collect help recursion depth */
  /* identity of the outermost (unwind-path) task eval, for
     rei_pool_unwind_sink: written only by catching = 0 executes — inner
     help / inline recursion clears the shm announce, so it cannot serve
     the unwind path */
  int in_eval;
  uint32_t cur_rs_index;
  uint64_t cur_seq, cur_task_id;
  uint16_t cur_sub_slot;
  uint32_t probe_streak;         /* thief-probe backstop state */
  uint32_t probe_victim;
  struct rei_rk_s { uint32_t idx; uint64_t seq; } *rk;
  uint32_t *rk_pos;              /* per result slot: rk position + 1, 0 = none */
  uint32_t rk_n, rk_cursor;
  /* cumulative stat counters, mirrored into the slot's stat_* fields by
     pool_stats_publish at park/fairness-tick cadence */
  uint64_t st_tasks, st_steals, st_inj, st_parks, st_helps;
  /* process-local adaptive spin budgets (ns; see rei_spin_learn) and the
     collect park count: st_collect_parks is the collect-side mirror of
     st_parks. None are mirrored to shm */
  uint64_t scan_budget_ns;
  uint64_t collect_budget_ns;
  uint64_t st_collect_parks;

  _Atomic int owner_dead;        /* death-listener flag: wake trigger only */
  rei_death_watch *watch;
};

// Layout ----------------------------------------------------------------------------

static uint64_t pool_ring_bytes(const rei_pool_hdr *h) {
  return REI_INJ_META_SIZE + (uint64_t) h->inj_cap * h->slot;
}

/* Region size implied by a header; the create sizes with it and the attach
   validator checks against it, so both sides share one piece of offset math. */
static uint64_t pool_fixed_size(const rei_pool_hdr *h) {
  return 64 +
    (uint64_t) h->max_workers * sizeof(rei_wk_slot) +
    (uint64_t) h->max_submitters * sizeof(rei_sub_slot) +
    128 +
    (uint64_t) h->max_submitters * pool_ring_bytes(h) +
    (uint64_t) h->max_workers * ((uint64_t) h->deque_cap * h->slot) +
    (uint64_t) h->result_slots * h->slot +
    192;
}

static void pool_wire(rei_pool *p) {
  unsigned char *b = (unsigned char *) p->shm.addr;
  const rei_pool_hdr *h = &p->hdr;
  p->base = b;
  p->inline_entry = h->slot - (uint32_t) sizeof(rei_entry_hdr);
  p->inline_rs = h->slot - (uint32_t) sizeof(rei_rs_hdr);

  size_t off = 64;
  p->wk = (rei_wk_slot *) (b + off);
  off += (size_t) h->max_workers * sizeof(rei_wk_slot);
  p->sub = (rei_sub_slot *) (b + off);
  off += (size_t) h->max_submitters * sizeof(rei_sub_slot);
  p->inj_ready = (_Atomic uint64_t *) (b + off + REI_TIER_READY_OFF);
  p->full_waiters = (_Atomic uint64_t *) (b + off + REI_TIER_FULL_OFF);
  off += 128;
  p->rings = b + off;
  off += (size_t) h->max_submitters * pool_ring_bytes(h);
  off += (size_t) h->max_workers * ((size_t) h->deque_cap * h->slot);
  p->results = b + off;
  off += (size_t) h->result_slots * h->slot;
  p->shutdown = (_Atomic uint32_t *) (b + off + REI_CTRL_SHUTDOWN_OFF);
  p->parked_workers = (_Atomic uint64_t *) (b + off + REI_CTRL_PARKED_OFF);
  p->help_wanted = (_Atomic uint32_t *) (b + off + REI_CTRL_HELP_OFF);
}

/* The layout accessors read only handle state but hand out pointers into
   the shared mapping: const handle, mutable region. */
static unsigned char *pool_ring(const rei_pool *p, uint32_t s) {
  return p->rings + (size_t) s * pool_ring_bytes(&p->hdr);
}

static _Atomic int64_t *ring_tail(unsigned char *r) {
  return (_Atomic int64_t *) (r + REI_INJ_TAIL_OFF);
}

static _Atomic int64_t *ring_head(unsigned char *r) {
  return (_Atomic int64_t *) (r + REI_INJ_HEAD_OFF);
}

static unsigned char *ring_entry(rei_pool *p, unsigned char *r, uint64_t i) {
  return r + REI_INJ_META_SIZE +
    (i & ((uint64_t) p->hdr.inj_cap - 1)) * p->hdr.slot;
}

static rei_rs_hdr *pool_rs(const rei_pool *p, uint32_t idx) {
  return (rei_rs_hdr *) (p->results + (size_t) idx * p->hdr.slot);
}

static unsigned char *deque_entry_at(rei_pool *p, rei_wk_slot *w, int64_t i) {
  return p->base + w->deque_buf_off +
    ((uint64_t) i & ((uint64_t) w->deque_cap - 1)) * p->hdr.slot;
}

static int deque_nonempty(rei_wk_slot *w) {
  return atomic_load_explicit(&w->deque_top, memory_order_acquire) <
    atomic_load_explicit(&w->deque_bottom, memory_order_acquire);
}

const char *rei_pool_hdr_validate(const void *region, size_t region_size,
                                  rei_pool_hdr *out) {
  if (region_size < 64)
    return "region is smaller than a pool header";
  rei_pool_hdr h;
  memcpy(&h, region, sizeof(h));
  if (h.magic != REI_POOL_MAGIC)
    return "bad magic: not a rei pool region";
  if (h.version != REI_ABI_VERSION)
    return "ABI version mismatch: participant and controller were built "
           "against different rei wire formats";
  if (h.max_workers == 0 || h.max_workers > REI_MAX_WORKERS ||
      h.max_submitters == 0 || h.max_submitters > 64)
    return "registry capacities out of range";
  if ((h.inj_cap & (h.inj_cap - 1)) != 0 || h.inj_cap < 2 ||
      (h.deque_cap & (h.deque_cap - 1)) != 0 || h.deque_cap < 2 ||
      (h.slot & (h.slot - 1)) != 0 || h.slot < 128 || h.slot > (1u << 20))
    return "queue capacities or slot size are not valid powers of two";
  if (h.result_slots == 0 || h.result_slots % h.max_submitters != 0)
    return "result slots are not a multiple of the submitter capacity";
  if (pool_fixed_size(&h) > region_size)
    return "pool sections exceed the mapped region";
  if (h.livedir_offset > region_size ||
      h.livedir_size > region_size - h.livedir_offset ||
      h.livedir_size == 0 || h.livedir_size > 900)
    return "liveness-dir string is missing or lies outside the region";
  if (out != NULL) *out = h;
  return NULL;
}

// Parkers and liveness paths -----------------------------------------------------------

static rei_parker *pool_wk_pk(rei_pool *p, uint32_t i) {
  return &p->pks[i];
}

static rei_parker *pool_sub_pk(rei_pool *p, uint32_t j) {
  return &p->pks[p->hdr.max_workers + j];
}

/* Entity numbering (the Windows event-name key): workers 0..MW-1, submitters
   MW..MW+MS-1. Every participant attaches every entity's parker up front —
   submitters unpark workers, workers unpark submitters — with create = 1 only
   on the controller, before any spawn. */
static int pool_parkers_attach(rei_pool *p, int create) {
  uint32_t mw = p->hdr.max_workers, ms = p->hdr.max_submitters;
  p->pks = calloc(mw + ms, sizeof(rei_parker));
  if (p->pks == NULL) return -1;
  for (uint32_t i = 0; i < mw + ms; i++) {
    _Atomic uint32_t *epoch = i < mw ? &p->wk[i].park_epoch
                                     : &p->sub[i - mw].park_epoch;
    if (rei_parker_attach(&p->pks[i], epoch, p->shm.name, (int) i,
                          create) != 0) {
      for (uint32_t k = 0; k < i; k++) rei_parker_detach(&p->pks[k]);
      free(p->pks);
      p->pks = NULL;
      return -1;
    }
  }
  p->pk_ok = 1;
  return 0;
}

/* kind is "wk" / "sub" (with idx) or "owner" (idx ignored). All paths live in
   the controller-chosen directory recorded in the header — no participant
   ever resolves the directory independently. */
static int pool_live_path(rei_pool *p, char *buf, size_t size,
                          const char *kind, uint32_t idx) {
  const char *suffix = p->shm.name + strlen(REI_PREFIX_LITERAL);
  int n = strcmp(kind, "owner") == 0 ?
    snprintf(buf, size, "%s/rei_%s.owner", p->livedir, suffix) :
    snprintf(buf, size, "%s/rei_%s.%s.%u", p->livedir, suffix, kind, idx);
  return (n > 0 && (size_t) n < size) ? 0 : -1;
}

// Forward declarations for the cross-section call graph ------------------------------

static void pool_unpark_result_waiter(rei_pool *p, rei_rs_hdr *rs);
static void pool_unpark_one_worker(rei_pool *p);
static int pool_reaping_free(rei_wk_slot *w);
static int pool_probe_worker(rei_pool *p, uint32_t slot);
static void pool_probe_submitter(rei_pool *p, uint32_t j);
static void pool_orphan_teardown_try(rei_pool *p);
static void pool_idle_sweep(rei_pool *p);
static int pool_execute(rei_pool *p, int catching);
static void pool_announce(rei_pool *p);
static void pool_stats_publish(rei_pool *p);
static int pool_watch_workers(rei_pool *p, const uint32_t *slots, size_t n);
static int pool_watch_owner(rei_pool *p, rei_parker *own_pk);

// Release ----------------------------------------------------------------------------

/* Full teardown of a handle's process-local state, idempotent. Never
   unlinks: the region name and liveness files are removed only by the
   controller's stop / destroy protocol. Order is load-bearing, as in the
   channel: the death watch and parkers reference the mapping. */
static void pool_release(rei_pool *p) {
  if (p->released) return;
  p->released = 1;
  if (p->watch != NULL) {
    rei_death_watch_stop(p->watch);
    p->watch = NULL;
  }
  if (p->wk_watch != NULL) {
    /* stop synchronizes with in-flight reap callbacks: after this loop
       nothing touches the mapping from another thread */
    for (uint32_t i = 0; i < p->hdr.max_workers; i++)
      if (p->wk_watch[i] != NULL) {
        rei_death_watch_stop(p->wk_watch[i]);
        p->wk_watch[i] = NULL;
      }
  }
  if (p->pk_ok) {
    uint32_t n = p->hdr.max_workers + p->hdr.max_submitters;
    for (uint32_t i = 0; i < n; i++) rei_parker_detach(&p->pks[i]);
    p->pk_ok = 0;
  }
  free(p->pks);
  p->pks = NULL;
  /* release the retain tables, the free list + ledger, and the mapping
     cache (their regions are independent of the pool region) */
  rei_stage_rollback(&p->h);
  rei_keepers_teardown(&p->h, p->keepers, p->keepers_n);
  rei_keepers_teardown(&p->h, p->sub_keepers, p->sub_keepers_n);
  rei_spill_fl_teardown(&p->h.fl);
  rei_oc_teardown(&p->h.oc);
  if (p->shm.addr != NULL) rei_shm_close_stack(&p->shm, 0);
  p->base = NULL;
  if (p->live_self != 0) {
    rei_live_close(p->live_self);
    p->live_self = 0;
  }
  if (p->live_sub != 0) {
    rei_live_close(p->live_sub);
    p->live_sub = 0;
  }
  if (p->live_owner != 0) {
    rei_live_close(p->live_owner);
    p->live_owner = 0;
  }
  if (p->live_all != NULL) {
    uint32_t n = p->hdr.max_workers + p->hdr.max_submitters;
    for (uint32_t i = 0; i < n; i++)
      if (p->live_all[i] != 0) rei_live_close(p->live_all[i]);
    free(p->live_all);
    p->live_all = NULL;
  }
}

/* The controller half of teardown, shared by rei_pool_stop, the create
   walk-back, and destroy: broadcast shutdown, wake everyone, cancel every
   pending result, unlink the names. Waiting for workers is the caller's
   business (destroy cannot wait). */
static void pool_shutdown_broadcast(rei_pool *p) {
  atomic_store_explicit(p->shutdown, 1u, memory_order_seq_cst);
  /* no parkers means a create walked back before attaching them: the token
     never left the process, so there is nobody to wake (and p->pks is NULL) */
  if (!p->pk_ok) return;
  for (uint32_t i = 0; i < p->hdr.max_workers; i++)
    rei_unpark(pool_wk_pk(p, i));
  for (uint32_t j = 0; j < p->hdr.max_submitters; j++)
    if (atomic_load_explicit(p->full_waiters, memory_order_acquire) &
        (1ull << j))
      rei_unpark(pool_sub_pk(p, j));
  for (uint32_t r = 0; r < p->hdr.result_slots; r++) {
    rei_rs_hdr *rs = pool_rs(p, r);
    int32_t expected = REI_RS_PENDING;
    if (atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                                REI_RS_CANCEL,
                                                memory_order_seq_cst,
                                                memory_order_relaxed)) {
      int32_t ws = atomic_load_explicit(&rs->waiter_slot,
                                        memory_order_acquire);
      if (ws >= 0 && (uint32_t) ws < p->hdr.max_submitters)
        rei_unpark(pool_sub_pk(p, (uint32_t) ws));
    }
  }
}

static void pool_remove_live_files(rei_pool *p);

static void pool_unlink_names(rei_pool *p) {
  rei_region_unlink(&p->shm);
  pool_remove_live_files(p);
}

/* Create/attach walk-back: the handle never escapes, so the caller reads
   the error from the thread-local slot. The create half also unlinks (the
   region and liveness files are ours); the open half never does. */
static void pool_failed(rei_pool *p, int created) {
  rei_err_record_tls(p->h.errcat, "%s", p->h.errmsg);
  if (created) {
    pool_shutdown_broadcast(p);
    pool_unlink_names(p);
  }
  pool_release(p);
  free(p->scratch);
  free(p->keepers);
  free(p->live_all);
  free(p);
}

// Handle access ---------------------------------------------------------------------

static rei_pool *pool_get(rei_pool *p) {
  if (p == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "pool handle is closed");
    return NULL;
  }
  if (p->released) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "pool handle is closed");
    return NULL;
  }
  if (p->self_pid != rei_self_pid()) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "pool handles do not survive fork()");
    return NULL;
  }
  return p;
}

static rei_status pool_intr(rei_pool *p) {
  rei_err_record(&p->h, REI_ERRCAT_INTERRUPTED, "interrupted");
  return REI_ERR;
}

/* An error raised by the hook is an infrastructure failure at its site:
   at submit the task stays committed (the dropped handle's cancel covers
   it); in the worker loop it takes the worker down — its stranded
   in-flight task fails through the ordinary death path. */
static void pool_trace_emit(rei_pool *p, rei_trace_event event,
                            uint64_t id) {
  if (p->trace != NULL) p->trace(event, id, p->trace_ctx);
}

rei_status rei_pool_set_trace(rei_pool *p, rei_trace_fn fn, void *ctx) {
  if (pool_get(p) == NULL) return REI_ERR;
  p->trace = fn;
  p->trace_ctx = ctx;
  return REI_OK;
}

// Create (controller) -----------------------------------------------------------------

static int rei_pow2_u64(uint64_t v) {
  return v != 0 && (v & (v - 1)) == 0;
}

rei_status rei_pool_create(rei_pool **out, const rei_pool_opts *opts,
                           const rei_binding *b) {
  *out = NULL;
  if (b == NULL || b->stage == NULL || b->read == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "a pool binding needs stage and read callbacks");
    return REI_ERR;
  }
  uint64_t maxw = opts->max_workers;
  uint64_t maxs = opts->max_submitters;
  uint64_t inj_cap = opts->injection_cap;
  uint64_t deque_cap = opts->per_worker_cap;
  uint64_t rslots = opts->result_slots;
  uint64_t slot = opts->slot_size;
  if (maxw < 1 || maxw > REI_MAX_WORKERS) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "max_workers must be between 1 and %d",
                       REI_MAX_WORKERS);
    return REI_ERR;
  }
  if (maxs < 1 || maxs > 64) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "max_submitters must be between 1 and 64");
    return REI_ERR;
  }
  if (!rei_pow2_u64(inj_cap) || inj_cap < 2 || inj_cap > ((uint64_t) 1 << 24)) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "injection_cap must be a power of two between 2 and 2^24");
    return REI_ERR;
  }
  if (!rei_pow2_u64(deque_cap) || deque_cap < 2 || deque_cap > ((uint64_t) 1 << 24)) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "per_worker_cap must be a power of two between 2 and 2^24");
    return REI_ERR;
  }
  /* floor 128: a result slot's inline budget (slot - 40) must hold a
     region name (up to 27 bytes on Windows) for an SHM_RAW spill */
  if (!rei_pow2_u64(slot) || slot < 128 || slot > ((uint64_t) 1 << 20)) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "slot_size must be a power of two between 128 and 2^20");
    return REI_ERR;
  }
  if (rslots < maxs || rslots > ((uint64_t) 1 << 24)) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "result_slots must be between max_submitters and 2^24");
    return REI_ERR;
  }
  rslots = (rslots + maxs - 1) / maxs * maxs;   /* per-submitter partition */
  const char *livedir = rei_live_dir();
  if (livedir == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "cannot resolve liveness lock directory");
    return REI_ERR;
  }
  size_t livedir_len = strlen(livedir);
  if (livedir_len > 900) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "liveness directory path too long");
    return REI_ERR;
  }

  rei_pool_hdr h = {
    .magic = REI_POOL_MAGIC,
    .version = REI_ABI_VERSION,
    .max_workers = (uint32_t) maxw,
    .max_submitters = (uint32_t) maxs,
    .inj_cap = (uint32_t) inj_cap,
    .deque_cap = (uint32_t) deque_cap,
    .result_slots = (uint32_t) rslots,
    .slot = (uint32_t) slot,
    .owner_pid = (uint64_t) rei_self_pid(),
  };
  uint64_t fixed = pool_fixed_size(&h);
  h.livedir_offset = REI_ALIGN64(fixed);
  h.livedir_size = livedir_len;
  uint64_t total = h.livedir_offset + livedir_len;
  if (total > ((uint64_t) 1 << 46)) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "pool region too large");
    return REI_ERR;
  }

  rei_pool *p = calloc(1, sizeof(*p));
  if (p == NULL) {
    rei_err_record_tls(REI_ERRCAT_NOMEMORY, "allocation failure");
    return REI_ERR;
  }
  int rc = rei_shm_create_populate(&p->shm, (size_t) total);
  if (rc != REI_ERRCAT_NONE) {
    const char *summary, *hint;
    rei_err_describe((rei_errcat) rc, &summary, &hint);
    rei_err_record_tls((rei_errcat) rc,
                       "cannot create pool region (%llu bytes): %s%s%s",
                       (unsigned long long) total, summary,
                       hint[0] != '\0' ? ". " : "", hint);
    free(p);
    return REI_ERR;
  }
  p->h.htype = REI_HTYPE_POOL;
  p->h.binding = *b;
  rei_read_tmpl_init(&p->h);
  p->role = REI_ROLE_CONTROLLER;
  p->self_pid = rei_self_pid();
  p->wk_slot = -1;
  p->sub_slot = 0;
  p->scan_budget_ns = REI_SPIN_BUDGET_NS;
  p->collect_budget_ns = REI_COLLECT_SPIN_BUDGET_NS;
  p->hdr = h;
  memcpy(p->livedir, livedir, livedir_len + 1);

  p->keepers = calloc((size_t) (rslots / maxs), sizeof(rei_keeper));
  p->live_all = calloc(maxw + maxs, sizeof(intptr_t));
  if (p->keepers == NULL || p->live_all == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_NOMEMORY, "allocation failure");
    pool_failed(p, 1);
    return REI_ERR;
  }
  p->keepers_n = (uint32_t) (rslots / maxs);

  unsigned char *base = (unsigned char *) p->shm.addr;
  memcpy(base, &h, sizeof(h));
  memcpy(base + h.livedir_offset, livedir, livedir_len);
  pool_wire(p);

  /* Host-assigned worker-slot geometry, written once before any spawn. The
     deque space feeds the stealing tier; a fresh region is zero-filled, so
     every status word starts FREE and every index at 0. */
  size_t deques_off = (size_t) (p->rings - base) +
    (size_t) h.max_submitters * pool_ring_bytes(&h);
  for (uint32_t i = 0; i < h.max_workers; i++) {
    p->wk[i].id = (int32_t) i;
    p->wk[i].deque_buf_off =
      (int64_t) (deques_off + (size_t) i * ((size_t) h.deque_cap * h.slot));
    p->wk[i].deque_cap = (int32_t) h.deque_cap;
    atomic_store_explicit(&p->wk[i].in_flight_rs, -1, memory_order_relaxed);
  }

  /* Liveness: create every file up front and keep the fds — the controller
     always probes on kept fds — then take the owner lock before any spawn
     (pool lifetime = creator lifetime) and the submitter-slot-0 lock. */
  char path[1024];
  for (uint32_t i = 0; i < h.max_workers + h.max_submitters; i++) {
    int is_wk = i < h.max_workers;
    if (pool_live_path(p, path, sizeof(path), is_wk ? "wk" : "sub",
                       is_wk ? i : i - h.max_workers) != 0) {
      rei_err_record(&p->h, REI_ERRCAT_OTHER, "liveness file path too long");
      pool_failed(p, 1);
      return REI_ERR;
    }
    if (rei_live_open(path, &p->live_all[i]) != 0) {
      rei_err_record(&p->h, REI_ERRCAT_OTHER,
                     "cannot create liveness file '%s'", path);
      pool_failed(p, 1);
      return REI_ERR;
    }
  }
  if (pool_live_path(p, path, sizeof(path), "owner", 0) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "liveness file path too long");
    pool_failed(p, 1);
    return REI_ERR;
  }
  if (rei_live_open(path, &p->live_owner) != 0 ||
      rei_live_try(p->live_owner) != REI_LIVE_ACQUIRED) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "cannot lock owner liveness file '%s'", path);
    pool_failed(p, 1);
    return REI_ERR;
  }

  /* Windows: every entity's named parker event must exist before any spawn */
  if (pool_parkers_attach(p, 1) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "cannot attach pool parkers");
    pool_failed(p, 1);
    return REI_ERR;
  }

  /* Claim submitter slot 0 for the calling process: lock-before-CAS, as in
     every join. */
  p->live_self = p->live_all[h.max_workers + 0];
  if (rei_live_try(p->live_self) != REI_LIVE_ACQUIRED) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "cannot lock submitter liveness file");
    pool_failed(p, 1);
    return REI_ERR;
  }
  rei_sub_slot *s0 = &p->sub[0];
  int32_t expected = REI_SUB_FREE;
  if (!atomic_compare_exchange_strong_explicit(&s0->status, &expected,
                                               REI_SUB_LIVE,
                                               memory_order_seq_cst,
                                               memory_order_relaxed)) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "submitter slot 0 is not free in a fresh region");
    pool_failed(p, 1);
    return REI_ERR;
  }
  s0->pid = (int64_t) p->self_pid;
  s0->rs_start = 0;
  s0->rs_count = (uint32_t) (rslots / maxs);
  rei_live_ident(p->live_self, &s0->live_dev, &s0->live_ino);
  p->rs_cursor = 0;

  *out = p;
  return REI_OK;
}

/* The join token is the region name past the namespace prefix:
   "<pid hex>_<counter hex>". */
rei_status rei_pool_token(const rei_pool *p, char *buf, size_t cap) {
  if (p == NULL || p->released) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "pool handle is closed");
    return REI_ERR;
  }
  const char *suffix = p->shm.name + strlen(REI_PREFIX_LITERAL);
  int n = snprintf(buf, cap, "%s", suffix);
  if (n <= 0 || (size_t) n >= cap) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "token buffer too small");
    return REI_ERR;
  }
  return REI_OK;
}

/* Startup / elastic-spawn rendezvous: park on the creator's submitter-0
   parker, re-checking each target slot for LIVE on each wake; workers
   unpark the creator on reaching LIVE. REI_TIMEOUT on expiry. */
rei_status rei_pool_ready_wait(rei_pool *p, const uint32_t *slots,
                               size_t n, double timeout_ms) {
  if (pool_get(p) == NULL) return REI_ERR;
  if (p->role != REI_ROLE_CONTROLLER) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "only the controller can wait for workers");
    return REI_ERR;
  }
  if (n > 0 && slots == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "expected worker slot indices");
    return REI_ERR;
  }
  for (size_t i = 0; i < n; i++)
    if (slots[i] >= p->hdr.max_workers) {
      rei_err_record(&p->h, REI_ERRCAT_OTHER,
                     "worker slot index out of range");
      return REI_ERR;
    }
  double deadline = timeout_ms < 0 ? -1 : rei_now() + timeout_ms / 1000;
  int timed_out = 0;
  for (;;) {
    uint32_t e = rei_parker_snapshot(pool_sub_pk(p, 0));
    size_t live = 0;
    for (size_t i = 0; i < n; i++)
      live += atomic_load_explicit(&p->wk[slots[i]].status,
                                   memory_order_acquire) == REI_WK_LIVE;
    if (live == n) break;
    long ms = REI_INTERRUPT_BOUND_MS;
    if (deadline >= 0) {
      double rem = deadline - rei_now();
      if (rem <= 0) {
        timed_out = 1;
        break;
      }
      long rem_ms = (long) (rem * 1000) + 1;
      if (rem_ms < ms) ms = rem_ms;
    }
    rei_park_bracket(&p->h.binding, 1);
    rei_park(pool_sub_pk(p, 0), e, ms);
    rei_park_bracket(&p->h.binding, 0);
    if (rei_check_interrupt(&p->h.binding)) return pool_intr(p);
  }
  /* On the way out — success or deadline expiry — point the death
     listener at whichever targets did join (non-LIVE and in-process slots
     are skipped). An interrupt abandon skips the arming, as the binding's
     longjmp would. */
  if (pool_watch_workers(p, slots, n) != 0) return REI_ERR;
  return timed_out ? REI_TIMEOUT : REI_OK;
}

/* Clean-exit request: the worker observes the word between tasks and takes
   its LEAVING path — non-blocking here, and never preemptive. Its deque is
   consumed in place (REAPING) and the process may linger as a lifetime
   anchor for uncollected results. */
rei_status rei_pool_retire(rei_pool *p, uint32_t slot) {
  if (pool_get(p) == NULL) return REI_ERR;
  if (p->role != REI_ROLE_CONTROLLER) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "only the controller can retire workers");
    return REI_ERR;
  }
  if (slot >= p->hdr.max_workers) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "worker slot index out of range");
    return REI_ERR;
  }
  if (atomic_load_explicit(&p->wk[slot].status, memory_order_acquire) !=
      REI_WK_LIVE) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "worker slot %u is not live",
                   slot);
    return REI_ERR;
  }
  atomic_store_explicit(&p->wk[slot].retire, 1, memory_order_seq_cst);
  rei_unpark(pool_wk_pk(p, slot));
  return REI_OK;
}

/* The GC-finalizer target: idempotent, never blocks. On a controller,
   broadcast so a late-joining worker exits instead of parking against a
   pool that went away, then unlink everything. */
void rei_pool_destroy(rei_pool *p) {
  if (p == NULL) return;
  if (!p->released && p->role == REI_ROLE_CONTROLLER && p->base != NULL) {
    pool_shutdown_broadcast(p);
    pool_unlink_names(p);
  }
  pool_release(p);
  free(p->scratch);
  free(p->rk);
  free(p->rk_pos);
  free(p->keepers);
  free(p->sub_keepers);
  free(p->wk_watch);
  free((void *) p->wk_dead);
  free(p->reap_ctx);
  free(p);
}

// Attach helpers ----------------------------------------------------------------------

/* Shared open: validate the token and the region header, allocate the
   handle and its retain table, wire the layout. keeper_len_is_rs selects
   the worker's result table (result_slots) over the submitter's task
   table (result_slots / max_submitters). NULL + the thread-local error
   slot on failure. */
static rei_pool *pool_open_common(const char *token, const rei_binding *b,
                                  int keeper_len_is_rs) {
  if (!rei_token_valid(token)) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "malformed region-name suffix");
    return NULL;
  }
  char name[REI_NAME_MAX];
  int nn = snprintf(name, sizeof(name), "%s%s", REI_PREFIX_LITERAL, token);
  if (nn <= 0 || (size_t) nn >= sizeof(name)) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "malformed region-name suffix");
    return NULL;
  }

  rei_pool *p = calloc(1, sizeof(*p));
  if (p == NULL) {
    rei_err_record_tls(REI_ERRCAT_NOMEMORY, "allocation failure");
    return NULL;
  }
  if (rei_shm_open_rw_stack(&p->shm, name, 1) != 0) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "cannot open pool region '%s'",
                       name);
    free(p);
    return NULL;
  }
  p->h.htype = REI_HTYPE_POOL;
  p->h.binding = *b;
  rei_read_tmpl_init(&p->h);
  p->self_pid = rei_self_pid();
  p->wk_slot = -1;
  p->sub_slot = -1;
  p->scan_budget_ns = REI_SPIN_BUDGET_NS;
  p->collect_budget_ns = REI_COLLECT_SPIN_BUDGET_NS;

  /* validate before touching any other field */
  const char *err = rei_pool_hdr_validate(p->shm.addr, p->shm.size, &p->hdr);
  if (err != NULL) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "invalid pool region: %s", err);
    pool_failed(p, 0);
    return NULL;
  }

  uint32_t klen = keeper_len_is_rs ?
    p->hdr.result_slots : p->hdr.result_slots / p->hdr.max_submitters;
  p->keepers = calloc((size_t) klen, sizeof(rei_keeper));
  if (p->keepers == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_NOMEMORY, "allocation failure");
    pool_failed(p, 0);
    return NULL;
  }
  p->keepers_n = klen;

  pool_wire(p);
  memcpy(p->livedir, p->base + p->hdr.livedir_offset,
         (size_t) p->hdr.livedir_size);
  p->livedir[p->hdr.livedir_size] = '\0';
  return p;
}

/* Open the owner liveness file and keep the fd. A successful non-blocking
   acquire means the previous holder is dead — the probe simply refuses the
   join (closing the handle releases the momentarily-held lock). */
static rei_status pool_owner_check(rei_pool *p) {
  char path[1024];
  if (pool_live_path(p, path, sizeof(path), "owner", 0) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "liveness file path too long");
    return REI_ERR;
  }
  if (rei_live_open(path, &p->live_owner) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "cannot open owner liveness file '%s'", path);
    return REI_ERR;
  }
  if (atomic_load_explicit(p->shutdown, memory_order_acquire) != 0 ||
      rei_live_try(p->live_owner) == REI_LIVE_ACQUIRED) {
    rei_err_record(&p->h, REI_ERRCAT_STOPPED, "pool stopped or owner dead");
    return REI_ERR;
  }
  return REI_OK;
}

/* Lock-first claim of a FREE submitter slot, mirroring the worker join: a
   dead submitter is recognisable by its free liveness lock regardless of
   which side of the CAS it died on. Fills the slot's identity fields, sets
   p->sub_slot, and returns the held lock through *lock_out. Shared by
   rei_pool_attach and a worker's first nested submit. */
static rei_status pool_claim_sub_slot(rei_pool *p, intptr_t *lock_out) {
  char path[1024];
  uint32_t per = p->hdr.result_slots / p->hdr.max_submitters;
  for (uint32_t j = 0; j < p->hdr.max_submitters; j++) {
    if (atomic_load_explicit(&p->sub[j].status, memory_order_acquire) !=
        REI_SUB_FREE)
      continue;
    if (pool_live_path(p, path, sizeof(path), "sub", j) != 0) {
      rei_err_record(&p->h, REI_ERRCAT_OTHER, "liveness file path too long");
      return REI_ERR;
    }
    intptr_t h;
    if (rei_live_open(path, &h) != 0) continue;
    if (rei_live_try(h) != REI_LIVE_ACQUIRED) {
      rei_live_close(h);
      continue;                    /* another claimant beat us */
    }
    int32_t expected = REI_SUB_FREE;
    if (!atomic_compare_exchange_strong_explicit(&p->sub[j].status,
                                                 &expected, REI_SUB_LIVE,
                                                 memory_order_seq_cst,
                                                 memory_order_relaxed)) {
      rei_live_close(h);           /* stale FREE reading */
      continue;
    }
    *lock_out = h;
    p->sub_slot = (int) j;
    break;
  }
  if (p->sub_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "submitter registry full");
    return REI_ERR;
  }

  rei_sub_slot *me = &p->sub[p->sub_slot];
  me->pid = (int64_t) p->self_pid;
  me->rs_start = (uint32_t) p->sub_slot * per;
  me->rs_count = per;
  atomic_store_explicit(&me->stat_spills, 0, memory_order_relaxed);
  atomic_store_explicit(&me->stat_spill_reuse, 0, memory_order_relaxed);
  rei_live_ident(*lock_out, &me->live_dev, &me->live_ino);
  return REI_OK;
}

/* A worker's lazy nested-submitter claim: the slot plus its task-keeper
   table, on first use only, so pools that never nest spend no submitter
   slots on workers. */
static rei_status pool_sub_keepers_claim(rei_pool *p) {
  if (p->sub_slot >= 0) return REI_OK;
  if (pool_claim_sub_slot(p, &p->live_sub) != 0) return REI_ERR;
  uint32_t n = p->sub[p->sub_slot].rs_count;
  p->sub_keepers = calloc((size_t) n, sizeof(rei_keeper));
  if (p->sub_keepers == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_NOMEMORY, "allocation failure");
    return REI_ERR;
  }
  p->sub_keepers_n = n;
  return REI_OK;
}

// Worker join --------------------------------------------------------------------------

rei_status rei_pool_worker_join(rei_pool **out, const char *token,
                                uint32_t slot, const rei_binding *b) {
  *out = NULL;
  if (b == NULL || b->stage == NULL || b->read == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "a pool binding needs stage and read callbacks");
    return REI_ERR;
  }
  rei_pool *p = pool_open_common(token, b, 1);
  if (p == NULL) return REI_ERR;
  p->role = REI_ROLE_WORKER;

  if (slot >= p->hdr.max_workers) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "worker slot index out of range");
    pool_failed(p, 0);
    return REI_ERR;
  }
  if (pool_owner_check(p) != 0) {
    pool_failed(p, 0);
    return REI_ERR;
  }

  /* Lock-before-CAS: what makes "CLAIMING + free lock" a reliable
     dead-worker signal for the reaper, and what fail-fasts against a
     leftover ghost from a previous spawn. */
  char path[1024];
  if (pool_live_path(p, path, sizeof(path), "wk", slot) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "liveness file path too long");
    pool_failed(p, 0);
    return REI_ERR;
  }
  if (rei_live_open(path, &p->live_self) != 0 ||
      rei_live_try(p->live_self) != REI_LIVE_ACQUIRED) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "worker slot %u already held — stale spawn?", slot);
    pool_failed(p, 0);
    return REI_ERR;
  }
  rei_wk_slot *me = &p->wk[slot];
  int32_t expected = REI_WK_FREE;
  if (!atomic_compare_exchange_strong_explicit(&me->status, &expected,
                                               REI_WK_CLAIMING,
                                               memory_order_seq_cst,
                                               memory_order_relaxed)) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "worker slot %u not free — stale spawn?", slot);
    pool_failed(p, 0);
    return REI_ERR;
  }
  p->wk_slot = (int) slot;

  me->pid = (int64_t) p->self_pid;
  rei_live_ident(p->live_self, &me->live_dev, &me->live_ino);
  atomic_store_explicit(&me->in_flight_rs, -1, memory_order_relaxed);
  atomic_store_explicit(&me->retire, 0, memory_order_relaxed);
  atomic_store_explicit(&me->park_state, REI_WPK_RUNNING,
                        memory_order_relaxed);
  pool_stats_publish(p);   /* zero any previous incarnation's counters */

  if (pool_parkers_attach(p, 0) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "cannot attach pool parkers");
    pool_failed(p, 0);
    return REI_ERR;
  }
  p->scratch = malloc(p->hdr.slot);
  if (p->scratch == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_NOMEMORY, "allocation failure");
    pool_failed(p, 0);
    return REI_ERR;
  }
  p->rng = ((uint64_t) p->self_pid * 0x9E3779B97F4A7C15ull) ^
    ((uint64_t) (rei_now() * 1e9)) ^ ((uint64_t) slot << 32);
  if (p->rng == 0) p->rng = 1;
  if (pool_watch_owner(p, pool_wk_pk(p, slot)) != 0) {
    pool_failed(p, 0);
    return REI_ERR;
  }

  expected = REI_WK_CLAIMING;
  atomic_compare_exchange_strong_explicit(&me->status, &expected,
                                          REI_WK_LIVE,
                                          memory_order_seq_cst,
                                          memory_order_relaxed);
  rei_unpark(pool_sub_pk(p, 0));   /* the creator's startup wait */

  *out = p;
  return REI_OK;
}

/* Clean worker exit. A nonempty deque is never drained anywhere: it
   becomes an ordinary steal target while the slot reads REAPING, and the
   observer of the drained deque returns the slot to FREE. Kept results are
   abandoned only when the handle releases here — the retiree's lame-duck
   loop drives that from the binding while results anchor region
   lifetimes. */
rei_status rei_pool_leave(rei_pool *p) {
  if (p == NULL || p->released) return REI_OK;
  if (p->role != REI_ROLE_WORKER || p->wk_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a worker handle");
    return REI_ERR;
  }
  rei_wk_slot *me = &p->wk[p->wk_slot];
  pool_stats_publish(p);   /* final, exact mirror for the departed slot */
  atomic_fetch_and_explicit(p->parked_workers, ~(1ull << p->wk_slot),
                            memory_order_seq_cst);
  int32_t expected = REI_WK_LIVE;
  if (atomic_compare_exchange_strong_explicit(&me->status, &expected,
                                              REI_WK_LEAVING,
                                              memory_order_seq_cst,
                                              memory_order_relaxed)) {
    /* walk the deque read-only, unparking each entry's result waiter;
       thieves may be advancing top concurrently — a wake for an
       already-stolen entry is a spurious wake, absorbed by the re-check */
    int64_t t = atomic_load_explicit(&me->deque_top, memory_order_acquire);
    int64_t b = atomic_load_explicit(&me->deque_bottom,
                                     memory_order_acquire);
    for (int64_t i = t; i < b; i++) {
      rei_entry_hdr *eh = (rei_entry_hdr *) deque_entry_at(p, me, i);
      if (eh->rs_index < p->hdr.result_slots)
        pool_unpark_result_waiter(p, pool_rs(p, eh->rs_index));
    }
    if (atomic_load_explicit(&me->deque_top, memory_order_acquire) >= b) {
      expected = REI_WK_LEAVING;
      atomic_compare_exchange_strong_explicit(&me->status, &expected,
                                              REI_WK_FREE,
                                              memory_order_seq_cst,
                                              memory_order_relaxed);
    } else {
      expected = REI_WK_LEAVING;
      atomic_compare_exchange_strong_explicit(&me->status, &expected,
                                              REI_WK_REAPING,
                                              memory_order_seq_cst,
                                              memory_order_relaxed);
      /* a thief that emptied the deque while we still read LEAVING saw
         nothing to free: re-check now that REAPING is published */
      if (!pool_reaping_free(me))
        pool_unpark_one_worker(p);
    }
  }
  /* the slot's own lock releases now, so the slot reads properly departed
     to any prober; the full release is deferred while kept results anchor
     region lifetimes — the retiree's lame-duck loop drives it. The sweep
     makes the anchor check exact: the step loop's busy-path reap is
     quota-bounded and may leave consumed records behind */
  if (p->live_self != 0) {
    rei_live_close(p->live_self);
    p->live_self = 0;
  }
  pool_idle_sweep(p);
  if (p->rk_n == 0) pool_release(p);
  return REI_OK;
}

/* One lame-duck beat for a retired worker anchoring uncollected results:
   reap the keeper table and report whether the anchor may drop. Plain
   bounded sleeps drive this from the binding — the slot's parker may
   already be reclaimed by a respawn, so no unpark can reach this process —
   and shutdown or owner death ends the linger. */
int rei_pool_lame_duck(rei_pool *p) {
  if (p == NULL || p->released) return 1;
  pool_idle_sweep(p);
  if (p->rk_n == 0 ||
      atomic_load_explicit(p->shutdown, memory_order_acquire) != 0 ||
      atomic_load_explicit(&p->owner_dead, memory_order_acquire) != 0) {
    pool_release(p);
    return 1;
  }
  return 0;
}

// Submitter join ------------------------------------------------------------------------

rei_status rei_pool_attach(rei_pool **out, const char *token,
                           const rei_binding *b) {
  *out = NULL;
  if (b == NULL || b->stage == NULL || b->read == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "a pool binding needs stage and read callbacks");
    return REI_ERR;
  }
  rei_pool *p = pool_open_common(token, b, 0);
  if (p == NULL) return REI_ERR;
  p->role = REI_ROLE_SUBMITTER;
  if (pool_owner_check(p) != 0) {
    pool_failed(p, 0);
    return REI_ERR;
  }
  if (pool_claim_sub_slot(p, &p->live_self) != 0) {
    pool_failed(p, 0);
    return REI_ERR;
  }
  if (pool_parkers_attach(p, 0) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "cannot attach pool parkers");
    pool_failed(p, 0);
    return REI_ERR;
  }
  if (pool_watch_owner(p, pool_sub_pk(p, (uint32_t) p->sub_slot)) != 0) {
    pool_failed(p, 0);
    return REI_ERR;
  }
  unsigned char *ring = pool_ring(p, (uint32_t) p->sub_slot);
  p->inj_ltail = atomic_load_explicit(ring_tail(ring), memory_order_acquire);
  p->inj_cached_head = atomic_load_explicit(ring_head(ring),
                                            memory_order_acquire);

  *out = p;
  return REI_OK;
}

// Submit --------------------------------------------------------------------------------

/* Directed-unpark half of the pusher wake: find a parked (or announcing)
   worker and unpark it. Returns 0 only when the mask read empty — every
   worker busy. Deque pushes (nested submit, help re-home, orphan drains)
   use this half alone: help beats scan injection rings only, so a
   doorbell rung for deque work buys nothing but one wasted no-op beat at
   some runner's next transition — idle workers reach deque work through
   their steal tiers, and the pre-park rescan (pool_any_work) guarantees
   nobody parks past it. The explicit-start variants exist for the reap
   paths, which run on the death listener's callback thread and must not
   touch the handle's process-local scan rotation. */
static int pool_unpark_worker_from(rei_pool *p, uint32_t start) {
  /* pusher protocol: push, fence, then the mask load — either the parking
     worker's rescan sees the push or we see its bit */
  atomic_thread_fence(memory_order_seq_cst);
  uint64_t w = atomic_load_explicit(p->parked_workers, memory_order_relaxed);
  if (w == 0) return 0;
  uint32_t mw = p->hdr.max_workers;
  for (uint32_t k = 0; k < mw; k++) {
    uint32_t i = (start + k) % mw;
    if (!(w & (1ull << i))) continue;
    int32_t expected = REI_WPK_PARKED;
    if (atomic_compare_exchange_strong_explicit(&p->wk[i].park_state,
                                                &expected, REI_WPK_WAKING,
                                                memory_order_seq_cst,
                                                memory_order_relaxed)) {
      rei_unpark(pool_wk_pk(p, i));
      return 1;
    }
    if (expected == REI_WPK_IDLE) {
      /* announced but not yet parked — its rescan may already have missed
         this publish, so an uncontested wake is not safe to skip. Its epoch
         snapshot predates the announce, so this unpark turns the upcoming
         sleep into an immediate return; at worst one spurious wake. */
      rei_unpark(pool_wk_pk(p, i));
      return 1;
    }
    /* RUNNING or WAKING: the worker transitioned away or another pusher
       claimed the wake; try the next set bit */
  }
  return 1;   /* someone was mid-transition: no doorbell, as ever */
}

/* The injection-publish wake: the unpark half, falling through to the
   doorbell when every worker is busy — map runners poll it once per batch
   transition, so queued injection work is picked up within ~a batch
   instead of at map end (rei_pool_help_once consumes it). */
static void pool_wake_one_worker_from(rei_pool *p, uint32_t start) {
  if (!pool_unpark_worker_from(p, start))
    atomic_store_explicit(p->help_wanted, 1u, memory_order_seq_cst);
}

static void pool_wake_one_worker(rei_pool *p) {
  pool_wake_one_worker_from(p, p->scan_start++);
}

static void pool_unpark_one_worker(rei_pool *p) {
  (void) pool_unpark_worker_from(p, p->scan_start++);
}

/* Block until the submitter's own ring has space (announce-then-rescan on
   full_waiters, parked on the submitter's own parker, woken directly by the
   worker whose pop freed a slot) or the deadline passes. Space is checked
   against the producer-local cached head first — workers CAS the shared
   head once per claim, so a fresh load would miss once per submit; a stale
   cache only under-reports space and apparent-full refreshes it, as with
   the channel's cached_head. REI_FULL at the deadline. */
static rei_status pool_ring_space_wait(rei_pool *p, _Atomic int64_t *head,
                                       double timeout_ms) {
  if (p->inj_ltail - p->inj_cached_head < (int64_t) p->hdr.inj_cap)
    return REI_OK;
  p->inj_cached_head = atomic_load_explicit(head, memory_order_acquire);
  if (p->inj_ltail - p->inj_cached_head < (int64_t) p->hdr.inj_cap)
    return REI_OK;
  uint64_t bit = 1ull << p->sub_slot;
  double deadline = timeout_ms < 0 ? -1 : rei_now() + timeout_ms / 1000;
  for (;;) {
    uint32_t e = rei_parker_snapshot(pool_sub_pk(p, (uint32_t) p->sub_slot));
    atomic_fetch_or_explicit(p->full_waiters, bit, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    p->inj_cached_head = atomic_load_explicit(head, memory_order_acquire);
    if (p->inj_ltail - p->inj_cached_head < (int64_t) p->hdr.inj_cap) {
      atomic_fetch_and_explicit(p->full_waiters, ~bit, memory_order_seq_cst);
      return REI_OK;
    }
    if (atomic_load_explicit(p->shutdown, memory_order_acquire) != 0) {
      atomic_fetch_and_explicit(p->full_waiters, ~bit, memory_order_seq_cst);
      rei_err_record(&p->h, REI_ERRCAT_STOPPED, "pool stopped");
      return REI_ERR;
    }
    if (atomic_load_explicit(&p->owner_dead, memory_order_acquire) != 0) {
      atomic_fetch_and_explicit(p->full_waiters, ~bit, memory_order_seq_cst);
      pool_orphan_teardown_try(p);
      rei_err_record(&p->h, REI_ERRCAT_STOPPED, "pool stopped or owner dead");
      return REI_ERR;
    }
    long ms = REI_INTERRUPT_BOUND_MS;
    if (deadline >= 0) {
      double rem = deadline - rei_now();
      if (rem <= 0) {
        atomic_fetch_and_explicit(p->full_waiters, ~bit,
                                  memory_order_seq_cst);
        return REI_FULL;
      }
      long rem_ms = (long) (rem * 1000) + 1;
      if (rem_ms < ms) ms = rem_ms;
    }
    rei_park_bracket(&p->h.binding, 1);
    rei_park(pool_sub_pk(p, (uint32_t) p->sub_slot), e, ms);
    rei_park_bracket(&p->h.binding, 0);
    atomic_fetch_and_explicit(p->full_waiters, ~bit, memory_order_seq_cst);
    if (rei_check_interrupt(&p->h.binding)) return pool_intr(p);
  }
}

/* Handle-vs-slot sequence check. */
static int pool_seq_match(rei_rs_hdr *rs, uint64_t seq) {
  return atomic_load_explicit(&rs->sequence, memory_order_relaxed) == seq;
}

static void pool_unpark_result_waiter(rei_pool *p, rei_rs_hdr *rs) {
  int32_t ws = atomic_load_explicit(&rs->waiter_slot, memory_order_acquire);
  if (ws >= 0 && (uint32_t) ws < p->hdr.max_submitters)
    rei_unpark(pool_sub_pk(p, (uint32_t) ws));
}

/* Keeper-drop wake for the worker that produced a freed result slot, gated
   on the parked mask: a running worker's own reap visits consume the FREE,
   so only an announced (idle or parked) worker needs the syscall. Pairs
   with the step loop's announce-before-sweep order through the same fence
   protocol as pool_wake_one_worker_from: either this fence-then-load sees
   the announce bit, or the worker's post-announce keeper sweep sees the
   FREE. Pure C — the submitter reaper calls it off the binding's thread. */
static void pool_unpark_keeper_drop(rei_pool *p, int32_t w) {
  if (w < 0 || (uint32_t) w >= p->hdr.max_workers) return;
  atomic_thread_fence(memory_order_seq_cst);
  if (atomic_load_explicit(p->parked_workers, memory_order_relaxed) &
      (1ull << (uint32_t) w))
    rei_unpark(pool_wk_pk(p, (uint32_t) w));
}

/* Spill staging (SHM_RAW, SHM_VEC, or RAWSPILL) is the off-ramp from the
   inline fast path — a region per payload, recycled from the handle's free
   list when steady-state traffic permits. Counted against the task's
   submitter for task and result payloads alike, so the dump surfaces an
   undersized slot_size from either direction of the traffic; the reuse
   count alongside says how much of that spill traffic is churn-free. */
static void pool_count_spill(rei_pool *p, uint32_t sub_slot,
                             const rei_slot_hdr *ph) {
  if ((ph->kind == REI_KIND_SHM_RAW || ph->kind == REI_KIND_SHM_VEC ||
       ph->kind == REI_KIND_RAWSPILL) &&
      sub_slot < p->hdr.max_submitters) {
    atomic_fetch_add_explicit(&p->sub[sub_slot].stat_spills, 1,
                              memory_order_relaxed);
    if (p->h.fl.last_reused)
      atomic_fetch_add_explicit(&p->sub[sub_slot].stat_spill_reuse, 1,
                                memory_order_relaxed);
  }
}

/* Result slot from the submitter's own subrange, its previous task keeper
   dropped en route: reusing a FREE slot is the lazy release backstop for
   keepers no collect dropped (cancelled or never-collected tasks) — safe
   because FREE strictly implies the worker is done with the entry — and it
   runs before staging, so a surrendered spill region can be popped by the
   very payload that recycles the slot. An abandoned stage leaves the
   keeper dropped early, which any FREE slot already permits. */
static rei_status pool_alloc_rs(rei_pool *p, rei_keeper *keepers,
                                uint32_t *out) {
  rei_sub_slot *me = &p->sub[p->sub_slot];
  for (uint32_t k = 0; k < me->rs_count; k++) {
    uint32_t cand = (p->rs_cursor + k) % me->rs_count;
    if (atomic_load_explicit(&pool_rs(p, me->rs_start + cand)->status,
                             memory_order_acquire) == REI_RS_FREE) {
      /* the slot's previous consumer is the zc entry's ledger key (the
         worker-death backstop); -1 when it was never claimed */
      rei_keeper *old = &keepers[cand];
      if (old->kind == REI_KEEP_ZC)
        old->key =
          atomic_load_explicit(&pool_rs(p, me->rs_start + cand)->worker_slot,
                               memory_order_acquire);
      rei_keeper_release(&p->h, keepers, cand);
      *out = cand;
      return REI_OK;
    }
  }
  rei_err_record(&p->h, REI_ERRCAT_EXHAUSTED,
                 "result slots exhausted — collect or cancel outstanding "
                 "tasks first");
  return REI_ERR;
}

/* The handle carries the sequence about to be installed at commit, so
   until the bump it is simply stale. */
static void pool_mint_task(rei_pool *p, uint32_t rs_index, rei_task *out) {
  out->seq = atomic_load_explicit(&pool_rs(p, rs_index)->sequence,
                                  memory_order_relaxed) + 1;
  out->rs_index = rs_index;
  out->reserved = 0;
}

/* Commit point: commit the staged keeper, install the sequence, and open
   the slot as PENDING. Everything that can fail ran before this. */
static void pool_commit_rs(rei_pool *p, rei_keeper *keepers, uint32_t local,
                           rei_rs_hdr *rs) {
  rei_keeper_commit(&p->h, keepers, local);
  p->rs_cursor = local + 1;
  atomic_fetch_add_explicit(&rs->sequence, 1, memory_order_relaxed);
  atomic_store_explicit(&rs->waiter_slot, -1, memory_order_relaxed);
  atomic_store_explicit(&rs->worker_slot, -1, memory_order_relaxed);
  atomic_store_explicit(&rs->status, REI_RS_PENDING, memory_order_release);
}

static void pool_fill_entry(rei_pool *p, rei_entry_hdr *eh,
                            uint32_t rs_index, uint16_t flags) {
  eh->task_id = ((uint64_t) p->sub_slot << 48) | ++p->task_counter;
  eh->rs_index = rs_index;
  eh->submitter_slot = (uint16_t) p->sub_slot;
  eh->flags = flags;   /* assign, never OR: ring and deque slots are reused */
}

/* Worker-side nested submit: the local-deque push. The worker becomes a
   submitter on first use — same keeper-table discipline, keyed by its own
   subrange, claimed lazily so pools that never nest spend no submitter
   slots on workers. The entry is staged directly into the worker's own
   deque slot and published by the bottom store; a full deque executes the
   task inline instead (work-first), so nested submit never blocks. */
static rei_status pool_submit_nested(rei_pool *p, void *task_obj,
                                     uint16_t flags, rei_task *out) {
  if (p->wk_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a worker handle");
    return REI_ERR;
  }
  if (pool_sub_keepers_claim(p) != 0) return REI_ERR;
  rei_keeper *keepers = p->sub_keepers;
  rei_stage_rollback(&p->h);
  uint32_t local;
  if (pool_alloc_rs(p, keepers, &local) != 0) return REI_ERR;
  uint32_t rs_index = p->sub[p->sub_slot].rs_start + local;
  rei_rs_hdr *rs = pool_rs(p, rs_index);

  rei_wk_slot *w = &p->wk[p->wk_slot];
  int64_t b = atomic_load_explicit(&w->deque_bottom, memory_order_relaxed);
  int64_t t = atomic_load_explicit(&w->deque_top, memory_order_acquire);
  int inline_exec = b - t >= (int64_t) w->deque_cap;
  unsigned char *e = inline_exec ? p->scratch : deque_entry_at(p, w, b);
  rei_entry_hdr *eh = (rei_entry_hdr *) e;

  /* everything that can fail — staging, and the evaluator check the
     inline path needs — runs before any observable mutation: a failure
     leaves the entry unpublished and the slot FREE */
  if (inline_exec && p->h.binding.exec == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "no evaluator registered on this worker handle");
    return REI_ERR;
  }
  if (p->h.binding.stage(task_obj, &eh->ph, e + sizeof(rei_entry_hdr),
                         p->inline_entry, &p->h) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_STAGE, "staging failed");
    return REI_ERR;
  }
  pool_count_spill(p, (uint32_t) p->sub_slot, &eh->ph);
  pool_mint_task(p, rs_index, out);
  pool_commit_rs(p, keepers, local, rs);
  pool_fill_entry(p, eh, rs_index, flags);
  uint64_t tid = eh->task_id;
  if (!inline_exec) {
    atomic_store_explicit(&w->deque_bottom, b + 1, memory_order_release);
    if (b <= t) pool_unpark_one_worker(p);   /* empty -> non-empty */
    pool_trace_emit(p, REI_TRACE_SUBMIT, tid);
  } else {
    pool_trace_emit(p, REI_TRACE_SUBMIT, tid);
    pool_announce(p);
    if (pool_execute(p, 1) != 0) return REI_ERR;
  }
  return REI_OK;
}

/* Per-task core of the submitter path: result slot, staging, handle,
   commit, entry fill — everything except the space wait, the tail
   publish, and the wake, which the batch entry amortizes across a
   burst. The submit trace emits at stage time. */
static rei_status pool_submit1(rei_pool *p, rei_keeper *keepers,
                               unsigned char *ring, void *task_obj,
                               uint16_t flags, rei_task *out) {
  rei_stage_rollback(&p->h);
  uint32_t local;
  if (pool_alloc_rs(p, keepers, &local) != 0) return REI_ERR;
  uint32_t rs_index = p->sub[p->sub_slot].rs_start + local;
  rei_rs_hdr *rs = pool_rs(p, rs_index);

  /* staging can fail: nothing observable yet */
  unsigned char *e = ring_entry(p, ring, (uint64_t) p->inj_ltail);
  rei_entry_hdr *eh = (rei_entry_hdr *) e;
  if (p->h.binding.stage(task_obj, &eh->ph, e + sizeof(rei_entry_hdr),
                         p->inline_entry, &p->h) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_STAGE, "staging failed");
    return REI_ERR;
  }
  pool_count_spill(p, (uint32_t) p->sub_slot, &eh->ph);
  pool_mint_task(p, rs_index, out);
  pool_commit_rs(p, keepers, local, rs);
  pool_fill_entry(p, eh, rs_index, flags);
  p->inj_ltail++;
  pool_trace_emit(p, REI_TRACE_SUBMIT, eh->task_id);
  return REI_OK;
}

/* Injection publish: the tail store, then the ready bit, in that order. */
static void pool_inj_publish(rei_pool *p, unsigned char *ring) {
  atomic_store_explicit(ring_tail(ring), p->inj_ltail, memory_order_release);
  uint64_t bit = 1ull << p->sub_slot;
  if (!(atomic_load_explicit(p->inj_ready, memory_order_relaxed) & bit))
    atomic_fetch_or_explicit(p->inj_ready, bit, memory_order_seq_cst);
}

/* The shared submit core: the shutdown/owner gates, then the role
   dispatch. */
static rei_status pool_submit(rei_pool *p, void *task_obj, uint16_t flags,
                              rei_task *out, double timeout_ms) {
  if (atomic_load_explicit(p->shutdown, memory_order_acquire) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_STOPPED, "pool stopped");
    return REI_ERR;
  }
  if (atomic_load_explicit(&p->owner_dead, memory_order_acquire) != 0) {
    pool_orphan_teardown_try(p);
    rei_err_record(&p->h, REI_ERRCAT_STOPPED, "pool stopped or owner dead");
    return REI_ERR;
  }
  if (p->role == REI_ROLE_WORKER)
    return pool_submit_nested(p, task_obj, flags, out);
  if (p->sub_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a submitter handle");
    return REI_ERR;
  }

  unsigned char *ring = pool_ring(p, (uint32_t) p->sub_slot);
  rei_status st = pool_ring_space_wait(p, ring_head(ring), timeout_ms);
  if (st != REI_OK) return st;

  st = pool_submit1(p, p->keepers, ring, task_obj, flags, out);
  if (st != REI_OK) return st;
  pool_inj_publish(p, ring);
  pool_wake_one_worker(p);
  return REI_OK;
}

rei_status rei_pool_submit(rei_pool *p, void *task_obj, rei_task *out,
                           double timeout_ms) {
  return rei_pool_submit_flags(p, task_obj, 0, out, timeout_ms);
}

/* One crossing per burst. The tail store and ready bit publish per
   element, so workers drain as the burst stages and a burst larger than
   the ring cannot deadlock. Wakes keep the pusher half of the parker
   handshake: a cadence of every 64 publishes (never wider than the ring,
   so a parked worker is roused long before the ring can fill and the
   space wait always has a popper to unpark it), then one pass per worker
   after the last publish — a worker can still lose the park race
   mid-burst (its pre-park re-check read a stale tail), and only a wake
   paired with the final publish contains that race. Ring-full past the
   deadline mid-burst ends the batch early (REI_OK, *n_out < n); fatal
   outcomes raise, with the tasks already submitted staying valid and
   collectible. */
rei_status rei_pool_submit_batch_fn(rei_pool *p, rei_obj_supply supply,
                                    void *ctx, size_t n,
                                    rei_task *out, size_t *n_out,
                                    double timeout_ms) {
  *n_out = 0;
  if (pool_get(p) == NULL) return REI_ERR;
  if (n > 0 && (supply == NULL || out == NULL)) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "expected task objects and handle out-params");
    return REI_ERR;
  }
  if (atomic_load_explicit(p->shutdown, memory_order_acquire) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_STOPPED, "pool stopped");
    return REI_ERR;
  }
  if (atomic_load_explicit(&p->owner_dead, memory_order_acquire) != 0) {
    pool_orphan_teardown_try(p);
    rei_err_record(&p->h, REI_ERRCAT_STOPPED, "pool stopped or owner dead");
    return REI_ERR;
  }

  if (p->role == REI_ROLE_WORKER) {
    for (size_t i = 0; i < n; i++) {
      if (pool_submit_nested(p, supply(ctx, i), 0, &out[i]) != 0)
        return REI_ERR;
      *n_out = i + 1;
    }
    return REI_OK;
  }
  if (p->sub_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a submitter handle");
    return REI_ERR;
  }
  unsigned char *ring = pool_ring(p, (uint32_t) p->sub_slot);

  double deadline = timeout_ms < 0 ? -1 : rei_now() + timeout_ms / 1000;
  /* wake cadence: a power of two no wider than the ring (both are), so
     one wake lands within any ring-filling window */
  int64_t cad = (int64_t) p->hdr.inj_cap < 64 ? (int64_t) p->hdr.inj_cap : 64;
  size_t done = 0;
  for (size_t i = 0; i < n; i++) {
    double rem_ms = deadline < 0 ? -1 :
      (deadline - rei_now() > 0 ? (deadline - rei_now()) * 1000 : 0);
    rei_status st = pool_ring_space_wait(p, ring_head(ring), rem_ms);
    if (st == REI_FULL) break;
    if (st != REI_OK) {
      *n_out = done;
      return st;
    }
    st = pool_submit1(p, p->keepers, ring, supply(ctx, i), 0, &out[i]);
    if (st != REI_OK) {
      *n_out = done;
      return st;
    }
    done++;
    pool_inj_publish(p, ring);
    if ((i & (size_t) (cad - 1)) == 0)
      for (uint32_t k = 0; k < p->hdr.max_workers; k++)
        pool_wake_one_worker(p);
  }
  /* the protocol wake: pair the burst's last publish with a parked-mask
     check per worker, as the single submit pairs every publish */
  for (size_t k = 0; k < done && k < (size_t) p->hdr.max_workers; k++)
    pool_wake_one_worker(p);

  *n_out = done;
  return REI_OK;
}

/* The array form over the supply core. */
static void *pool_batch_array_supply(void *ctx, size_t i) {
  return ((void **) ctx)[i];
}

rei_status rei_pool_submit_batch(rei_pool *p, void **objs, size_t n,
                                 rei_task *out, size_t *n_out,
                                 double timeout_ms) {
  return rei_pool_submit_batch_fn(p, pool_batch_array_supply, objs, n, out,
                                  n_out, timeout_ms);
}

// Worker step ----------------------------------------------------------------------------

/* Drop a kept result once its slot has left OK/ERR (or been resequenced):
   the collector's or a cancel's FREE transition is the consumed-signal,
   its directed unpark what re-runs the reap on a parked worker. rk_pos
   keys the table by slot — at most one record per slot, updated in place
   when a reused slot republishes — so a stale record can never alias (and
   nil) a successor's keeper. Every exit from OK/ERR is a consumer-done
   signal (collect materialized, or a cancel / submitter reap freed an
   uncollectable slot), so a spilled result region surrenders to the free
   list here. Returns 1 when the record at i was retained, 0 when it was
   removed (the swapped-in tail record then sits at i). */
static int pool_rk_visit(rei_pool *p, uint32_t i) {
  rei_rs_hdr *rs = pool_rs(p, p->rk[i].idx);
  int32_t st = atomic_load_explicit(&rs->status, memory_order_acquire);
  uint64_t seq = atomic_load_explicit(&rs->sequence, memory_order_relaxed);
  if ((st == REI_RS_OK || st == REI_RS_ERR) && seq == p->rk[i].seq)
    return 1;
  rei_keeper_release(&p->h, p->keepers, p->rk[i].idx);
  p->rk_pos[p->rk[i].idx] = 0;
  p->rk_n--;
  if (i < p->rk_n) {
    p->rk[i] = p->rk[p->rk_n];
    p->rk_pos[p->rk[i].idx] = i + 1;
  }
  return 0;
}

/* The full sweep, for the idle and departure paths (pre-park, empty step
   returns, the lame-duck beat) where visiting every record costs nothing
   the pool feels. The idle path also runs the full lent-ledger sweep —
   the busy paths' quota'd sweeps clear only residue. */
static void pool_reap_result_keepers(rei_pool *p) {
  uint32_t i = 0;
  while (i < p->rk_n) i += (uint32_t) pool_rk_visit(p, i);
  p->rk_cursor = 0;
  rei_ledger_sweep(&p->h.fl, REI_LEDGER_MAX);
}

/* The idle-path sweep proper: the full keeper reap plus the binding's
   sweep hook (its idle cache drop). An idle or departing worker holds
   nothing it cannot re-stage. */
static void pool_idle_sweep(rei_pool *p) {
  pool_reap_result_keepers(p);
  if (p->h.binding.sweep != NULL) p->h.binding.sweep(p->h.binding.ctx);
}

/* The busy-path reap: at most REI_REAP_QUOTA visits under a rotating
   cursor, so a loaded worker's per-task reap cost is O(1) against any
   number of results outstanding — under a fire-then-collect backlog a
   whole-table scan would go quadratic. Consumption keeps pace as long as
   the quota exceeds the frees a task period can see; the idle-path sweep
   clears any residue. */
static void pool_reap_quota(rei_pool *p) {
  uint32_t lim = p->rk_n < REI_REAP_QUOTA ? p->rk_n : REI_REAP_QUOTA;
  for (uint32_t k = 0; k < lim && p->rk_n > 0; k++) {
    if (p->rk_cursor >= p->rk_n) p->rk_cursor = 0;
    p->rk_cursor += (uint32_t) pool_rk_visit(p, p->rk_cursor);
  }
  /* the busy-path ledger sweep, quota'd like the reap itself */
  rei_ledger_sweep(&p->h.fl, REI_REAP_QUOTA);
}

/* First use allocates the table and the slot -> record map, both bounded
   by result_slots (rk_pos keys one record per slot, so the table never
   grows past it). Runs before the publish CAS: an allocation failure
   after publish would leave a pinned keeper the reap never visits. */
static int pool_rk_reserve(rei_pool *p) {
  if (p->rk != NULL) return 0;
  p->rk = calloc(p->hdr.result_slots, sizeof(*p->rk));
  p->rk_pos = calloc(p->hdr.result_slots, sizeof(*p->rk_pos));
  if (p->rk == NULL || p->rk_pos == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_NOMEMORY, "allocation failure");
    return -1;
  }
  return 0;
}

static void pool_rk_add(rei_pool *p, uint32_t idx, uint64_t seq) {
  uint32_t pos = p->rk_pos[idx];
  if (pos != 0) {          /* reused slot: the record follows the new
                              incarnation — publish replaced the keeper */
    p->rk[pos - 1].seq = seq;
    return;
  }
  p->rk[p->rk_n].idx = idx;
  p->rk[p->rk_n].seq = seq;
  p->rk_pos[idx] = ++p->rk_n;
}

/* Announce-before-claim: a worker dying after a claim CAS but before
   recording the task would otherwise vanish it. Recorded from the entry
   copied into scratch, before any claim (ring-head CAS, deque-bottom
   commit, or steal CAS) is attempted; plain stores suffice — the only
   reader is a post-mortem reaper serialized by the liveness lock. */
static void pool_announce(rei_pool *p) {
  rei_entry_hdr *eh = (rei_entry_hdr *) p->scratch;
  rei_wk_slot *me = &p->wk[p->wk_slot];
  atomic_store_explicit(&me->in_flight_rs, (int32_t) eh->rs_index,
                        memory_order_relaxed);
  atomic_store_explicit(&me->in_flight_seq,
                        atomic_load_explicit(&pool_rs(p, eh->rs_index)->
                                             sequence,
                                             memory_order_relaxed),
                        memory_order_relaxed);
}

static void pool_announce_clear(rei_pool *p) {
  atomic_store_explicit(&p->wk[p->wk_slot].in_flight_rs, -1,
                        memory_order_relaxed);
}

/* Claims must copy before their CAS so a dying claimer leaves the entry
   untouched. Only the fixed header and its framed bytes are meaningful: a
   large inline slot would otherwise turn every claim into a slot-sized
   copy. A slot is rewritten only once head/top has moved past it, so a
   winning claim read a coherent header and hdr + len covers every
   meaningful byte; a loser may read a stale, torn one, but any
   slot-bounded copy is safe — it is discarded with the failed CAS. */
static size_t pool_entry_copy_bytes(rei_pool *p, const rei_entry_hdr *eh) {
  switch (eh->ph.kind) {
  case REI_KIND_NIL:
    return sizeof(*eh);
  case REI_KIND_INLINE:
  case REI_KIND_RAWVEC:
  case REI_KIND_STR1:
  case REI_KIND_SHM_RAW:
  case REI_KIND_SHM_VEC:
  case REI_KIND_REF:
    if (eh->ph.len <= p->inline_entry) return sizeof(*eh) + eh->ph.len;
    break;
  case REI_KIND_RAWSPILL:
    /* the framed bytes are the region name, its length in aux >> 8 */
    if ((eh->ph.aux >> 8) < REI_NAME_MAX)
      return sizeof(*eh) + (uint32_t) (eh->ph.aux >> 8);
    break;
  }
  return p->hdr.slot;               /* torn or foreign header: full slot */
}

static void pool_copy_entry(rei_pool *p, unsigned char *dst,
                            const unsigned char *src) {
  rei_entry_hdr hdr;
  memcpy(&hdr, src, sizeof(hdr));
  size_t n = pool_entry_copy_bytes(p, &hdr);
  memcpy(dst, src, n);
}

/* Mirror the local counters into the slot — only at the park announce,
   post-park, the fairness tick, step returns, and leave. Never per task:
   the stat_* fields share line 1 with deque_top, and a per-task write
   would reintroduce exactly the thief-CAS pingpong that line's layout
   avoids. Under load the mirrors lag by up to one tick (61 claims); a
   parked or departed worker's are exact. */
static void pool_stats_publish(rei_pool *p) {
  rei_wk_slot *me = &p->wk[p->wk_slot];
  atomic_store_explicit(&me->stat_tasks, p->st_tasks, memory_order_relaxed);
  atomic_store_explicit(&me->stat_steals, p->st_steals,
                        memory_order_relaxed);
  atomic_store_explicit(&me->stat_inj, p->st_inj, memory_order_relaxed);
  atomic_store_explicit(&me->stat_parks, p->st_parks, memory_order_relaxed);
  atomic_store_explicit(&me->stat_helps, p->st_helps, memory_order_relaxed);
}

/* Copy-then-CAS claim over the ring scan, ready-mask-gated on the fast
   path (use_mask) and unfiltered on the fairness tick. The mask is a hint
   only: a stale clear bit is repaired by the pre-park full rescan, so it
   costs one trip to the park path, never a lost task. */
static int pool_claim_rings(rei_pool *p, int use_mask) {
  uint64_t ready = ~0ull;
  if (use_mask) {
    ready = atomic_load_explicit(p->inj_ready, memory_order_acquire);
    if (ready == 0) return 0;
  }
  uint32_t ms = p->hdr.max_submitters;
  uint32_t start = p->scan_start;
  for (uint32_t k = 0; k < ms; k++) {
    uint32_t s = (start + k) % ms;
    if (!(ready & (1ull << s))) continue;
    unsigned char *ring = pool_ring(p, s);
    _Atomic int64_t *hd = ring_head(ring), *tl = ring_tail(ring);
    for (;;) {
      int64_t head = atomic_load_explicit(hd, memory_order_acquire);
      int64_t tail = atomic_load_explicit(tl, memory_order_acquire);
      if (head >= tail) {
        /* clear, then re-check: the producer's OR follows its tail publish,
           so a publish racing the clear is caught and the bit restored */
        if (use_mask) {
          atomic_fetch_and_explicit(p->inj_ready, ~(1ull << s),
                                    memory_order_seq_cst);
          if (atomic_load_explicit(tl, memory_order_acquire) >
              atomic_load_explicit(hd, memory_order_acquire))
            atomic_fetch_or_explicit(p->inj_ready, 1ull << s,
                                     memory_order_seq_cst);
        }
        break;
      }
      pool_copy_entry(p, p->scratch, ring_entry(p, ring, (uint64_t) head));
      rei_entry_hdr *eh = (rei_entry_hdr *) p->scratch;
      if (eh->rs_index >= p->hdr.result_slots) {
        /* a corrupt entry can never be claimed safely: skip the CAS,
           leaving head untouched — the submitter's reap cancels the slot */
        rei_err_record(&p->h, REI_ERRCAT_OTHER, "corrupt pool entry");
        break;
      }
      pool_announce(p);
      if (atomic_compare_exchange_strong_explicit(hd, &head, head + 1,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed)) {
        p->scan_start = s + 1;
        p->st_inj++;
        /* backpressure wake: ring and waiter correspond one-to-one */
        uint64_t bit = 1ull << s;
        if (atomic_load_explicit(p->full_waiters, memory_order_relaxed) &
            bit) {
          uint64_t w = atomic_fetch_and_explicit(p->full_waiters, ~bit,
                                                 memory_order_seq_cst);
          if (w & bit) rei_unpark(pool_sub_pk(p, s));
        }
        return 1;
      }
      pool_announce_clear(p);
      /* lost the claim race to another worker: retry this ring */
    }
  }
  return 0;
}

// Deques and stealing ---------------------------------------------------------------------

/* Owner push at the bottom (the copy form — pool_submit_nested stages
   directly into the deque slot). Entry bytes are written before the
   release store of bottom, which is what publishes them to thieves. The
   full check loads top fresh: the steal path's stale-copy argument (a
   slot is overwritten only after top advanced past it, failing the
   thief's CAS) relies on the owner never lapping an unadvanced top. */
static int pool_deque_push(rei_pool *p, const unsigned char *entry) {
  rei_wk_slot *me = &p->wk[p->wk_slot];
  int64_t b = atomic_load_explicit(&me->deque_bottom, memory_order_relaxed);
  int64_t t = atomic_load_explicit(&me->deque_top, memory_order_acquire);
  if (b - t >= (int64_t) me->deque_cap) return 0;
  pool_copy_entry(p, deque_entry_at(p, me, b), entry);
  atomic_store_explicit(&me->deque_bottom, b + 1, memory_order_release);
  return 1;
}

/* Chase-Lev take (Le et al. orderings): decrement bottom, seq_cst fence,
   load top; the last element resolves the owner-vs-thief race by CAS on
   top. The entry is copied and announced before the claim can commit —
   only the owner writes the buffer, so the pre-decrement copy is stable —
   and the announce is cleared on the lost race. */
static int pool_deque_pop(rei_pool *p) {
  rei_wk_slot *me = &p->wk[p->wk_slot];
  int64_t b = atomic_load_explicit(&me->deque_bottom, memory_order_relaxed);
  int64_t t = atomic_load_explicit(&me->deque_top, memory_order_acquire);
  if (t >= b) return 0;
  b--;
  pool_copy_entry(p, p->scratch, deque_entry_at(p, me, b));
  if (((rei_entry_hdr *) p->scratch)->rs_index >= p->hdr.result_slots) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "corrupt pool entry");
    return 0;
  }
  pool_announce(p);
  atomic_store_explicit(&me->deque_bottom, b, memory_order_relaxed);
  atomic_thread_fence(memory_order_seq_cst);
  t = atomic_load_explicit(&me->deque_top, memory_order_relaxed);
  if (t < b) return 1;                       /* not the last: ours outright */
  int got = 0;
  if (t == b)                                /* last element: race thieves */
    got = atomic_compare_exchange_strong_explicit(&me->deque_top, &t, t + 1,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed);
  atomic_store_explicit(&me->deque_bottom, b + 1, memory_order_relaxed);
  if (!got) pool_announce_clear(p);
  return got;
}

enum { REI_STEAL_EMPTY = 0, REI_STEAL_GOT, REI_STEAL_ABORT };

/* Attempt REAPING -> FREE. Conclusive only with top/bottom re-loaded after
   the status acquire (an earlier bottom read may predate the leaver's final
   writes): for an orphaned deque bottom is static and top monotonic, so
   top >= bottom then means truly drained, and any observer may free the
   slot — which closes the race where a thief empties the deque while the
   owner still reads LEAVING. Returns 1 when the deque is drained. */
static int pool_reaping_free(rei_wk_slot *w) {
  if (deque_nonempty(w)) return 0;
  int32_t expected = REI_WK_REAPING;
  atomic_compare_exchange_strong_explicit(&w->status, &expected, REI_WK_FREE,
                                          memory_order_seq_cst,
                                          memory_order_relaxed);
  return 1;
}

/* Chase-Lev steal: copy the entry at top, then CAS top to claim it; only
   the CAS publishes the theft, so a lost race or a torn copy from the
   owner lapping the buffer is discarded unobserved. An orphaned (REAPING)
   deque is consumed through this same path; whoever observes it drained
   returns the slot to FREE. */
static int pool_steal_from(rei_pool *p, uint32_t v) {
  rei_wk_slot *w = &p->wk[v];
  int64_t t = atomic_load_explicit(&w->deque_top, memory_order_acquire);
  atomic_thread_fence(memory_order_seq_cst);
  int64_t b = atomic_load_explicit(&w->deque_bottom, memory_order_acquire);
  if (t >= b) {
    if (atomic_load_explicit(&w->status, memory_order_acquire) ==
        REI_WK_REAPING && !pool_reaping_free(w))
      return REI_STEAL_ABORT;   /* orphaned and nonempty after all: retry */
    return REI_STEAL_EMPTY;
  }
  pool_copy_entry(p, p->scratch, deque_entry_at(p, w, t));
  if (((rei_entry_hdr *) p->scratch)->rs_index >= p->hdr.result_slots) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "corrupt pool entry");
    return REI_STEAL_ABORT;
  }
  pool_announce(p);
  if (!atomic_compare_exchange_strong_explicit(&w->deque_top, &t, t + 1,
                                               memory_order_seq_cst,
                                               memory_order_relaxed)) {
    pool_announce_clear(p);
    return REI_STEAL_ABORT;
  }
  if (atomic_load_explicit(&w->status, memory_order_acquire) ==
      REI_WK_REAPING)
    pool_reaping_free(w);
  p->st_steals++;
  return REI_STEAL_GOT;
}

static uint64_t pool_rng(rei_pool *p) {   /* xorshift64 */
  uint64_t x = p->rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return p->rng = x;
}

#define REI_STEAL_ROUNDS 4
#define REI_HELP_DEPTH_LIMIT 32

/* Random victim, one attempt per victim, bounded rounds; EMPTY and ABORT
   alike move to the next victim. Victims are LIVE and REAPING slots — or
   REAPING only for a help-mode collector at its depth limit, since live
   peers' deques have a guaranteed executor (their owner) while ownerless
   work does not. Exhausting the rounds falls through to the caller's next
   tier (injection scan, then the pre-park spin + announce-then-rescan),
   which is what makes a missed steal safe. */
static int pool_steal_any(rei_pool *p, int reaping_only) {
  uint32_t mw = p->hdr.max_workers;
  if (mw <= 1) return 0;
  for (int r = 0; r < REI_STEAL_ROUNDS; r++) {
    uint32_t start = (uint32_t) (pool_rng(p) % mw);
    for (uint32_t k = 0; k < mw; k++) {
      uint32_t i = (start + k) % mw;
      if ((int) i == p->wk_slot) continue;
      int32_t st = atomic_load_explicit(&p->wk[i].status,
                                        memory_order_acquire);
      if (st != REI_WK_REAPING && (reaping_only || st != REI_WK_LIVE))
        continue;
      if (pool_steal_from(p, i) == REI_STEAL_GOT) return 1;
      if (st == REI_WK_LIVE && deque_nonempty(&p->wk[i]))
        p->probe_victim = i;
    }
  }
  return 0;
}

#define REI_PROBE_STREAK 16

static int pool_steal(rei_pool *p) {
  p->probe_victim = UINT32_MAX;
  if (pool_steal_any(p, 0)) {
    p->probe_streak = 0;
    return 1;
  }
  /* thief backstop: repeated failures against an apparently-live,
     apparently-nonempty victim warrant one death probe — the cross-check
     for a reap the listener never ran */
  if (p->probe_victim != UINT32_MAX &&
      ++p->probe_streak >= REI_PROBE_STREAK) {
    p->probe_streak = 0;
    pool_probe_worker(p, p->probe_victim);
  }
  return 0;
}

/* The fairness tick's full scan: every injection ring unfiltered by the
   ready mask, then every REAPING slot's orphaned deque — the bound on
   external-submission (and ownerless-work) latency when a saturated pool
   never otherwise falls through its local tiers. */
static int pool_fairness_scan(rei_pool *p) {
  if (pool_claim_rings(p, 0)) return 1;
  for (uint32_t i = 0; i < p->hdr.max_workers; i++) {
    if ((int) i == p->wk_slot) continue;
    if (atomic_load_explicit(&p->wk[i].status, memory_order_acquire) ==
        REI_WK_REAPING && pool_steal_from(p, i) == REI_STEAL_GOT)
      return 1;
  }
  return 0;
}

/* One claim attempt in tier order. On success the entry is in scratch and
   announced; the caller executes it. */
static int pool_next_task(rei_pool *p) {
  if (++p->claims % 61 == 0) {
    pool_stats_publish(p);
    if (pool_fairness_scan(p)) return 1;
  }
  if (pool_deque_pop(p)) return 1;
  if (pool_steal(p)) return 1;
  return pool_claim_rings(p, 1);
}

/* Cheap work probe for the pre-announce spin: one load of the ready mask
   plus a sweep of the deque index lines. */
static int pool_work_hint(rei_pool *p) {
  if (atomic_load_explicit(p->inj_ready, memory_order_acquire) != 0)
    return 1;
  for (uint32_t i = 0; i < p->hdr.max_workers; i++) {
    int32_t st = atomic_load_explicit(&p->wk[i].status,
                                      memory_order_acquire);
    if ((st == REI_WK_LIVE || st == REI_WK_REAPING) &&
        deque_nonempty(&p->wk[i]))
      return 1;
  }
  return 0;
}

/* Unfiltered work check for the pre-park rescan, covering every claim
   source — injection rings (repairing a stale-clear ready bit as it goes)
   and every LIVE or REAPING deque, own included. */
static int pool_any_work(rei_pool *p) {
  int any = 0;
  for (uint32_t s = 0; s < p->hdr.max_submitters; s++) {
    unsigned char *ring = pool_ring(p, s);
    if (atomic_load_explicit(ring_head(ring), memory_order_acquire) <
        atomic_load_explicit(ring_tail(ring), memory_order_acquire)) {
      atomic_fetch_or_explicit(p->inj_ready, 1ull << s,
                               memory_order_seq_cst);
      any = 1;
    }
  }
  for (uint32_t i = 0; i < p->hdr.max_workers; i++) {
    int32_t st = atomic_load_explicit(&p->wk[i].status,
                                      memory_order_acquire);
    if ((st == REI_WK_LIVE || st == REI_WK_REAPING) &&
        deque_nonempty(&p->wk[i]))
      any = 1;
  }
  return any;
}

// Death reaping ---------------------------------------------------------------------------

/* Everything in this section is pure SHM/CAS/flock/unpark code with no
   binding state: the controller's death-listener callback runs it off the
   handle's own thread, concurrently with whatever that thread is doing, so
   nothing here touches scratch, keeper tables, or the scan rotation.
   Holding the slot's liveness lock is the reap grant; every store is
   CAS-guarded or idempotent, so concurrent or repeated reaps are harmless
   (a kept-fd flock re-acquire succeeds and re-runs the reap; a LockFileEx
   re-acquire reads HELD and skips — the holding prober's reap suffices). */

/* Fail the dead worker's tasks — the announced claim, then a worker_slot
   sweep for whatever it was executing — unpark every waiter its orphaned
   deque names (their help scans then steal from it), and either wake a
   drainer or free the emptied slot. Read-only walk; resumable. */
static void pool_orphan_and_finalize(rei_pool *p, rei_wk_slot *w,
                                     uint32_t slot) {
  int32_t inf = atomic_load_explicit(&w->in_flight_rs, memory_order_acquire);
  if (inf >= 0 && (uint32_t) inf < p->hdr.result_slots) {
    rei_rs_hdr *rs = pool_rs(p, (uint32_t) inf);
    if (atomic_load_explicit(&rs->sequence, memory_order_relaxed) ==
        atomic_load_explicit(&w->in_flight_seq, memory_order_relaxed)) {
      int32_t expected = REI_RS_PENDING;
      if (atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                                  REI_RS_DIED,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed)) {
        pool_unpark_result_waiter(p, rs);
      } else if (expected == REI_RS_CANCEL) {
        /* handle already dropped: no collector waits; return the slot */
        atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                                REI_RS_FREE,
                                                memory_order_seq_cst,
                                                memory_order_relaxed);
      }
    }
    atomic_store_explicit(&w->in_flight_rs, -1, memory_order_relaxed);
  }
  /* The announce covers claimed-but-not-yet-executing (worker_slot still
     -1); a nested claim overwrites it and its publish clears it, so tasks
     the worker was executing — at any nesting depth — are found by their
     worker_slot stamp instead. No sequence check needed: pool_commit_rs
     resets the stamp before its PENDING release store, so a slot freed
     and recommitted can never read as the dead worker's. */
  for (uint32_t i = 0; i < p->hdr.result_slots; i++) {
    rei_rs_hdr *rs = pool_rs(p, i);
    int32_t st = atomic_load_explicit(&rs->status, memory_order_acquire);
    if ((st != REI_RS_PENDING && st != REI_RS_CANCEL) ||
        atomic_load_explicit(&rs->worker_slot, memory_order_relaxed) !=
        (int32_t) slot)
      continue;
    int32_t expected = REI_RS_PENDING;
    if (atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                                REI_RS_DIED,
                                                memory_order_seq_cst,
                                                memory_order_relaxed)) {
      pool_unpark_result_waiter(p, rs);
    } else if (expected == REI_RS_CANCEL) {
      /* the dead executor owed the CANCEL consume: return the slot */
      atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                              REI_RS_FREE,
                                              memory_order_seq_cst,
                                              memory_order_relaxed);
    }
  }
  int64_t b = atomic_load_explicit(&w->deque_bottom, memory_order_acquire);
  for (int64_t i = atomic_load_explicit(&w->deque_top, memory_order_acquire);
       i < b; i++) {
    rei_entry_hdr *eh = (rei_entry_hdr *) deque_entry_at(p, w, i);
    if (eh->rs_index < p->hdr.result_slots)
      pool_unpark_result_waiter(p, pool_rs(p, eh->rs_index));
  }
  if (atomic_load_explicit(&w->deque_top, memory_order_acquire) < b) {
    /* orphaned work must drain even when no waiter is parked; deque work
       is help-unreachable, so the unpark half suffices */
    (void) pool_unpark_worker_from(p, slot);
  } else {
    int32_t expected = REI_WK_REAPING;
    atomic_compare_exchange_strong_explicit(&w->status, &expected,
                                            REI_WK_FREE,
                                            memory_order_seq_cst,
                                            memory_order_relaxed);
  }
}

/* Precondition: the caller holds the slot's liveness lock. */
static void pool_reap_worker(rei_pool *p, uint32_t slot) {
  rei_wk_slot *w = &p->wk[slot];
  for (;;) {
    int32_t expected = atomic_load_explicit(&w->status,
                                            memory_order_acquire);
    if (expected == REI_WK_LIVE || expected == REI_WK_LEAVING) {
      if (!atomic_compare_exchange_strong_explicit(&w->status, &expected,
                                                   REI_WK_REAPING,
                                                   memory_order_seq_cst,
                                                   memory_order_relaxed))
        continue;
      pool_orphan_and_finalize(p, w, slot);
    } else if (expected == REI_WK_REAPING) {
      /* predecessor reaper died mid-walk: re-running it is harmless */
      pool_orphan_and_finalize(p, w, slot);
    } else if (expected == REI_WK_CLAIMING) {
      /* died between its lock acquire and CLAIMING -> LIVE: deque
         uninitialized, nothing in flight — lock-before-CAS in the join is
         what makes this state conclusive for a lock holder */
      if (!atomic_compare_exchange_strong_explicit(&w->status, &expected,
                                                   REI_WK_FREE,
                                                   memory_order_seq_cst,
                                                   memory_order_relaxed))
        continue;
    }
    return;   /* FREE: predecessor reap complete */
  }
}

/* Non-blocking death probe + reap. The controller probes on its kept fds;
   everyone else opens the path without O_CREAT and must prove the file's
   identity against the slot's recorded (dev, inode) before trusting an
   acquire — ENOENT or a mismatch reads as indeterminate, never a verdict,
   because a false DEAD is the one verdict the protocol cannot absorb.
   Returns 1 when a reap ran. */
static int pool_probe_worker(rei_pool *p, uint32_t slot) {
  rei_wk_slot *w = &p->wk[slot];
  if (atomic_load_explicit(&w->status, memory_order_acquire) == REI_WK_FREE)
    return 0;
  int dead = 0;
  if (p->live_all != NULL) {
    if (rei_live_try(p->live_all[slot]) != REI_LIVE_ACQUIRED) return 0;
    pool_reap_worker(p, slot);
    rei_live_unlock(p->live_all[slot]);
    dead = 1;
  } else {
    char path[1024];
    intptr_t h;
    if (pool_live_path(p, path, sizeof(path), "wk", slot) != 0) return 0;
    if (rei_live_open_existing(path, &h) != 0) return 0;
    uint64_t dev, ino;
    dead = rei_live_ident(h, &dev, &ino) == 0 &&
      dev == w->live_dev && ino == w->live_ino &&
      rei_live_try(h) == REI_LIVE_ACQUIRED;
    if (dead) pool_reap_worker(p, slot);
    rei_live_close(h);
  }
  /* the zc death backstop: this handle's lent regions consumed by the
     dead worker (task-arg payloads, keyed at the release point)
     force-reclaim. Runs on the handle-owning thread only — every probe
     caller is one — never in the controller's off-thread death callback */
  if (dead) rei_ledger_force(&p->h.fl, (int32_t) slot);
  return dead;
}

/* Precondition: the caller holds the dead submitter's liveness lock.
   Cancels its PENDING slots (mid-execution workers observe the publish-CAS
   failure and discard), frees its published-but-uncollected results
   (releasing the producing workers' keepers), and leaves CANCEL slots
   alone — they free at pop, which is what keeps release-at-reuse safe for
   entries still queued in the dead submitter's ring. */
static void pool_reap_submitter(rei_pool *p, uint32_t j) {
  rei_sub_slot *s = &p->sub[j];
  int32_t expected = REI_SUB_LIVE;
  if (!atomic_compare_exchange_strong_explicit(&s->status, &expected,
                                               REI_SUB_REAPING,
                                               memory_order_seq_cst,
                                               memory_order_acquire) &&
      expected != REI_SUB_REAPING)
    return;                                  /* FREE: nothing to do */
  for (uint32_t k = 0; k < s->rs_count; k++) {
    rei_rs_hdr *rs = pool_rs(p, s->rs_start + k);
    for (;;) {
      int32_t st = atomic_load_explicit(&rs->status, memory_order_acquire);
      if (st == REI_RS_PENDING) {
        int32_t e2 = REI_RS_PENDING;
        if (!atomic_compare_exchange_strong_explicit(&rs->status, &e2,
                                                     REI_RS_CANCEL,
                                                     memory_order_seq_cst,
                                                     memory_order_relaxed))
          continue;
        pool_unpark_result_waiter(p, rs);
      } else if (st == REI_RS_OK || st == REI_RS_ERR || st == REI_RS_DIED) {
        int32_t wk = atomic_load_explicit(&rs->worker_slot,
                                          memory_order_acquire);
        int32_t e2 = st;
        if (!atomic_compare_exchange_strong_explicit(&rs->status, &e2,
                                                     REI_RS_FREE,
                                                     memory_order_seq_cst,
                                                     memory_order_relaxed))
          continue;
        pool_unpark_keeper_drop(p, wk);
      }
      break;
    }
  }
  atomic_store_explicit(&s->status, REI_SUB_FREE, memory_order_seq_cst);
}

static void pool_probe_submitter(rei_pool *p, uint32_t j) {
  if ((int) j == p->sub_slot) return;        /* our own held lock */
  rei_sub_slot *s = &p->sub[j];
  if (atomic_load_explicit(&s->status, memory_order_acquire) ==
      REI_SUB_FREE)
    return;
  int reaped = 0;
  if (p->live_all != NULL) {
    intptr_t h = p->live_all[p->hdr.max_workers + j];
    if (rei_live_try(h) != REI_LIVE_ACQUIRED) return;
    pool_reap_submitter(p, j);
    rei_live_unlock(h);
    reaped = 1;
  } else {
    char path[1024];
    intptr_t h;
    if (pool_live_path(p, path, sizeof(path), "sub", j) != 0) return;
    if (rei_live_open_existing(path, &h) != 0) return;
    uint64_t dev, ino;
    if (rei_live_ident(h, &dev, &ino) == 0 &&
        dev == s->live_dev && ino == s->live_ino &&
        rei_live_try(h) == REI_LIVE_ACQUIRED) {
      pool_reap_submitter(p, j);
      reaped = 1;
    }
    rei_live_close(h);
  }
  /* the zc death backstop rides the submitter reap: this handle's lent
     regions consumed by the dead submitter force-reclaim (REFHELD ones
     leak + unlink). Only ever runs on the handle-owning thread (the probe
     callers), never in the controller's off-thread death callback */
  if (reaped) rei_ledger_force(&p->h.fl, (int32_t) j);
}

// Death watches ------------------------------------------------------------------

/* The controller's per-worker death callback: OS notification -> lock
   verdict -> reap, entirely off the handle's own thread. A pid-reuse race
   is absorbed by the lock (the impostor holds nothing here). Everything
   reached from here is pure SHM/CAS/flock/unpark code — see the reaping
   section's header note. */
static void pool_wk_death_cb(void *arg) {
  struct rei_reap_ctx_s *c = arg;
  rei_pool *p = c->pool;
  if (rei_live_try(p->live_all[c->slot]) == REI_LIVE_ACQUIRED) {
    pool_reap_worker(p, c->slot);
    rei_live_unlock(p->live_all[c->slot]);
  }
}

/* The controller points its death listener at every LIVE target slot's
   pid, stopping any stale watch first so respawned slots get fresh
   watches. The callback reap then runs off the handle's thread at OS
   notification latency. In-process joins (the unit harness) are skipped —
   a process cannot meaningfully watch itself. */
static int pool_watch_workers(rei_pool *p, const uint32_t *slots, size_t n) {
  if (p->wk_watch == NULL) {
    p->wk_watch = calloc(p->hdr.max_workers, sizeof(*p->wk_watch));
    p->wk_dead = calloc(p->hdr.max_workers, sizeof(*p->wk_dead));
    p->reap_ctx = calloc(p->hdr.max_workers, sizeof(*p->reap_ctx));
    if (p->wk_watch == NULL || p->wk_dead == NULL || p->reap_ctx == NULL) {
      rei_err_record(&p->h, REI_ERRCAT_NOMEMORY, "allocation failure");
      return -1;
    }
  }
  for (size_t i = 0; i < n; i++) {
    uint32_t s = slots[i];
    rei_wk_slot *w = &p->wk[s];
    if (atomic_load_explicit(&w->status, memory_order_acquire) !=
        REI_WK_LIVE)
      continue;
    if (w->pid == (int64_t) p->self_pid) continue;
    if (p->wk_watch[s] != NULL) {
      rei_death_watch_stop(p->wk_watch[s]);
      p->wk_watch[s] = NULL;
    }
    atomic_store_explicit(&p->wk_dead[s], 0, memory_order_relaxed);
    p->reap_ctx[s].pool = p;
    p->reap_ctx[s].slot = s;
    /* NULL leaves the probes and the teardown sweep as the backstops */
    p->wk_watch[s] = rei_death_watch_start2((long) w->pid, &p->wk_dead[s],
                                            NULL, pool_wk_death_cb,
                                            &p->reap_ctx[s]);
  }
  return 0;
}

/* The owner watch every participant arms at join: the listener translates
   the owner's exit into owner_dead plus an unpark of the joiner's own
   parker (the worker slot's for a worker, its submitter slot's for a
   submitter), so a parked step/collect loop wakes into its owner-gone
   path. Detection is a wake trigger only — the liveness lock stays the
   verdict. */
static int pool_watch_owner(rei_pool *p, rei_parker *own_pk) {
  if ((long) p->hdr.owner_pid == p->self_pid) return 0;   /* in-process join */
  p->watch = rei_death_watch_start((long) p->hdr.owner_pid, &p->owner_dead,
                                   own_pk);
  if (p->watch == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "cannot watch owner process %llu",
                   (unsigned long long) p->hdr.owner_pid);
    return -1;
  }
  return 0;
}

/* Owner-death cleanup: acquiring the owner lock (the kept fd from join)
   grants exclusive teardown; a caller finding it held knows teardown is in
   progress elsewhere and simply fails locally. The broadcast is exactly
   the stop broadcast; the tail is janitorial best-effort — liveness files
   by path, the region via the dead-PID reaper (its name embeds the dead
   creator's pid). */
static void pool_remove_live_files(rei_pool *p) {
  char path[1024];
  for (uint32_t i = 0; i < p->hdr.max_workers; i++)
    if (pool_live_path(p, path, sizeof(path), "wk", i) == 0) remove(path);
  for (uint32_t j = 0; j < p->hdr.max_submitters; j++)
    if (pool_live_path(p, path, sizeof(path), "sub", j) == 0) remove(path);
  if (pool_live_path(p, path, sizeof(path), "owner", 0) == 0) remove(path);
}

static void pool_orphan_teardown_try(rei_pool *p) {
  if (p->live_owner == 0 ||
      rei_live_try(p->live_owner) != REI_LIVE_ACQUIRED)
    return;
  pool_shutdown_broadcast(p);
  pool_remove_live_files(p);
  int n = 0;
  char **list = rei_shm_reap(&n);
  for (int i = 0; i < n; i++) free(list[i]);
  free(list);
}

// The result sink and the publish verbs --------------------------------------------------

/* The sink the worker loop hands to the binding's exec: the core fills
   it from the claimed entry (and the unwind path from the saved cur_*
   identity), the binding passes it back to one of the publish verbs
   below. */
static rei_result_sink pool_make_sink(rei_pool *p, uint32_t rs_index,
                                      uint16_t sub_slot, uint64_t seq,
                                      uint64_t task_id) {
  rei_rs_hdr *rs = pool_rs(p, rs_index);
  rei_result_sink sink = {
    .p = p,
    .rs = rs,
    .payload = (unsigned char *) rs + sizeof(rei_rs_hdr),
    .rs_index = rs_index,
    .inline_max = p->inline_rs,
    .sub_slot = sub_slot,
    .seq = seq,
    .task_id = task_id
  };
  return sink;
}

/* The publish tail shared by the sink verbs: count the spill, CAS the
   status OK/ERR, swap the slot's keeper record, wake the waiter — or
   consume a concurrent CANCEL and probe the (possibly dead) submitter.
   Retires the in-flight announce and emits the task's terminal trace
   event. Payload writes are plain stores into a slot no allocator can
   touch (status stays PENDING/CANCEL until the FREE transition); the
   publish CAS is the release barrier a collector's acquire load pairs
   with. */
static int pool_publish_tail(rei_result_sink *sink, int ok) {
  rei_pool *p = sink->p;
  rei_rs_hdr *rs = sink->rs;
  uint8_t kind = p->h.fl.staging_kind;
  pool_count_spill(p, (uint32_t) sink->sub_slot, &rs->ph);
  p->st_tasks++;
  int32_t expected = REI_RS_PENDING;
  int published =
    atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                            ok ? REI_RS_OK : REI_RS_ERR,
                                            memory_order_seq_cst,
                                            memory_order_acquire);
  if (published) {
    /* a same-slot republish can overwrite the previous incarnation's
       keeper before any reap visit ran; the slot was freed and recommitted
       in between, so that region surrenders rather than leaking */
    rei_keeper_release(&p->h, p->keepers, sink->rs_index);
    rei_keeper_commit(&p->h, p->keepers, sink->rs_index);
    /* a spilled result region's consumer is the task's submitter — key
       the zc entry so the submitter-death backstop can force-reclaim it */
    if (p->keepers[sink->rs_index].kind == REI_KEEP_ZC)
      p->keepers[sink->rs_index].key = (int32_t) sink->sub_slot;
    /* a keeperless result (the self-contained kinds) pins nothing: no
       record */
    if (kind != REI_KEEP_FREE)
      pool_rk_add(p, sink->rs_index, sink->seq);
    pool_unpark_result_waiter(p, rs);
  } else {
    /* cancelled while we ran: drop the result, return the slot — a spilled
       result region was never published, so it recycles immediately */
    rei_keeper_discard(&p->h);
    expected = REI_RS_CANCEL;
    atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                            REI_RS_FREE,
                                            memory_order_seq_cst,
                                            memory_order_relaxed);
    /* consuming a CANCEL — here or at the pre-eval skip — is submitter
       death's one hot-path trigger: a live submitter means a genuine
       cancellation, a dead one is reaped in-line */
    if (sink->sub_slot < p->hdr.max_submitters)
      pool_probe_submitter(p, sink->sub_slot);
  }
  atomic_store_explicit(&p->wk[p->wk_slot].in_flight_rs, -1,
                        memory_order_relaxed);
  pool_trace_emit(p,
                  published ? (ok ? REI_TRACE_DONE : REI_TRACE_ERROR)
                            : REI_TRACE_DROP,
                  sink->task_id);
  return published;
}

/* The sink verbs, invoked by the binding's exec (and the unwind path's
   err publish). Each rolls back any uncommitted spill checkout and
   reserves the keeper-drop table ahead of staging, so nothing past the
   CAS can fail. Return 1 when the publish CAS won, 0 when a cancel beat
   it, -1 on infrastructure failure (recorded on the handle — the worker
   cannot continue). */
int rei_result_publish(rei_result_sink *sink, void *value) {
  rei_pool *p = sink->p;
  rei_stage_rollback(&p->h);
  if (pool_rk_reserve(p) != 0) return -1;
  if (p->h.binding.stage(value, &sink->rs->ph, sink->payload,
                         sink->inline_max, &p->h) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_STAGE, "staging failed");
    return -1;
  }
  return pool_publish_tail(sink, 1);
}

int rei_result_publish_err(rei_result_sink *sink, void *flattened,
                           uint32_t inline_n) {
  rei_pool *p = sink->p;
  rei_stage_rollback(&p->h);
  if (pool_rk_reserve(p) != 0) return -1;
  if (inline_n != 0) {
    /* the binding framed the flattened envelope INLINE: a self-contained
       stream pins nothing — the keeperless kinds' discipline, which the
       collect path's keeperless gate already reads off the magic byte */
    if (inline_n > sink->inline_max) {
      rei_err_record(&p->h, REI_ERRCAT_OTHER,
                     "inline error envelope exceeds the slot budget");
      return -1;
    }
    sink->rs->ph.kind = REI_KIND_INLINE;
    sink->rs->ph.len = inline_n;
    sink->rs->ph.aux = 0;
  } else {
    /* below the flatten's inline guarantee (a 128-byte slot holds no
       classed condition) the tiered stage carries the envelope out of
       line */
    if (p->h.binding.stage(flattened, &sink->rs->ph, sink->payload,
                           sink->inline_max, &p->h) != 0) {
      rei_err_record(&p->h, REI_ERRCAT_STAGE, "staging failed");
      return -1;
    }
  }
  return pool_publish_tail(sink, 0);
}

/* The status-only terminal for a task whose out-of-line payload vanished
   with its dead enqueuer (Win32 mappings cannot outlive their creator):
   the task can never run anywhere — it fails as DIED exactly like a
   claimed task whose worker died, and the drain continues in this
   thief. */
void rei_result_publish_died(rei_result_sink *sink) {
  rei_pool *p = sink->p;
  rei_rs_hdr *rs = sink->rs;
  int32_t expected = REI_RS_PENDING;
  if (atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                              REI_RS_DIED,
                                              memory_order_seq_cst,
                                              memory_order_relaxed)) {
    pool_unpark_result_waiter(p, rs);
  } else if (expected == REI_RS_CANCEL) {
    atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                            REI_RS_FREE,
                                            memory_order_seq_cst,
                                            memory_order_relaxed);
    if (sink->sub_slot < p->hdr.max_submitters)
      pool_probe_submitter(p, sink->sub_slot);
  }
  atomic_store_explicit(&p->wk[p->wk_slot].in_flight_rs, -1,
                        memory_order_relaxed);
  pool_trace_emit(p, REI_TRACE_DROP, sink->task_id);
}

/* Executes the claimed, announced entry in scratch and publishes into its
   result slot. Reentrant: help-mode and nested-submit execution recurse
   through here from inside the binding's eval, and every claim path
   reuses scratch — so everything needed from the entry and the announce
   is copied out before the exec. The claim bookkeeping and the cancel
   skip are the transport's; the frame decode, the eval, and the error
   flattening are the binding's (binding.exec), publishing through the
   sink. Returns 0 on success; a nonzero exec_fn return is infrastructure
   failure — recorded here, and the worker cannot continue. */
static int pool_execute(rei_pool *p, int catching) {
  if (p->h.binding.exec == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "no evaluator registered on this worker handle");
    return 1;
  }
  rei_wk_slot *me = &p->wk[p->wk_slot];
  rei_entry_hdr *eh = (rei_entry_hdr *) p->scratch;
  uint32_t rs_index = eh->rs_index;
  uint16_t sub_slot = eh->submitter_slot;
  uint64_t task_id = eh->task_id;
  rei_rs_hdr *rs = pool_rs(p, rs_index);
  uint64_t seq = atomic_load_explicit(&me->in_flight_seq,
                                      memory_order_relaxed);
  atomic_store_explicit(&rs->worker_slot, p->wk_slot, memory_order_relaxed);
  if (!catching) {
    p->cur_rs_index = rs_index;
    p->cur_seq = seq;
    p->cur_task_id = task_id;
    p->cur_sub_slot = sub_slot;
  }

  /* skip dead work: the check races a cancel's CAS, and correctness rests
     on the publish CAS in the tail either way. The probe rides here as at
     the failed publish — a submitter that died before its queued work was
     claimed would otherwise pin its slot until the stop sweep */
  if (atomic_load_explicit(&rs->status, memory_order_acquire) ==
      REI_RS_CANCEL) {
    int32_t expected = REI_RS_CANCEL;
    atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                            REI_RS_FREE,
                                            memory_order_seq_cst,
                                            memory_order_relaxed);
    if (sub_slot < p->hdr.max_submitters)
      pool_probe_submitter(p, sub_slot);
    atomic_store_explicit(&me->in_flight_rs, -1, memory_order_relaxed);
    pool_trace_emit(p, REI_TRACE_DROP, task_id);
    return 0;
  }

  rei_result_sink sink = pool_make_sink(p, rs_index, sub_slot, seq,
                                        task_id);
  pool_trace_emit(p, REI_TRACE_START, task_id);
  /* scratch (and eh with it) is dead once the task runs: the eval may
     claim into it — the binding decodes the frame before then */
  if (p->h.binding.exec(&eh->ph, p->scratch + sizeof(rei_entry_hdr),
                        (size_t) p->inline_entry, &sink, catching,
                        p->h.binding.ctx) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "the binding's exec_fn failed (infrastructure failure)");
    return 1;
  }
  return 0;
}

void rei_pool_eval_mark(rei_pool *p, int in_flight) {
  p->in_eval = in_flight;
}

int rei_pool_unwind_sink(rei_pool *p, rei_result_sink *out) {
  if (p == NULL || !p->in_eval) return 0;
  p->in_eval = 0;
  *out = pool_make_sink(p, p->cur_rs_index, p->cur_sub_slot, p->cur_seq,
                        p->cur_task_id);
  return 1;
}

// The worker loop --------------------------------------------------------------------

/* The loop's terminal codes, mapped by the two entries onto
   rei_step_result / rei_worker_exit. */
enum { POOL_RUN_TASK = 1, POOL_RUN_IDLE = 0, POOL_RUN_SHUTDOWN = -1,
       POOL_RUN_RETIRED = -2, POOL_RUN_OWNER_GONE = -3, POOL_RUN_ERROR = -4 };

/* The worker loop, shared by its two entry points. rei_pool_step
   (single = 1) runs one iteration — claim + execute one task or park —
   returning after the task, on timeout, or on a terminal condition; the
   in-process harnesses single-step it. rei_pool_worker_run (single = 0)
   stays in C across tasks and returns only the terminal exits. Two
   per-task disciplines: an interrupt check after each execute (a binding
   run loop's back-edge check) and, for single = 0 with a finite timeout,
   a deadline recompute at park-timeout expiry. The evaluator comes from
   the handle, checked up front so a claim can never outrun a missing
   evaluator. */
static int pool_step_impl(rei_pool *p, double timeout_ms, int single) {
  if (p->h.binding.exec == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "no evaluator registered on this worker handle");
    return POOL_RUN_ERROR;
  }
  rei_wk_slot *me = &p->wk[p->wk_slot];
  uint64_t my_bit = 1ull << p->wk_slot;
  double deadline = timeout_ms < 0 ? -1 : rei_now() + timeout_ms / 1000;

  /* heal any announce (or unwind-path eval flag) left dangling by an
     abandoned previous step: a stale bit costs the pusher one failed CAS.
     Gated on the process-local flag — only this worker ever sets its own
     bit, so the flag is exact and a clean previous exit skips a per-task
     seq_cst RMW on the mask line every worker shares. */
  p->in_eval = 0;
  if (p->announced) {
    atomic_store_explicit(&me->park_state, REI_WPK_RUNNING,
                          memory_order_relaxed);
    atomic_fetch_and_explicit(p->parked_workers, ~my_bit,
                              memory_order_seq_cst);
    p->announced = 0;
  }

  for (;;) {
    pool_reap_quota(p);
    if (atomic_load_explicit(p->shutdown, memory_order_acquire) != 0) {
      pool_stats_publish(p);
      return POOL_RUN_SHUTDOWN;
    }
    if (atomic_load_explicit(&p->owner_dead, memory_order_acquire) != 0) {
      /* first confirmed detector tears the orphan pool down; losers just
         exit — the winner's broadcast collapses everyone's discovery */
      pool_orphan_teardown_try(p);
      pool_stats_publish(p);
      return POOL_RUN_OWNER_GONE;
    }
    if (atomic_load_explicit(&me->retire, memory_order_acquire) != 0) {
      pool_stats_publish(p);
      return POOL_RUN_RETIRED;
    }

    if (pool_next_task(p)) {
      /* work found since the last reset recovers the scan budget —
         without this, work caught after a park wake strands it at the
         floor under trickle traffic */
      p->scan_budget_ns = REI_SPIN_BUDGET_NS;
      if (pool_execute(p, 0) != 0) return POOL_RUN_ERROR;
      if (single) return POOL_RUN_TASK;
      if (rei_check_interrupt(&p->h.binding)) {
        pool_intr(p);
        return POOL_RUN_ERROR;
      }
      continue;
    }

    if (timeout_ms == 0) {
      pool_idle_sweep(p);
      pool_stats_publish(p);
      return POOL_RUN_IDLE;
    }

    /* time-boxed spin before announcing: sub-µs submit gaps are absorbed
       without touching the parked_workers line. The budget decays only
       when the spin comes up empty. The hint scan is O(max_workers): the
       light stride applies only to small pools, where the scan is a few
       loads and the clock would dominate */
    double until = rei_now() + (double) p->scan_budget_ns / 1e9;
    if (deadline >= 0 && deadline < until) until = deadline;
    int caught;
    REI_SPIN_WAIT(pool_work_hint(p), until,
                  p->hdr.max_workers <= 4 ? REI_SPIN_CLOCK_EVERY_LIGHT / 2
                                          : REI_SPIN_CLOCK_EVERY,
                  caught);
    if (caught) {
      p->scan_budget_ns = REI_SPIN_BUDGET_NS;
      continue;
    }
    p->scan_budget_ns = p->scan_budget_ns / 2 < REI_SPIN_FLOOR_NS ?
      REI_SPIN_FLOOR_NS : p->scan_budget_ns / 2;

    /* announce-then-rescan (the sleep race): either our rescan sees the
       push or the pusher's mask load sees our bit */
    uint32_t e = rei_parker_snapshot(pool_wk_pk(p, (uint32_t) p->wk_slot));
    p->announced = 1;
    atomic_store_explicit(&me->park_state, REI_WPK_IDLE,
                          memory_order_relaxed);
    pool_stats_publish(p);
    atomic_fetch_or_explicit(p->parked_workers, my_bit,
                             memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    /* going idle: sweep the whole keeper table (and the binding's sweep
       hook), so a parked worker holds only what is genuinely uncollected.
       After the announce, so a keeper-drop FREE racing this park is either
       consumed here or its dropper saw our bit and unparks us
       (pool_unpark_keeper_drop) — the epoch snapshot above predates the
       bit, so that unpark turns the park below into an immediate return */
    pool_idle_sweep(p);
    /* retire joins the wake conditions here: its store + unpark landing
       between the loop-top check and the epoch snapshot above would
       otherwise be a lost wake, and the park below sleeps for the nominal
       bound */
    if (pool_any_work(p) ||
        atomic_load_explicit(&me->retire, memory_order_acquire) != 0 ||
        atomic_load_explicit(p->shutdown, memory_order_acquire) != 0 ||
        atomic_load_explicit(&p->owner_dead, memory_order_acquire) != 0) {
      atomic_fetch_and_explicit(p->parked_workers, ~my_bit,
                                memory_order_seq_cst);
      atomic_store_explicit(&me->park_state, REI_WPK_RUNNING,
                            memory_order_relaxed);
      p->announced = 0;
      continue;
    }
    int32_t expected = REI_WPK_IDLE;
    if (atomic_compare_exchange_strong_explicit(&me->park_state, &expected,
                                                REI_WPK_PARKED,
                                                memory_order_seq_cst,
                                                memory_order_relaxed)) {
      long ms = -1;
      if (deadline >= 0) {
        double rem = deadline - rei_now();
        ms = rem <= 0 ? 0 : (long) (rem * 1000) + 1;
      }
      rei_park_bracket(&p->h.binding, 1);
      rei_park(pool_wk_pk(p, (uint32_t) p->wk_slot), e, ms);
      rei_park_bracket(&p->h.binding, 0);
      p->st_parks++;
    }
    atomic_fetch_and_explicit(p->parked_workers, ~my_bit,
                              memory_order_seq_cst);
    atomic_store_explicit(&me->park_state, REI_WPK_RUNNING,
                          memory_order_relaxed);
    p->announced = 0;
    pool_stats_publish(p);
    if (rei_check_interrupt(&p->h.binding)) {
      pool_intr(p);
      return POOL_RUN_ERROR;
    }
    if (deadline >= 0 && rei_now() >= deadline) {
      pool_idle_sweep(p);
      if (single) return POOL_RUN_IDLE;
      deadline = rei_now() + timeout_ms / 1000;
    }
  }
}

int rei_pool_step(rei_pool *p, double timeout_ms) {
  if (pool_get(p) == NULL) return REI_STEP_SHUTDOWN;
  if (p->role != REI_ROLE_WORKER || p->wk_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a worker handle");
    return REI_STEP_SHUTDOWN;
  }
  switch (pool_step_impl(p, timeout_ms, 1)) {
  case POOL_RUN_TASK: return REI_STEP_TASK;
  case POOL_RUN_IDLE: return REI_STEP_IDLE;
  case POOL_RUN_RETIRED: return REI_STEP_RETIRED;
  default: return REI_STEP_SHUTDOWN;   /* shutdown, owner death, error */
  }
}

rei_worker_exit rei_pool_worker_run(rei_pool *p) {
  if (pool_get(p) == NULL) return REI_EXIT_ERROR;
  if (p->role != REI_ROLE_WORKER || p->wk_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a worker handle");
    return REI_EXIT_ERROR;
  }
  switch (pool_step_impl(p, -1, 0)) {
  case POOL_RUN_SHUTDOWN: return REI_EXIT_SHUTDOWN;
  case POOL_RUN_RETIRED: return REI_EXIT_RETIRED;
  case POOL_RUN_OWNER_GONE: return REI_EXIT_OWNER_GONE;
  default: return REI_EXIT_ERROR;
  }
}

// Collect and cancel ----------------------------------------------------------------------

/* Drop the task keeper of a terminal slot in this submitter's subrange.
   OK/ERR means the worker materialized the task args, DIED that the entry's
   claimant is dead: either way no reader remains, so a spilled task region
   surrenders to the free list with the keeper. key names the consuming
   worker for the zc ledger (the worker-death backstop); -1 when unknown. */
static void pool_task_keeper_drop(rei_pool *p, uint32_t idx, int32_t key) {
  rei_sub_slot *me = &p->sub[p->sub_slot];
  if (idx < me->rs_start || idx >= me->rs_start + me->rs_count) return;
  rei_keeper *keepers =
    p->role == REI_ROLE_WORKER ? p->sub_keepers : p->keepers;
  uint32_t local = idx - me->rs_start;
  if (keepers[local].kind == REI_KEEP_ZC) keepers[local].key = key;
  rei_keeper_release(&p->h, keepers, local);
}

/* Learn the collect budget from a completed wait (t_wait < 0 on the
   never-waited fast path); the tried budget seeds the halve branch. */
static void pool_collect_learn(rei_pool *p, double t_wait,
                               uint64_t budget) {
  if (t_wait < 0) return;
  p->collect_budget_ns =
    rei_spin_learn((rei_now() - t_wait) * 1e9, budget,
                   (uint64_t) REI_COLLECT_SPIN_BUDGET_NS);
}

/* Invoke the binding's read_fn for a terminal slot. OK/ERR carry the
   slot's payload frame; CANCEL/DIED carry none — a NIL header stands in,
   and read_fn builds the binding's error object off ctx.outcome (DIED
   also fills the claimant record). Returns the product, NULL with the
   handle's error slot filled on failure. */
static void *pool_read_outcome(rei_pool *p, rei_rs_hdr *rs, int32_t st,
                               int32_t died_slot, int64_t died_pid) {
  rei_read_ctx ctx = p->h.read_tmpl;
  ctx.outcome = st;
  ctx.died_slot = died_slot;
  ctx.died_pid = died_pid;
  rei_slot_hdr nil_hdr = { REI_KIND_NIL, 0, 0 };
  const rei_slot_hdr *hdr = st == REI_RS_OK || st == REI_RS_ERR ?
    &rs->ph : &nil_hdr;
  void *v = p->h.binding.read(hdr, (unsigned char *) rs + sizeof(rei_rs_hdr),
                              p->inline_rs, &ctx);
  if (v == NULL) {
    const char *what =
      ctx.gone ? "payload region vanished" :
      st == REI_RS_OK ? "payload read failed" :
      st == REI_RS_ERR ? "task failed" :
      st == REI_RS_DIED ? "worker died while executing this task" :
      "task cancelled or pool stopped";
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "%s", what);
  }
  return v;
}

/* The terminal claim shared by the three collect forms. OK/ERR
   materialize BEFORE the FREE transition — publication of FREE is what
   lets the worker's keeper reap unlink everything the payload references
   — then drop the task keeper, FREE the slot, and wake the producer's
   keeper sweep (skipped for the keeperless kinds, whose FREE is invisible
   to the reap). DIED is status-word only: the claimant is confirmed dead
   (DIED implies the reap ran under the liveness lock), its lent task-arg
   regions force-reclaim here, the slot frees, and the claimant record
   rides the read. CANCEL reads without consuming — the task keeper
   releases at slot reuse, not here (the worker may not have materialized
   yet). Returns the read_fn product, NULL with the handle's error slot
   filled (an OK/ERR read failure consumes nothing — retryable). */
static void *pool_rs_claim(rei_pool *p, rei_rs_hdr *rs, uint32_t idx,
                           int32_t st) {
  switch (st) {
  case REI_RS_OK:
  case REI_RS_ERR: {
    void *v = pool_read_outcome(p, rs, st, -1, 0);
    if (v == NULL) return NULL;
    uint32_t kind = rs->ph.kind;
    int32_t w = atomic_load_explicit(&rs->worker_slot, memory_order_acquire);
    pool_task_keeper_drop(p, idx, w);
    int32_t expected = st;
    if (!atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                                 REI_RS_FREE,
                                                 memory_order_seq_cst,
                                                 memory_order_acquire)) {
      /* a double collect is a binding bug; the product stays with the
         binding (the core has no free hook to hand it back through) */
      rei_err_record(&p->h, REI_ERRCAT_OTHER,
                     "task handle already collected");
      return NULL;
    }
    if (!rei_keeperless(kind, (const unsigned char *) rs +
                        sizeof(rei_rs_hdr)))
      pool_unpark_keeper_drop(p, w);
    return v;
  }
  case REI_RS_DIED: {
    int32_t w = atomic_load_explicit(&rs->worker_slot, memory_order_acquire);
    pool_task_keeper_drop(p, idx, w);
    if (w >= 0 && (uint32_t) w < p->hdr.max_workers)
      rei_ledger_force(&p->h.fl, w);
    int64_t wpid =
      (w >= 0 && (uint32_t) w < p->hdr.max_workers) ? p->wk[w].pid : 0;
    int32_t expected = REI_RS_DIED;
    if (!atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                                 REI_RS_FREE,
                                                 memory_order_seq_cst,
                                                 memory_order_relaxed)) {
      rei_err_record(&p->h, REI_ERRCAT_OTHER,
                     "task handle already collected");
      return NULL;
    }
    return pool_read_outcome(p, rs, st, w, wpid);
  }
  case REI_RS_CANCEL:
    return pool_read_outcome(p, rs, st, -1, 0);
  case REI_RS_FREE:
  default:
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "task handle already collected");
    return NULL;
  }
}

rei_status rei_pool_collect(rei_pool *p, const rei_task *t,
                            void **value_out, double timeout_ms) {
  if (value_out != NULL) *value_out = NULL;
  if (pool_get(p) == NULL) return REI_ERR;
  if (t == NULL || t->rs_index >= p->hdr.result_slots) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "invalid task handle");
    return REI_ERR;
  }
  if (p->sub_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a submitter's task handle");
    return REI_ERR;
  }
  rei_rs_hdr *rs = pool_rs(p, t->rs_index);
  double deadline = -1;
  double t_wait = -1;

  int32_t st;
  for (;;) {
    if (!pool_seq_match(rs, t->seq)) {
      rei_err_record(&p->h, REI_ERRCAT_OTHER,
                     "task handle already collected or invalidated");
      return REI_ERR;
    }
    if (atomic_load_explicit(&p->owner_dead, memory_order_acquire) != 0)
      pool_orphan_teardown_try(p);   /* its cancel sweep ends this wait */
    st = atomic_load_explicit(&rs->status, memory_order_acquire);
    if (st != REI_RS_PENDING) break;

    /* Help mode: a worker blocked on its own subtree must make progress
       on runnable work, not park — N workers simultaneously parked in
       nested collects would deadlock the pool. The awaited task is on this
       worker's own deque bottom, claimed by a peer (progress either way),
       or on a dead worker's orphaned deque; help covers own pops and
       steals, and deliberately excludes the submitter injection rings. At
       the depth limit only ownerless work remains eligible (own bottom +
       REAPING deques): live peers' deques have a guaranteed executor, so
       excluding them there bounds C-stack growth without a hang — every
       chain of waiting collectors bottoms out in a worker executing. */
    if (p->role == REI_ROLE_WORKER && p->wk_slot >= 0 &&
        p->h.binding.exec != NULL) {
      int got = pool_deque_pop(p);
      if (!got)
        got = pool_steal_any(p, p->help_depth >= REI_HELP_DEPTH_LIMIT);
      if (got) {
        p->st_helps++;
        p->help_depth++;
        int rc = pool_execute(p, 1);
        p->help_depth--;
        if (rc != 0) return REI_ERR;
        continue;
      }
    }

    if (timeout_ms == 0) return REI_TIMEOUT;

    /* hoisted above the spin so the first episode's `until` is clamped
       too — a finite timeout's clock covers the whole wait. Still after
       the fast-path break and the poll return, so the indefinite and
       already-done paths pay no extra clock read. One read serves the
       deadline compute and the bound. */
    double now = rei_now();
    if (t_wait < 0) t_wait = now;
    if (deadline < 0 && timeout_ms > 0)
      deadline = now + timeout_ms / 1000;

    /* time-boxed spin before the park announce: a short task's publish
       is absorbed without the park/wake syscall pair on either side,
       since waiter_slot stays unannounced through the spin and the
       publisher skips its wake. Every wait exit learns its measured
       turnaround via rei_spin_learn. */
    uint64_t budget = p->collect_budget_ns;
    double until = now + (double) budget / 1e9;
    if (deadline >= 0 && deadline < until) until = deadline;
    int caught;
    REI_SPIN_WAIT((st = atomic_load_explicit(&rs->status,
                                             memory_order_acquire)) !=
                  REI_RS_PENDING, until, REI_SPIN_CLOCK_EVERY_LIGHT,
                  caught);
    if (caught) {
      pool_collect_learn(p, t_wait, budget);
      break;
    }

    /* announce -> fence -> re-check -> park bounded; the publishing worker
       reads waiter_slot after its publish CAS and unparks us */
    uint32_t e = rei_parker_snapshot(pool_sub_pk(p, (uint32_t) p->sub_slot));
    atomic_store_explicit(&rs->waiter_slot, p->sub_slot,
                          memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&rs->status, memory_order_acquire) !=
        REI_RS_PENDING) {
      pool_collect_learn(p, t_wait, budget);
      continue;
    }
    long ms = REI_INTERRUPT_BOUND_MS;
    if (deadline >= 0) {
      double rem = deadline - rei_now();
      if (rem <= 0) return REI_TIMEOUT;
      long rem_ms = (long) (rem * 1000) + 1;
      if (rem_ms < ms) ms = rem_ms;
    }
    rei_park_bracket(&p->h.binding, 1);
    rei_park(pool_sub_pk(p, (uint32_t) p->sub_slot), e, ms);
    rei_park_bracket(&p->h.binding, 0);
    p->st_collect_parks++;
    if (rei_check_interrupt(&p->h.binding)) return pool_intr(p);
    st = atomic_load_explicit(&rs->status, memory_order_acquire);
    if (st != REI_RS_PENDING) {
      pool_collect_learn(p, t_wait, budget);
      break;
    }
    /* backstop for a missed death notification, piggybacked on a wake
       that happened regardless — never a wakeup of its own */
    {
      int32_t claimant = atomic_load_explicit(&rs->worker_slot,
                                              memory_order_acquire);
      if (claimant >= 0 && (uint32_t) claimant < p->hdr.max_workers)
        pool_probe_worker(p, (uint32_t) claimant);
      if (deadline >= 0 && rei_now() >= deadline &&
          atomic_load_explicit(&rs->status, memory_order_acquire) ==
          REI_RS_PENDING)
        return REI_TIMEOUT;
    }
  }

  void *v = pool_rs_claim(p, rs, t->rs_index, st);
  if (v == NULL) return REI_ERR;
  if (value_out != NULL) *value_out = v;
  return REI_OK;
}

/* The collect-any predicate: the first handle (input order) whose slot
   reached a terminal state. OK, ERR, DIED, and CANCEL all report — a
   cancelled task is "done", as in asyncio's FIRST_COMPLETED. */
static int pool_any_terminal(rei_rs_hdr **rss, size_t n, size_t *found,
                             int32_t *st) {
  for (size_t i = 0; i < n; i++) {
    int32_t si = atomic_load_explicit(&rss[i]->status, memory_order_acquire);
    if (si != REI_RS_PENDING) {
      *found = i;
      *st = si;
      return 1;
    }
  }
  return 0;
}

/* The collect-all predicate: every slot terminal. */
static int pool_all_terminal(rei_rs_hdr **rss, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (atomic_load_explicit(&rss[i]->status, memory_order_acquire) ==
        REI_RS_PENDING)
      return 0;
  return 1;
}

/* Withdraw the waiter announcement from every still-PENDING slot: a
   publisher reads waiter_slot after its publish CAS, and a stale
   announcement would cost it an unpark syscall nobody needs — the caller
   is awake by construction, the only parker on these slots being this
   submitter. */
static void pool_any_unannounce(rei_rs_hdr **rss, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (atomic_load_explicit(&rss[i]->status, memory_order_acquire) ==
        REI_RS_PENDING)
      atomic_store_explicit(&rss[i]->waiter_slot, -1, memory_order_relaxed);
}

/* Vectored-collect front matter: validate the array and resolve each
   handle to its slot (sequence-checked). */
static rei_status pool_collect_resolve(rei_pool *p, const rei_task *tasks,
                                       size_t n, rei_rs_hdr ***rss_out) {
  if (tasks == NULL || n == 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "expected a non-empty task array");
    return REI_ERR;
  }
  if (p->sub_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a submitter's task handle");
    return REI_ERR;
  }
  rei_rs_hdr **rss = malloc(n * sizeof(*rss));
  if (rss == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_NOMEMORY, "allocation failure");
    return REI_ERR;
  }
  for (size_t i = 0; i < n; i++) {
    if (tasks[i].rs_index >= p->hdr.result_slots) {
      rei_err_record(&p->h, REI_ERRCAT_OTHER, "invalid task handle");
      free(rss);
      return REI_ERR;
    }
    rei_rs_hdr *rs = pool_rs(p, tasks[i].rs_index);
    if (!pool_seq_match(rs, tasks[i].seq)) {
      rei_err_record(&p->h, REI_ERRCAT_OTHER,
                     "task handle already collected or invalidated");
      free(rss);
      return REI_ERR;
    }
    rss[i] = rs;
  }
  *rss_out = rss;
  return REI_OK;
}

/* Wait on any of a submitter's outstanding tasks, on the existing slot
   mechanics: the wait predicate is the whole slot set — announce on every
   slot, park once on the submitter's one parker (every publish's directed
   unpark lands on it), scan on wake. Announcements are withdrawn at each
   exit so a later publish pays no stray unpark. O(n) per scan; no
   protocol change. */
rei_status rei_pool_collect_any(rei_pool *p, const rei_task *tasks,
                                size_t n, size_t *index_out,
                                void **value_out, double timeout_ms) {
  if (value_out != NULL) *value_out = NULL;
  if (pool_get(p) == NULL) return REI_ERR;
  if (index_out == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "expected an index out-param");
    return REI_ERR;
  }
  rei_rs_hdr **rss;
  if (pool_collect_resolve(p, tasks, n, &rss) != REI_OK) return REI_ERR;

  double deadline = -1;
  double t_wait = -1;
  size_t found = 0;
  int32_t st = REI_RS_PENDING;

  for (;;) {
    if (atomic_load_explicit(&p->owner_dead, memory_order_acquire) != 0)
      pool_orphan_teardown_try(p);   /* its cancel sweep ends this wait */
    if (pool_any_terminal(rss, n, &found, &st)) break;

    /* Help mode, as in collect: a worker blocked on its own subtree must
       make progress on runnable work, not park. */
    if (p->role == REI_ROLE_WORKER && p->wk_slot >= 0 &&
        p->h.binding.exec != NULL) {
      int got = pool_deque_pop(p);
      if (!got)
        got = pool_steal_any(p, p->help_depth >= REI_HELP_DEPTH_LIMIT);
      if (got) {
        p->st_helps++;
        p->help_depth++;
        int rc = pool_execute(p, 1);
        p->help_depth--;
        if (rc != 0) {
          pool_any_unannounce(rss, n);
          free(rss);
          return REI_ERR;
        }
        continue;
      }
    }

    if (timeout_ms == 0) {
      free(rss);
      return REI_TIMEOUT;
    }

    /* hoisted above the spin so the first episode's `until` is clamped
       too — a finite timeout's clock covers the whole wait. One read
       serves the deadline compute and the bound. */
    double now = rei_now();
    if (t_wait < 0) t_wait = now;
    if (deadline < 0 && timeout_ms > 0)
      deadline = now + timeout_ms / 1000;

    /* time-boxed spin before the park announce, as in collect: a fast
       result caught here costs neither side a syscall, waiter_slot
       staying unannounced through the spin. The predicate is O(n), so
       the light stride applies only to the common small-n case */
    uint64_t budget = p->collect_budget_ns;
    double until = now + (double) budget / 1e9;
    if (deadline >= 0 && deadline < until) until = deadline;
    int caught;
    REI_SPIN_WAIT(pool_any_terminal(rss, n, &found, &st), until,
                  n <= 4 ? REI_SPIN_CLOCK_EVERY_LIGHT :
                           REI_SPIN_CLOCK_EVERY, caught);
    if (caught) {
      pool_collect_learn(p, t_wait, budget);
      break;
    }

    /* announce on every slot -> fence -> re-check -> park bounded; each
       publishing worker reads its slot's waiter_slot after the publish
       CAS and unparks this submitter's one parker */
    uint32_t e = rei_parker_snapshot(pool_sub_pk(p, (uint32_t) p->sub_slot));
    for (size_t i = 0; i < n; i++)
      atomic_store_explicit(&rss[i]->waiter_slot, p->sub_slot,
                            memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    if (pool_any_terminal(rss, n, &found, &st)) {
      pool_collect_learn(p, t_wait, budget);
      break;
    }
    long ms = REI_INTERRUPT_BOUND_MS;
    if (deadline >= 0) {
      double rem = deadline - rei_now();
      if (rem <= 0) {
        pool_any_unannounce(rss, n);
        free(rss);
        return REI_TIMEOUT;
      }
      long rem_ms = (long) (rem * 1000) + 1;
      if (rem_ms < ms) ms = rem_ms;
    }
    rei_park_bracket(&p->h.binding, 1);
    rei_park(pool_sub_pk(p, (uint32_t) p->sub_slot), e, ms);
    rei_park_bracket(&p->h.binding, 0);
    p->st_collect_parks++;
    if (rei_check_interrupt(&p->h.binding)) {
      pool_any_unannounce(rss, n);
      free(rss);
      return pool_intr(p);
    }
    if (pool_any_terminal(rss, n, &found, &st)) {
      pool_collect_learn(p, t_wait, budget);
      break;
    }
    /* backstop for a missed death notification, piggybacked on a wake
       that happened regardless — never a wakeup of its own */
    for (size_t i = 0; i < n; i++) {
      int32_t claimant = atomic_load_explicit(&rss[i]->worker_slot,
                                              memory_order_acquire);
      if (claimant >= 0 && (uint32_t) claimant < p->hdr.max_workers)
        pool_probe_worker(p, (uint32_t) claimant);
    }
    if (deadline >= 0 && rei_now() >= deadline &&
        !pool_any_terminal(rss, n, &found, &st)) {
      pool_any_unannounce(rss, n);
      free(rss);
      return REI_TIMEOUT;
    }
  }

  pool_any_unannounce(rss, n);
  *index_out = found;
  void *v = pool_rs_claim(p, rss[found], tasks[found].rs_index, st);
  free(rss);
  if (v == NULL) return REI_ERR;   /* the index still reports */
  if (value_out != NULL) *value_out = v;
  return REI_OK;
}

/* Wait on all of a submitter's outstanding tasks, on the collect_any
   mechanics with an all-terminal predicate: announce on every pending
   slot, park once on the submitter's one parker, scan on wake. While
   parked the waiter is announced on every pending slot, so each
   publishing worker unparks it — K slow completions cost K wakes x O(n)
   scans, still well under the per-collect loop the verb replaces. One
   overall deadline; a timeout claims nothing, so no completed result is
   silently discarded. On success the claim runs in input order and stops
   at the first non-OK outcome by position inclusive: slots before it are
   claimed, slots after it stay collectible. */
rei_status rei_pool_collect_all(rei_pool *p, const rei_task *tasks,
                                size_t n, void **values_out,
                                size_t *err_index_out, double timeout_ms) {
  if (values_out != NULL)
    for (size_t i = 0; i < n; i++) values_out[i] = NULL;
  if (pool_get(p) == NULL) return REI_ERR;
  rei_rs_hdr **rss;
  if (pool_collect_resolve(p, tasks, n, &rss) != REI_OK) return REI_ERR;

  double deadline = -1;
  double t_wait = -1;

  for (;;) {
    if (atomic_load_explicit(&p->owner_dead, memory_order_acquire) != 0)
      pool_orphan_teardown_try(p);   /* its cancel sweep ends this wait */
    if (pool_all_terminal(rss, n)) break;

    /* Help mode, as in collect: a worker blocked on its own subtree must
       make progress on runnable work, not park. */
    if (p->role == REI_ROLE_WORKER && p->wk_slot >= 0 &&
        p->h.binding.exec != NULL) {
      int got = pool_deque_pop(p);
      if (!got)
        got = pool_steal_any(p, p->help_depth >= REI_HELP_DEPTH_LIMIT);
      if (got) {
        p->st_helps++;
        p->help_depth++;
        int rc = pool_execute(p, 1);
        p->help_depth--;
        if (rc != 0) {
          pool_any_unannounce(rss, n);
          free(rss);
          return REI_ERR;
        }
        continue;
      }
    }

    if (timeout_ms == 0) {
      free(rss);
      return REI_TIMEOUT;
    }

    double now = rei_now();
    if (t_wait < 0) t_wait = now;
    if (deadline < 0 && timeout_ms > 0)
      deadline = now + timeout_ms / 1000;

    /* time-boxed spin before the park announce, as in collect_any: a
       fast completion caught here costs neither side a syscall,
       waiter_slot staying unannounced through the spin */
    uint64_t budget = p->collect_budget_ns;
    double until = now + (double) budget / 1e9;
    if (deadline >= 0 && deadline < until) until = deadline;
    int caught;
    REI_SPIN_WAIT(pool_all_terminal(rss, n), until,
                  n <= 4 ? REI_SPIN_CLOCK_EVERY_LIGHT :
                           REI_SPIN_CLOCK_EVERY, caught);
    if (caught) {
      pool_collect_learn(p, t_wait, budget);
      break;
    }

    /* announce on every pending slot -> fence -> re-check -> park
       bounded; each publishing worker reads its slot's waiter_slot after
       the publish CAS and unparks this submitter's one parker */
    uint32_t e = rei_parker_snapshot(pool_sub_pk(p, (uint32_t) p->sub_slot));
    for (size_t i = 0; i < n; i++)
      if (atomic_load_explicit(&rss[i]->status, memory_order_acquire) ==
          REI_RS_PENDING)
        atomic_store_explicit(&rss[i]->waiter_slot, p->sub_slot,
                              memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    if (pool_all_terminal(rss, n)) {
      pool_collect_learn(p, t_wait, budget);
      break;
    }
    long ms = REI_INTERRUPT_BOUND_MS;
    if (deadline >= 0) {
      double rem = deadline - rei_now();
      if (rem <= 0) {
        pool_any_unannounce(rss, n);
        free(rss);
        return REI_TIMEOUT;
      }
      long rem_ms = (long) (rem * 1000) + 1;
      if (rem_ms < ms) ms = rem_ms;
    }
    rei_park_bracket(&p->h.binding, 1);
    rei_park(pool_sub_pk(p, (uint32_t) p->sub_slot), e, ms);
    rei_park_bracket(&p->h.binding, 0);
    p->st_collect_parks++;
    if (rei_check_interrupt(&p->h.binding)) {
      pool_any_unannounce(rss, n);
      free(rss);
      return pool_intr(p);
    }
    if (pool_all_terminal(rss, n)) {
      pool_collect_learn(p, t_wait, budget);
      break;
    }
    /* backstop for a missed death notification, piggybacked on a wake
       that happened regardless — never a wakeup of its own */
    for (size_t i = 0; i < n; i++) {
      int32_t claimant = atomic_load_explicit(&rss[i]->worker_slot,
                                              memory_order_acquire);
      if (claimant >= 0 && (uint32_t) claimant < p->hdr.max_workers)
        pool_probe_worker(p, (uint32_t) claimant);
    }
    if (deadline >= 0 && rei_now() >= deadline &&
        !pool_all_terminal(rss, n)) {
      pool_any_unannounce(rss, n);
      free(rss);
      return REI_TIMEOUT;
    }
  }

  pool_any_unannounce(rss, n);

  /* every slot terminal: claim in input order */
  size_t err = n;
  rei_status rc = REI_OK;
  for (size_t i = 0; i < n; i++) {
    int32_t st = atomic_load_explicit(&rss[i]->status, memory_order_acquire);
    void *v = pool_rs_claim(p, rss[i], tasks[i].rs_index, st);
    if (v == NULL) {
      err = i;   /* the read failure (slot i unconsumed when retryable) */
      rc = REI_ERR;
      break;
    }
    if (values_out != NULL) values_out[i] = v;
    if (st != REI_RS_OK) {
      err = i;
      break;
    }
  }
  free(rss);
  if (err_index_out != NULL) *err_index_out = err;
  return rc;
}

/* Advisory and discard-only, never preemptive: a task already executing
   runs to completion and its result is dropped by the worker's failed
   publish CAS. Total — cancel runs from binding unwind paths, so a stale
   handle, a closed pool, or a forked child answers 0 instead of raising.
   A completed slot is left collectible; releasing a handle that will
   never be collected is rei_pool_task_release. */
int rei_pool_cancel(rei_pool *p, const rei_task *t) {
  if (p == NULL || p->released || t == NULL || p->base == NULL ||
      p->self_pid != rei_self_pid())
    return 0;
  if (t->rs_index >= p->hdr.result_slots) return 0;
  rei_rs_hdr *rs = pool_rs(p, t->rs_index);
  if (!pool_seq_match(rs, t->seq)) return 0;
  int32_t expected = REI_RS_PENDING;
  if (atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                              REI_RS_CANCEL,
                                              memory_order_seq_cst,
                                              memory_order_relaxed)) {
    pool_unpark_result_waiter(p, rs);
    return 1;
  }
  return 0;
}

/* The finalizer release for a handle that was never collected: a pending
   task is cancelled as rei_pool_cancel does; a terminal OK/ERR/DIED slot
   is freed, so the producing worker's keeper sweep drops what the result
   retained and the slot never lingers until reuse. Same totality as
   cancel — a binding's GC finalizer runs it without a live error path. */
int rei_pool_task_release(rei_pool *p, const rei_task *t) {
  if (p == NULL || p->released || t == NULL || p->base == NULL ||
      p->self_pid != rei_self_pid())
    return 0;
  if (t->rs_index >= p->hdr.result_slots) return 0;
  rei_rs_hdr *rs = pool_rs(p, t->rs_index);
  if (!pool_seq_match(rs, t->seq)) return 0;
  for (;;) {
    int32_t st = atomic_load_explicit(&rs->status, memory_order_acquire);
    if (st == REI_RS_PENDING) {
      int32_t expected = REI_RS_PENDING;
      if (atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                                  REI_RS_CANCEL,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed)) {
        pool_unpark_result_waiter(p, rs);
        return 1;
      }
    } else if (st == REI_RS_OK || st == REI_RS_ERR || st == REI_RS_DIED) {
      int32_t w = atomic_load_explicit(&rs->worker_slot,
                                       memory_order_acquire);
      int32_t expected = st;
      if (atomic_compare_exchange_strong_explicit(&rs->status, &expected,
                                                  REI_RS_FREE,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed)) {
        pool_unpark_keeper_drop(p, w);
        return 0;
      }
    } else {
      return 0;            /* CANCEL or FREE: another party owns the slot */
    }
  }
}

/* Non-consuming state probe: two single reads, racy against slot reuse
   exactly as the dump is. Collect leaves the handle intact and moves the
   slot on, so FREE (or an advanced sequence) reads as collected; a
   released pool means the task can never be collected. */
int rei_pool_task_state(rei_pool *p, const rei_task *t) {
  if (p == NULL || p->released || t == NULL || p->base == NULL ||
      p->self_pid != rei_self_pid())
    return REI_RS_FREE;
  if (t->rs_index >= p->hdr.result_slots) return REI_RS_FREE;
  rei_rs_hdr *rs = pool_rs(p, t->rs_index);
  if (!pool_seq_match(rs, t->seq)) return REI_RS_FREE;
  return atomic_load_explicit(&rs->status, memory_order_acquire);
}

// Map support --------------------------------------------------------------------

/* The pool-signal trio a map runner threads through its batch loop: three
   opaque word addresses, loaded relaxed once per batch transition. The
   returned copy is malloc'd; the caller frees. The words it points at
   live with the pool region / the handle, which the runner holds. */
rei_pool_sig *rei_pool_signals(rei_pool *p) {
  if (pool_get(p) == NULL) return NULL;
  rei_pool_sig *s = malloc(sizeof(*s));
  if (s == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_NOMEMORY, "allocation failure");
    return NULL;
  }
  s->help_wanted = p->help_wanted;
  s->shutdown = p->shutdown;
  s->owner_dead = &p->owner_dead;
  return s;
}

/* The map's batch-sizing inputs, in one read pass: the caller's FREE
   result slots in its own subrange (not rs_count — the subrange is shared
   with whatever tasks are already outstanding, and pool_alloc_rs would
   otherwise error mid-submit), the injection cap, and the entry inline
   budget. Only this process allocates from its own subrange, so the FREE
   count can only grow under it. A worker's first nested map claims its
   submitter slot here — before the count, which would otherwise read an
   unclaimed subrange — exactly as nested submit does. */
int rei_pool_map_caps(rei_pool *p, uint32_t *free_rs, uint32_t *inj_cap,
                      uint32_t *inline_entry) {
  if (pool_get(p) == NULL) return -1;
  if (atomic_load_explicit(p->shutdown, memory_order_acquire) != 0) {
    rei_err_record(&p->h, REI_ERRCAT_STOPPED, "pool stopped");
    return -1;
  }
  if (p->role == REI_ROLE_WORKER && p->wk_slot >= 0 && p->sub_slot < 0 &&
      pool_sub_keepers_claim(p) != REI_OK)
    return -1;
  if (p->sub_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a submitter handle");
    return -1;
  }
  rei_sub_slot *me = &p->sub[p->sub_slot];
  uint32_t n = 0;
  for (uint32_t k = 0; k < me->rs_count; k++)
    n += atomic_load_explicit(&pool_rs(p, me->rs_start + k)->status,
                              memory_order_acquire) == REI_RS_FREE;
  if (free_rs != NULL) *free_rs = n;
  if (inj_cap != NULL) *inj_cap = p->hdr.inj_cap;
  if (inline_entry != NULL) *inline_entry = p->inline_entry;
  return 0;
}

rei_status rei_pool_submit_flags(rei_pool *p, void *task_obj, uint16_t flags,
                                 rei_task *out, double timeout_ms) {
  if (pool_get(p) == NULL) return REI_ERR;
  if (out == NULL) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "expected a task handle out-param");
    return REI_ERR;
  }
  return pool_submit(p, task_obj, flags, out, timeout_ms);
}

/* One doorbell-gated help beat at a runner's batch boundary: clear the
   doorbell, claim one injection entry (unfiltered scan — the bell rang, so
   a stale ready mask must not hide the task it rang for), then re-check
   the rings and restore the doorbell if entries remain — the clear ->
   re-check -> restore discipline pool_claim_rings applies to inj_ready,
   without which a second submitter's store racing the clear is eaten with
   it and that task waits until the map ends. Injection-only, the mirror
   image of collect's help mode (which excludes injection): a blocked
   collect helps to unblock its own subtree, a runner helps precisely to
   hand foreign submitters their chunk-boundary interleave back.

   An ordinary claim executes inline under the help-mode machinery
   (help_depth bounds the recursion). A runner-flagged claim must not: a
   map's runners are its join tickets, and a helper nested inside its own
   cursor drain adds zero parallelism while consuming one — left alone it
   swallows the whole runner set within microseconds (the bell restore
   re-arms it each beat) and silently serializes the map. The claim itself
   stays unfiltered — the ring is SPSC FIFO, so refusing a runner would
   block ordinary tasks queued behind it — but the runner is re-homed onto
   this worker's own deque instead, where an idle peer is woken (bell-less:
   deque work is help-unreachable) or steals it at its next scan, and the
   owner's own pop after its current runner bounds the worst case at
   today's serialization. The flag is read only after the winning head CAS
   (a loser may copy a torn header; a winner's is coherent by the rs_index
   argument), and the announce is cleared *before* the deque push: the
   reverse order would leave the entry both announced and deque-published,
   so a death in that window fires both recovery paths and the survivor's
   late publish can land in a recommitted slot. Clear-first shrinks the
   window to a lost entry, which is benign for runners only — the lane
   never claims the cursor, so the map's abandon path trims it — which is
   why the flag stays runner-only. A full deque (reachable at deque_cap =
   2) falls back to the inline execute, announce intact. Returns 1 when it
   claimed, 0 when not, -1 on an exec_fn infrastructure failure (recorded
   on the handle). */
int rei_pool_help_once(rei_pool *p) {
  if (pool_get(p) == NULL) return -1;
  if (p->role != REI_ROLE_WORKER || p->wk_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a worker handle");
    return -1;
  }
  int got = 0;
  if (p->help_depth < REI_HELP_DEPTH_LIMIT) {
    atomic_store_explicit(p->help_wanted, 0u, memory_order_seq_cst);
    got = pool_claim_rings(p, 0);
    if (got) {
      rei_entry_hdr *eh = (rei_entry_hdr *) p->scratch;
      rei_wk_slot *me = &p->wk[p->wk_slot];
      int64_t b = atomic_load_explicit(&me->deque_bottom,
                                       memory_order_relaxed);
      int64_t t = atomic_load_explicit(&me->deque_top,
                                       memory_order_acquire);
      if ((eh->flags & REI_ENTRY_RUNNER) &&
          b - t < (int64_t) me->deque_cap) {
        /* the space pre-check cannot be invalidated — only the owner
           pushes to its own deque, thieves only free space — so the push
           below cannot fail */
        uint64_t tid = eh->task_id;
        pool_announce_clear(p);
        pool_deque_push(p, p->scratch);
        if (b <= t) pool_unpark_one_worker(p);   /* empty -> non-empty */
        pool_trace_emit(p, REI_TRACE_REHOME, tid);
      } else {
        p->st_helps++;                 /* helps = executed foreign work */
        p->help_depth++;
        int rc = pool_execute(p, 1);
        p->help_depth--;
        if (rc != 0) return -1;
      }
    }
    for (uint32_t s = 0; s < p->hdr.max_submitters; s++) {
      unsigned char *ring = pool_ring(p, s);
      if (atomic_load_explicit(ring_head(ring), memory_order_acquire) <
          atomic_load_explicit(ring_tail(ring), memory_order_acquire)) {
        atomic_store_explicit(p->help_wanted, 1u, memory_order_seq_cst);
        break;
      }
    }
  }
  return got;
}

/* Claim up to n injection entries and queue them on this worker's own
   deque instead of executing them — the deterministic way to populate a
   deque in tests. The full check precedes the claim (space only grows
   once we own the bottom), so a claimed entry can always be queued; the
   announce clears *before* the push, the re-home discipline — the reverse
   order leaves the entry both announced and deque-published, a
   double-recovery state no production path produces. */
int rei_pool_deque_pull(rei_pool *p, uint32_t n) {
  if (pool_get(p) == NULL) return -1;
  if (p->role != REI_ROLE_WORKER || p->wk_slot < 0) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER, "not a worker handle");
    return -1;
  }
  rei_wk_slot *me = &p->wk[p->wk_slot];
  int was_empty = !deque_nonempty(me);
  uint32_t moved = 0;
  while (moved < n) {
    if (atomic_load_explicit(&me->deque_bottom, memory_order_relaxed) -
        atomic_load_explicit(&me->deque_top, memory_order_acquire) >=
        (int64_t) me->deque_cap)
      break;
    if (!pool_claim_rings(p, 1)) break;
    pool_announce_clear(p);
    pool_deque_push(p, p->scratch);
    moved++;
  }
  /* the nested-submit wake rule: a push taking the deque from empty to
     non-empty wakes one parked peer */
  if (moved > 0 && was_empty) pool_unpark_one_worker(p);
  return (int) moved;
}

// Stop ---------------------------------------------------------------------------------

rei_status rei_pool_stop(rei_pool *p, double timeout_ms) {
  if (p == NULL || p->released) return REI_OK;   /* stop is idempotent */
  if (p->role != REI_ROLE_CONTROLLER) {
    rei_err_record(&p->h, REI_ERRCAT_OTHER,
                   "only the controller can stop a pool");
    return REI_ERR;
  }

  pool_shutdown_broadcast(p);

  /* wait for workers to take their clean-exit path; their liveness locks
     release with their fds either way */
  double deadline = timeout_ms < 0 ? -1 : rei_now() + timeout_ms / 1000;
  int clean;
  int interrupted = 0;
  for (;;) {
    clean = 1;
    for (uint32_t i = 0; i < p->hdr.max_workers; i++) {
      int32_t st = atomic_load_explicit(&p->wk[i].status,
                                        memory_order_acquire);
      if (st == REI_WK_LIVE || st == REI_WK_LEAVING ||
          st == REI_WK_CLAIMING)
        clean = 0;
    }
    if (clean || (deadline >= 0 && rei_now() >= deadline)) break;
    for (uint32_t i = 0; i < p->hdr.max_workers; i++)
      rei_unpark(pool_wk_pk(p, i));
    uint32_t e = rei_parker_snapshot(pool_sub_pk(p, 0));
    rei_park_bracket(&p->h.binding, 1);
    rei_park(pool_sub_pk(p, 0), e, 50);
    rei_park_bracket(&p->h.binding, 0);
    if (rei_check_interrupt(&p->h.binding)) {
      /* the teardown still completes — a half-stopped pool is the worse
         outcome; the caller learns the wait was abandoned */
      interrupted = 1;
      break;
    }
  }

  /* teardown sweep: probe + reap whatever did not exit cleanly — dead
     workers (their in-flight tasks fail, their deques orphan) and dead or
     detached submitters (their result slots release). A live hung worker
     stays unreaped, correctly: the lock adjudicates exit, not stall. */
  for (uint32_t i = 0; i < p->hdr.max_workers; i++)
    pool_probe_worker(p, i);
  for (uint32_t j = 0; j < p->hdr.max_submitters; j++)
    pool_probe_submitter(p, j);

  pool_unlink_names(p);
  pool_release(p);
  if (interrupted) return pool_intr(p);
  return clean ? REI_OK : REI_TIMEOUT;
}

// Introspection --------------------------------------------------------------------

/* Snapshots over the wire state (dump adds the handle-local counters):
   cold-path, single loads, states can move mid-fill. */

static void pool_status_fill(const rei_pool *p, rei_pool_status *out) {
  memset(out, 0, sizeof(*out));
  out->size = (uint32_t) sizeof(*out);
  out->name = p->shm.name;
  out->role = p->role;
  out->max_workers = p->hdr.max_workers;
  out->max_submitters = p->hdr.max_submitters;
  out->injection_cap = p->hdr.inj_cap;
  out->result_slots = p->hdr.result_slots;
  out->slot_size = p->hdr.slot;
  uint32_t nw = 0, ns = 0;
  for (uint32_t i = 0; i < p->hdr.max_workers; i++) {
    int32_t st = atomic_load_explicit(&p->wk[i].status,
                                      memory_order_acquire);
    out->worker_state[i] = (uint8_t) st;
    nw += st == REI_WK_LIVE;
    int64_t d =
      atomic_load_explicit(&p->wk[i].deque_bottom, memory_order_acquire) -
      atomic_load_explicit(&p->wk[i].deque_top, memory_order_acquire);
    out->deque_depth[i] = d > 0 ? d : 0;   /* a pop transiently reads -1 */
  }
  out->n_workers = nw;
  for (uint32_t j = 0; j < p->hdr.max_submitters; j++) {
    int32_t st = atomic_load_explicit(&p->sub[j].status,
                                      memory_order_acquire);
    out->sub_state[j] = (uint8_t) st;
    ns += st == REI_SUB_LIVE;
    unsigned char *ring = pool_ring(p, j);
    int64_t q =
      atomic_load_explicit(ring_tail(ring), memory_order_acquire) -
      atomic_load_explicit(ring_head(ring), memory_order_acquire);
    out->inj_queued[j] = q > 0 ? (uint32_t) q : 0;
  }
  out->n_submitters = ns;
  out->parked_mask =
    atomic_load_explicit(p->parked_workers, memory_order_acquire);
  for (uint32_t r = 0; r < p->hdr.result_slots; r++) {
    int32_t st = atomic_load_explicit(&pool_rs(p, r)->status,
                                      memory_order_acquire);
    if (st >= REI_RS_FREE && st <= REI_RS_DIED) out->tasks_by_state[st]++;
  }
  out->shutdown =
    (int32_t) atomic_load_explicit(p->shutdown, memory_order_acquire);
}

rei_status rei_pool_status_get(const rei_pool *p, rei_pool_status *out) {
  if (p == NULL || p->released) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "pool handle is closed");
    return REI_ERR;
  }
  if (out == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "expected a status out-param");
    return REI_ERR;
  }
  pool_status_fill(p, out);
  return REI_OK;
}

rei_status rei_pool_dump_get(const rei_pool *p, rei_pool_dump *out) {
  if (p == NULL || p->released) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "pool handle is closed");
    return REI_ERR;
  }
  if (out == NULL) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "expected a dump out-param");
    return REI_ERR;
  }
  memset(out, 0, sizeof(*out));
  out->size = (uint32_t) sizeof(*out);
  pool_status_fill(p, &out->status);
  out->n_workers = p->hdr.max_workers;
  for (uint32_t i = 0; i < p->hdr.max_workers; i++) {
    const rei_wk_slot *w = &p->wk[i];
    rei_worker_stat *ws = &out->workers[i];
    ws->status = atomic_load_explicit(&w->status, memory_order_acquire);
    ws->park_state =
      atomic_load_explicit(&w->park_state, memory_order_acquire);
    ws->pid = w->pid;
    ws->tasks = atomic_load_explicit(&w->stat_tasks, memory_order_relaxed);
    ws->steals = atomic_load_explicit(&w->stat_steals, memory_order_relaxed);
    ws->injections = atomic_load_explicit(&w->stat_inj, memory_order_relaxed);
    ws->parks = atomic_load_explicit(&w->stat_parks, memory_order_relaxed);
    ws->helps = atomic_load_explicit(&w->stat_helps, memory_order_relaxed);
    ws->deque_top =
      atomic_load_explicit(&w->deque_top, memory_order_acquire);
    ws->deque_bottom =
      atomic_load_explicit(&w->deque_bottom, memory_order_acquire);
    ws->in_flight_rs =
      atomic_load_explicit(&w->in_flight_rs, memory_order_acquire);
  }
  out->n_submitters = p->hdr.max_submitters;
  uint64_t ready = atomic_load_explicit(p->inj_ready, memory_order_acquire);
  uint64_t full = atomic_load_explicit(p->full_waiters,
                                       memory_order_acquire);
  for (uint32_t j = 0; j < p->hdr.max_submitters; j++) {
    const rei_sub_slot *s = &p->sub[j];
    rei_sub_stat *ss = &out->submitters[j];
    unsigned char *ring = pool_ring(p, j);
    ss->status = atomic_load_explicit(&s->status, memory_order_acquire);
    ss->pid = s->pid;
    ss->rs_start = s->rs_start;
    ss->rs_count = s->rs_count;
    ss->injected =
      (uint64_t) atomic_load_explicit(ring_tail(ring), memory_order_acquire);
    ss->claimed =
      (uint64_t) atomic_load_explicit(ring_head(ring), memory_order_acquire);
    ss->spills = atomic_load_explicit(&s->stat_spills, memory_order_relaxed);
    ss->spill_reuse =
      atomic_load_explicit(&s->stat_spill_reuse, memory_order_relaxed);
    ss->ready = (int32_t) ((ready >> j) & 1);
    ss->full_waiter = (int32_t) ((full >> j) & 1);
  }
  /* the spill free list, consumer mapping cache, and collect-side park
     count are handle-local, so their counters surface here rather than in
     the cross-process stats (stat_parks covers only worker parks) */
  out->fl_entries = p->h.fl.n;
  out->fl_bytes = p->h.fl.total;
  out->fl_hits = p->h.fl.hits;
  out->open_hits = p->h.oc.hits;
  out->open_misses = p->h.oc.misses;
  out->collect_parks = p->st_collect_parks;
  out->ledger_entries = p->h.fl.led_n;
  out->help_wanted =
    (int32_t) atomic_load_explicit(p->help_wanted, memory_order_acquire);
  return REI_OK;
}

rei_status rei_pool_tasks_get(const rei_pool *p, rei_rs_row *out,
                              uint32_t cap, uint32_t *n_out) {
  if (p == NULL || p->released) {
    rei_err_record_tls(REI_ERRCAT_OTHER, "pool handle is closed");
    return REI_ERR;
  }
  if (n_out == NULL || (cap > 0 && out == NULL)) {
    rei_err_record_tls(REI_ERRCAT_OTHER,
                       "expected a row buffer and a count out-param");
    return REI_ERR;
  }
  uint32_t total = 0, m = 0;
  for (uint32_t r = 0; r < p->hdr.result_slots; r++) {
    rei_rs_hdr *rs = pool_rs(p, r);
    int32_t st = atomic_load_explicit(&rs->status, memory_order_acquire);
    if (st == REI_RS_FREE) continue;
    if (m < cap) {
      out[m].slot = r;
      out[m].status = st;
      out[m].sequence =
        atomic_load_explicit(&rs->sequence, memory_order_relaxed);
      out[m].worker_slot =
        atomic_load_explicit(&rs->worker_slot, memory_order_acquire);
      out[m].waiter_slot =
        atomic_load_explicit(&rs->waiter_slot, memory_order_acquire);
      m++;
    }
    total++;
  }
  *n_out = total;
  return REI_OK;
}

// Handle error access ------------------------------------------------------------------

rei_errcat rei_pool_errcat(const rei_pool *p) {
  return p != NULL ? p->h.errcat : rei_last_error_category();
}

const char *rei_pool_error(const rei_pool *p) {
  return p != NULL ? p->h.errmsg : rei_last_error_message();
}

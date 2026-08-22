/* Soak tier: full-duplex channel contention between two processes for
   REI_SOAK_SECONDS (the make default is 120; nightly runs set it
   higher). Both sides send sequence-tagged payloads across the
   INLINE / ARENA / SHM_RAW tiers while draining; every received
   seq-bearing message must carry its direction's next sequence number
   and an intact fill pattern (NIL messages carry none).

   Shutdown is the channel's close discipline: the close bit means "I
   have drained the peer's messages", so neither side may signal before
   draining. Each side ends its sends with an in-band DONE message
   carrying its total; the ring is FIFO per direction, so a received
   DONE means every message before it arrived and verified — the count
   is a cross-check. Only then does a side signal and rendezvous. POSIX
   (fork); a stub passes elsewhere. Run via `make test-soak`. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#ifdef _WIN32

int main(void) {
  puts("soak_channel: skipped on Windows (fork-based)");
  return 0;
}

#else

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PAYLOAD (1u << 20)

static uint64_t rng_state;

static uint64_t rnd(void) {
  uint64_t x = rng_state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng_state = x;
  return x * 0x2545F4914F6CDD1DULL;
}

/* 10% NIL, 45% INLINE (8-32 B), 35% ARENA (512 B-32 KiB), 10% SHM_RAW
   (100-1024 KiB). Seq-bearing payloads are always >= 8 bytes. */
static size_t pick_len(void) {
  uint64_t r = rnd() % 100;
  if (r < 10) return 0;
  if (r < 55) return 8 + rnd() % 25;
  if (r < 90) return 512 + rnd() % 32257;
  return (100 + rnd() % 925) << 10;
}

/* bytes [0-7]: the direction's sequence number, LE; the rest its low byte */
static void fill(uint8_t *buf, size_t len, uint64_t seq) {
  for (int i = 0; i < 8; i++) buf[i] = (uint8_t) (seq >> (8 * i));
  memset(buf + 8, (uint8_t) seq, len - 8);
}

static void verify(const rei_bytes *b, uint64_t expect_seq) {
  assert(b->len >= 8);
  const uint8_t *d = b->data;
  uint64_t seq = 0;
  for (int i = 0; i < 8; i++) seq |= (uint64_t) d[i] << (8 * i);
  assert(seq == expect_seq);
  for (size_t i = 8; i < b->len; i++) assert(d[i] == (uint8_t) seq);
}

/* The end-of-stream marker: a 16-byte frame — magic, then the sender's
   total message count, LE. The magic's high bytes cannot alias a
   seq-tagged payload's small-integer header. */
static const uint8_t DONE_MAGIC[8] = { 0xD0, 0x0D, 0xFE, 0xED,
                                       0xBE, 0x51, 0xC0, 0xDE };

/* Per-direction run state. NIL sends carry no sequence number and skip
   it on both sides. */
struct traffic {
  uint64_t sent, received;
  uint64_t send_seq, recv_seq;
  uint64_t peer_sent;   /* from the peer's DONE */
  int done;             /* the peer's DONE arrived (its ring is drained) */
};

static void send_round(rei_channel *c, uint8_t *buf, struct traffic *t) {
  for (int k = 0; k < 4; k++) {
    size_t len = pick_len();
    if (len != 0) fill(buf, len, t->send_seq);
    rei_bytes b = { buf, len };
    rei_status st = rei_channel_send(c, &b);
    if (st == REI_OK) {
      t->sent++;
      if (len != 0) t->send_seq++;
    } else if (st == REI_FULL) {
      break;
    } else {
      assert(0);   /* no close and no peer death before the shutdown */
    }
  }
}

static void recv_one(rei_bytes *b, struct traffic *t) {
  if (b->len == sizeof(DONE_MAGIC) + 8 &&
      memcmp(b->data, DONE_MAGIC, 8) == 0) {
    const uint8_t *d = b->data;
    for (int i = 0; i < 8; i++)
      t->peer_sent |= (uint64_t) d[8 + i] << (8 * i);
    t->done = 1;   /* FIFO: everything the peer sent arrived first */
    return;
  }
  if (b->len != 0) verify(b, t->recv_seq++);
  t->received++;
}

static int recv_round(rei_channel *c, struct traffic *t, long timeout_ms) {
  void *objs[16];
  size_t n = 0;
  rei_status st = rei_channel_recv_batch(c, objs, 16, &n, timeout_ms);
  assert(st == REI_OK || st == REI_TIMEOUT);
  for (size_t i = 0; i < n; i++) {
    recv_one(objs[i], t);
    rei_bytes_free(objs[i]);
  }
  return n != 0;
}

/* The whole lifecycle: traffic until the deadline, then the DONE
   handshake, then the close rendezvous. */
static void run_traffic(rei_channel *c, double seconds, struct traffic *t) {
  uint8_t *buf = malloc(MAX_PAYLOAD);
  assert(buf != NULL);
  double deadline = rei_now() + seconds;
  while (rei_now() < deadline) {
    send_round(c, buf, t);
    recv_round(c, t, 0);
  }

  /* send DONE: the ring may be full; the peer is draining, so a drain
     round here makes room (and both sides make progress) */
  uint8_t done_msg[16];
  memcpy(done_msg, DONE_MAGIC, 8);
  for (int i = 0; i < 8; i++)
    done_msg[8 + i] = (uint8_t) (t->sent >> (8 * i));
  for (;;) {
    rei_bytes b = { done_msg, sizeof(done_msg) };
    rei_status st = rei_channel_send(c, &b);
    if (st == REI_OK) break;
    assert(st == REI_FULL);
    recv_round(c, t, 100);
  }

  /* drain until the peer's DONE: every earlier message verified */
  while (!t->done) recv_round(c, t, 1000);
  assert(t->received == t->peer_sent);

  /* the close discipline: my bit only now — I have drained the peer */
  assert(rei_channel_close_signal(c) == REI_OK);
  assert(rei_channel_close(c, 30000) == REI_OK);
  free(buf);
}

int main(void) {
  const char *env = getenv("REI_SOAK_SECONDS");
  double seconds = env != NULL ? atof(env) : 120;
  assert(seconds > 0);

  rei_binding b;
  rei_binding_bytes(&b);
  rei_channel_opts opts;
  rei_channel_opts_init(&opts);
  opts.capacity = 1024;
  opts.slot_size = 512;
  opts.arena_size = 8u << 20;

  rei_channel *host = NULL;
  assert(rei_channel_create(&host, &opts, &b) == REI_OK);
  char token[64];
  assert(rei_channel_token(host, token, sizeof(token)) == REI_OK);

  fflush(stdout);
  pid_t pid = fork();
  assert(pid >= 0);

  if (pid == 0) {                       /* child: the peer */
    rng_state = 0x9E3779B97F4A7C15ULL;
    rei_channel *peer = NULL;
    assert(rei_channel_attach(&peer, token, &b) == REI_OK);
    assert(rei_channel_ready_set(peer) == REI_OK);
    struct traffic t = { 0 };
    run_traffic(peer, seconds, &t);
    rei_channel_destroy(peer);
    _exit(0);
  }

  rng_state = 0x2545F4914F6CDD1DULL;
  assert(rei_channel_ready_wait(host, 30000) == REI_OK);
  struct traffic t = { 0 };
  run_traffic(host, seconds, &t);
  rei_channel_destroy(host);

  int st = 0;
  assert(waitpid(pid, &st, 0) == pid);
  assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  printf("soak_channel: ok (%llu msgs out, %llu in, %.0f s)\n",
         (unsigned long long) t.sent, (unsigned long long) t.received,
         seconds);
  return 0;
}

#endif

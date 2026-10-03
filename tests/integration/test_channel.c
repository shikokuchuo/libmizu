/* Integration tier: the channel fork guard. A forked child inherits a
   copy of the parent's handle: every verb must fail MIZU_ERR without
   touching shared state, the total probes answer empty, and the parent's
   channel must be uncorrupted afterwards — a real peer attaches by token
   and echoes. Compiles against the installed headers (mizu.h + mizu_ext.h
   — the bytes binding is ext-tier surface) with internal.h absent: the
   API's compile-time contract check.
   POSIX (fork); a stub passes elsewhere. Run via `make test-integration`. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mizu.h"
#include "mizu_ext.h"

#ifdef _WIN32

int main(void) {
  puts("test_channel integration: skipped on Windows (fork-based)");
  return 0;
}

#else

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static char token[64];
static mizu_channel *host;   /* the handle the misuser inherits */

/* The forked misuser: every verb on the inherited handle fails MIZU_ERR
   and the probes answer empty — and, the point, none of it may touch the
   shared region. Distinct exit codes pinpoint the first violation. */
static void run_misuser(void) {
  char buf[64];
  if (mizu_channel_token(host, buf, sizeof buf) != MIZU_ERR) _exit(3);
  if (mizu_channel_ready_set(host) != MIZU_ERR) _exit(4);
  mizu_bytes tb = { (void *) "x", 1 };
  if (mizu_channel_send(host, &tb) != MIZU_ERR) _exit(5);
  void *one[1] = { &tb };
  size_t acc = 99;
  if (mizu_channel_send_batch(host, one, 1, &acc) != MIZU_ERR || acc != 0)
    _exit(6);
  void *obj = NULL;
  if (mizu_channel_recv(host, &obj, 0) != MIZU_ERR) _exit(7);
  size_t n = 99;
  if (mizu_channel_recv_batch(host, one, 1, &n, 0) != MIZU_ERR || n != 0)
    _exit(8);
  if (mizu_channel_close_signal(host) != MIZU_ERR) _exit(9);
  if (mizu_channel_close(host, 0) != MIZU_ERR) _exit(10);
  if (mizu_channel_alive(host) != 0) _exit(11);
  if (mizu_channel_peer_ident(host) != 0) _exit(12);
  mizu_channel_info info;
  memset(&info, 0, sizeof info);
  if (mizu_channel_info_get(host, &info) != MIZU_ERR) _exit(13);
  if (strcmp(mizu_channel_error(host),
             "channel handles do not survive fork()") != 0)
    _exit(14);
  _exit(0);   /* deliberately no destroy: teardown is the parent's */
}

/* The real peer: attach by token, echo until CLOSED. */
static void run_peer(void) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel *p;
  if (mizu_channel_attach(&p, token, &b) != MIZU_OK) _exit(15);
  if (mizu_channel_ready_set(p) != MIZU_OK) _exit(16);
  for (;;) {
    void *obj = NULL;
    mizu_status st = mizu_channel_recv(p, &obj, 5000);
    if (st == MIZU_CLOSED) break;
    if (st != MIZU_OK) _exit(17);
    if (mizu_channel_send(p, obj) != MIZU_OK) _exit(18);
    mizu_bytes_free(obj);
  }
  mizu_channel_destroy(p);
  _exit(0);
}

static int wait_exit(pid_t pid) {
  int st = 0;
  assert(waitpid(pid, &st, 0) == pid);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

static void test_fork_guard(void) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 64;
  opts.slot_size = 512;
  assert(mizu_channel_create(&host, &opts, &b) == MIZU_OK);
  assert(mizu_channel_token(host, token, sizeof token) == MIZU_OK);

  fflush(stdout);
  pid_t m = fork();
  assert(m >= 0);
  if (m == 0) run_misuser();
  assert(wait_exit(m) == 0);

  /* nothing the misuser did may leak into the live channel */
  pid_t p = fork();
  assert(p >= 0);
  if (p == 0) run_peer();
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  mizu_bytes tb = { (void *) "ping", 4 };
  assert(mizu_channel_send(host, &tb) == MIZU_OK);
  void *obj = NULL;
  assert(mizu_channel_recv(host, &obj, 5000) == MIZU_OK);
  mizu_bytes *rb = obj;
  assert(rb->len == 4 && memcmp(rb->data, "ping", 4) == 0);
  mizu_bytes_free(rb);
  assert(mizu_channel_close(host, 5000) == MIZU_OK);
  assert(wait_exit(p) == 0);
  mizu_channel_destroy(host);
  puts("ok fork_guard");
}

int main(void) {
  test_fork_guard();
  puts("test_channel integration: all passed");
  return 0;
}

#endif

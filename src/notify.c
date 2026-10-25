/* Notify endpoints: the named, pollable wake object an armed consumer
   owns and the publisher signals at its publish checkpoint. POSIX is a
   bound AF_UNIX datagram socket — the abstract namespace on Linux (no
   filesystem entry, no unlink races, auto-cleanup on close), a
   livedir-derived path on macOS (the sun_path budget is the arm's
   recorded failure); Windows is an auto-reset named event (SetEvent
   while signaled coalesces, the wait consumes it — edge-triggered for
   free). The watch registration itself rides the death-listener
   machinery in the three wait_*.c. The contract is the ext-tier comment
   block on mizu_channel_notify_arm. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"

#ifndef _WIN32

#include <stddef.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct mizu_notify_ep_s {
  int fd;
  struct sockaddr_un addr;   /* the bound address (the unlink decision) */
  socklen_t addrlen;
};

struct mizu_notify_tx_s {
  int fd;
  struct sockaddr_un addr;   /* the peer endpoint's address */
  socklen_t addrlen;
};

/* The endpoint address for (livedir, region, tag). Both sides derive it
   from the region name and the recorded livedir — never resolved
   independently. */
static int notify_addr(const char *livedir, const char *region,
                       const char *tag, struct sockaddr_un *sa,
                       socklen_t *len, const char **err) {
  const char *suffix = region + strlen(MIZU_PREFIX_LITERAL);
  memset(sa, 0, sizeof(*sa));
  sa->sun_family = AF_UNIX;
#ifdef __linux__
  (void) livedir;
  int n = snprintf(sa->sun_path + 1, sizeof(sa->sun_path) - 1,
                   "mizu_%s.ntfy.%s", suffix, tag);
  if (n <= 0 || (size_t) n >= sizeof(sa->sun_path) - 1) {
    *err = "notify endpoint name too long";
    return -1;
  }
  *len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + (size_t) n);
#else
  int n = snprintf(sa->sun_path, sizeof(sa->sun_path),
                   "%s/mizu_%s.ntfy.%s", livedir, suffix, tag);
  if (n <= 0 || (size_t) n >= sizeof(sa->sun_path)) {
    *err = "livedir too long for a notify endpoint; set the "
           MIZU_LIVENESS_DIR_ENV " override";
    return -1;
  }
  *len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + (size_t) n + 1);
#endif
  return 0;
}

static int notify_socket(void) {
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  (void) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  (void) fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int one = 1;
  (void) setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  return fd;
}

mizu_notify_ep *mizu_notify_ep_open(const char *livedir, const char *region,
                                  const char *tag, const char **err) {
  struct sockaddr_un sa;
  socklen_t len;
  if (notify_addr(livedir, region, tag, &sa, &len, err) != 0) return NULL;
  int fd = notify_socket();
  if (fd < 0) {
    *err = "cannot create notify endpoint socket";
    return NULL;
  }
  /* unlink-before-bind: a dead holder's socket-path residue (macOS — the
     .live.* discipline) never blocks a fresh endpoint; suffix uniqueness
     makes a live same-path holder impossible */
  if (sa.sun_path[0] != '\0') (void) unlink(sa.sun_path);
  if (bind(fd, (struct sockaddr *) &sa, len) != 0) {
    *err = "cannot bind notify endpoint";
    close(fd);
    return NULL;
  }
  mizu_notify_ep *ep = malloc(sizeof(*ep));
  if (ep == NULL) {
    *err = "allocation failure";
    close(fd);
    if (sa.sun_path[0] != '\0') (void) unlink(sa.sun_path);
    return NULL;
  }
  ep->fd = fd;
  ep->addr = sa;
  ep->addrlen = len;
  return ep;
}

intptr_t mizu_notify_ep_handle(const mizu_notify_ep *ep) {
  return (intptr_t) ep->fd;
}

void mizu_notify_ep_drain(mizu_notify_ep *ep) {
  char buf[256];
  while (recv(ep->fd, buf, sizeof(buf), 0) > 0) { }
}

void mizu_notify_ep_close(mizu_notify_ep *ep, int unlink_path) {
  close(ep->fd);
  if (unlink_path != 0 && ep->addr.sun_path[0] != '\0')
    (void) unlink(ep->addr.sun_path);
  free(ep);
}

mizu_notify_tx *mizu_notify_tx_open(const char *livedir, const char *region,
                                  const char *tag) {
  const char *err = NULL;
  struct sockaddr_un sa;
  socklen_t len;
  if (notify_addr(livedir, region, tag, &sa, &len, &err) != 0) return NULL;
  int fd = notify_socket();
  if (fd < 0) return NULL;
  mizu_notify_tx *tx = malloc(sizeof(*tx));
  if (tx == NULL) {
    close(fd);
    return NULL;
  }
  tx->fd = fd;
  tx->addr = sa;
  tx->addrlen = len;
  return tx;
}

void mizu_notify_tx_send(mizu_notify_tx *tx) {
  static const char sig[1] = { 'n' };
  /* fire-and-forget: a stale or absent endpoint (ENOENT) and a full
     receive buffer (EAGAIN — a signal is already queued) are absorbed;
     the liveness lock is the death verdict */
#ifdef MSG_NOSIGNAL
  (void) sendto(tx->fd, sig, sizeof(sig), MSG_NOSIGNAL,
                (struct sockaddr *) &tx->addr, tx->addrlen);
#else
  (void) sendto(tx->fd, sig, sizeof(sig), 0,
                (struct sockaddr *) &tx->addr, tx->addrlen);
#endif
}

void mizu_notify_tx_close(mizu_notify_tx *tx) {
  close(tx->fd);
  free(tx);
}

#else /* _WIN32 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct mizu_notify_ep_s { HANDLE ev; };
struct mizu_notify_tx_s { HANDLE ev; };

/* "Local\mizu_<suffix>.ntfy.<tag>" — the parker event's name discipline. */
static int notify_name(const char *region, const char *tag, char *buf,
                       size_t cap) {
  int n = snprintf(buf, cap, "%s.ntfy.%s", region, tag);
  return (n > 0 && (size_t) n < cap) ? 0 : -1;
}

mizu_notify_ep *mizu_notify_ep_open(const char *livedir, const char *region,
                                  const char *tag, const char **err) {
  (void) livedir;
  char name[MIZU_NAME_MAX + 16];
  if (notify_name(region, tag, name, sizeof(name)) != 0) {
    *err = "notify endpoint name too long";
    return NULL;
  }
  HANDLE ev = CreateEventA(NULL, FALSE, FALSE, name);
  if (ev == NULL) {
    *err = "cannot create notify endpoint event";
    return NULL;
  }
  mizu_notify_ep *ep = malloc(sizeof(*ep));
  if (ep == NULL) {
    CloseHandle(ev);
    *err = "allocation failure";
    return NULL;
  }
  ep->ev = ev;
  return ep;
}

intptr_t mizu_notify_ep_handle(const mizu_notify_ep *ep) {
  return (intptr_t) ep->ev;
}

void mizu_notify_ep_drain(mizu_notify_ep *ep) {
  /* consume a pending stale signal: the zero-timeout wait absorbs it
     (auto-reset — at most one is pending) */
  (void) WaitForSingleObject(ep->ev, 0);
}

void mizu_notify_ep_close(mizu_notify_ep *ep, int unlink_path) {
  (void) unlink_path;   /* kernel object: no unlink step */
  CloseHandle(ep->ev);
  free(ep);
}

mizu_notify_tx *mizu_notify_tx_open(const char *livedir, const char *region,
                                  const char *tag) {
  (void) livedir;
  char name[MIZU_NAME_MAX + 16];
  if (notify_name(region, tag, name, sizeof(name)) != 0) return NULL;
  HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
  if (ev == NULL) return NULL;
  mizu_notify_tx *tx = malloc(sizeof(*tx));
  if (tx == NULL) {
    CloseHandle(ev);
    return NULL;
  }
  tx->ev = ev;
  return tx;
}

void mizu_notify_tx_send(mizu_notify_tx *tx) {
  SetEvent(tx->ev);
}

void mizu_notify_tx_close(mizu_notify_tx *tx) {
  CloseHandle(tx->ev);
  free(tx);
}

#endif /* _WIN32 */

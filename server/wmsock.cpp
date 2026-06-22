/* Copyright (C) 2026 Nemesis project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * wmsock.cpp -- Nemesis WM side-channel socket.
 *
 * Mirrors the Twin-server unix-socket bootstrap from util.cpp:InitTWDisplay
 * but on a parallel path (".Twin<TWDISPLAY>-wm"). No TwinAuth: the socket
 * lives at TmpDir (typically /tmp, world-writable but sticky). Access is
 * gated by the socket file mode (0600) plus the kernel's UID check on
 * connect(2); only the server's own uid can open this socket. Same trust
 * model as Twin's main libtw socket.
 */

#include "wmsock.h"

#include "twin.h"
#include "main.h"
#include "log.h"
#include "methods.h" // SendMsg()
#include "remote.h"
#include "extreg.h"
#include "util.h"
#include "obj/event.h"
#include "obj/msg.h"
#include "obj/widget.h"
#include "obj/screen.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* EOF/error watcher; phase 4F will need a larger buffer for inbound commands. */
#define WMSOCK_READ_BUFSZ 256
/* Current map event is ~110 bytes; margin left for format growth in phases 4B-4F. */
#define WMSOCK_JSON_LINE_BUFSZ 160
/* One WM at a time, by design. See wmsock.h doc-block. */
#define WMSOCK_LISTEN_BACKLOG 1

static uldat wmListenSlot = NOSLOT;
static uldat wmConnSlot = NOSLOT;
static struct sockaddr_un wmAddr;
static bool wmPathBound = false;

static void wmCloseConn(void) {
  if (wmConnSlot != NOSLOT) {
    UnRegisterRemote(wmConnSlot);
    wmConnSlot = NOSLOT;
  }
  if (Ext(WM, WMConnFd) >= 0) {
    close(Ext(WM, WMConnFd));
    Ext(WM, WMConnFd) = -1;
  }
}

/* Connected-fd read handler.
 * Phase-4A: WM is observer-only. We only watch for EOF/error here so we can
 * release the slot when the WM disconnects. Inbound commands come in Phase 4F.
 */
static void wmConnRead(int fd, uldat /*slot*/) {
  char buf[WMSOCK_READ_BUFSZ];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      /* Drop. Inbound parser will live here in Phase 4F. */
      continue;
    }
    if (n == 0) {
      log(INFO) << "twin: WM side-channel: external WM disconnected\n";
      wmCloseConn();
      return;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    log(WARNING) << "twin: WM side-channel: read error: " << Chars::from_c(strerror(errno)) << "\n";
    wmCloseConn();
    return;
  }
}

/* Listen-fd accept handler. Registered via RegisterRemoteFd. */
static void wmListenAccept(int /*fd*/, uldat /*slot*/) {
  struct sockaddr_un un_addr = {};
  socklen_t len = sizeof(un_addr);
  int newFd = accept(Ext(WM, WMListenFd), (struct sockaddr *)&un_addr, &len);
  if (newFd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      log(WARNING) << "twin: WM side-channel: accept failed: " << Chars::from_c(strerror(errno))
                   << "\n";
    return;
  }
  if (Ext(WM, WMConnFd) >= 0) {
    /* Already attached; refuse the second. */
    log(WARNING) << "twin: WM side-channel: rejected extra attach (already attached)\n";
    close(newFd);
    return;
  }
  fcntl(newFd, F_SETFL, O_NONBLOCK);
  fcntl(newFd, F_SETFD, FD_CLOEXEC);

  uldat slot = RegisterRemoteFd(newFd, wmConnRead);
  if (slot == NOSLOT) {
    close(newFd);
    log(WARNING) << "twin: WM side-channel: RegisterRemoteFd failed\n";
    return;
  }
  Ext(WM, WMConnFd) = newFd;
  wmConnSlot = slot;
  log(INFO) << "twin: WM side-channel: external WM attached on fd " << newFd << "\n";
}

bool WMSockInit(void) {
  if (Ext(WM, WMListenFd) >= 0)
    return true;

  if (!TWDisplay) {
    log(WARNING) << "twin: WM side-channel: TWDISPLAY not set, skipping\n";
    return false;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    log(WARNING) << "twin: WM side-channel: socket(AF_UNIX): " << Chars::from_c(strerror(errno))
                 << "\n";
    return false;
  }

  memset(&wmAddr, 0, sizeof(wmAddr));
  wmAddr.sun_family = AF_UNIX;
  udat pos = CopyToSockaddrUn(TmpDir.data(), &wmAddr, 0);
  pos = CopyToSockaddrUn("/.Twin", &wmAddr, pos);
  pos = CopyToSockaddrUn(TWDisplay, &wmAddr, pos);
  pos = CopyToSockaddrUn("-wm", &wmAddr, pos);
  /* CopyToSockaddrUn silently truncates if sun_path is full (util.cpp:1171).
   * Guard against a malformed bind path when TmpDir + TWDISPLAY is too long. */
  if (pos >= sizeof(wmAddr.sun_path) - 1) {
    log(WARNING) << "twin: WM side-channel: socket path too long, skipping\n";
    close(fd);
    return false;
  }

  /* Stale socket cleanup: try to connect; if it fails, unlink. */
  {
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
      if (connect(probe, (struct sockaddr *)&wmAddr, sizeof(wmAddr)) < 0) {
        unlink(wmAddr.sun_path);
      } else {
        /* Someone is already listening; refuse to bind. */
        close(probe);
        close(fd);
        log(WARNING) << "twin: WM side-channel: another server owns "
                     << Chars::from_c(wmAddr.sun_path) << "\n";
        return false;
      }
      close(probe);
    }
  }

  if (bind(fd, (struct sockaddr *)&wmAddr, sizeof(wmAddr)) < 0) {
    log(WARNING) << "twin: WM side-channel: bind " << Chars::from_c(wmAddr.sun_path) << ": "
                 << Chars::from_c(strerror(errno)) << "\n";
    close(fd);
    return false;
  }
  wmPathBound = true;

  if (chmod(wmAddr.sun_path, 0600) < 0 || listen(fd, WMSOCK_LISTEN_BACKLOG) < 0 ||
      fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
    log(WARNING) << "twin: WM side-channel: chmod/listen/fcntl: "
                 << Chars::from_c(strerror(errno)) << "\n";
    unlink(wmAddr.sun_path);
    wmPathBound = false;
    close(fd);
    return false;
  }

  uldat slot = RegisterRemoteFd(fd, wmListenAccept);
  if (slot == NOSLOT) {
    log(WARNING) << "twin: WM side-channel: RegisterRemoteFd failed\n";
    unlink(wmAddr.sun_path);
    wmPathBound = false;
    close(fd);
    return false;
  }

  Ext(WM, WMListenFd) = fd;
  wmListenSlot = slot;
  log(INFO) << "twin: WM side-channel: listening on " << Chars::from_c(wmAddr.sun_path) << "\n";
  return true;
}

void WMSockShutdown(void) {
  wmCloseConn();
  if (wmListenSlot != NOSLOT) {
    UnRegisterRemote(wmListenSlot);
    wmListenSlot = NOSLOT;
  }
  if (Ext(WM, WMListenFd) >= 0) {
    close(Ext(WM, WMListenFd));
    Ext(WM, WMListenFd) = -1;
  }
  if (wmPathBound) {
    unlink(wmAddr.sun_path);
    wmPathBound = false;
  }
}

/* Best-effort write; never blocks the server. On EPIPE we drop the client.
 * SIGPIPE is SIG_IGN process-wide via InitSignals (hw.cpp:155, 210-211),
 * so write() returns -1/EPIPE instead of killing the server.
 */
static void wmConnWriteLine(const char *buf, size_t len) {
  if (Ext(WM, WMConnFd) < 0)
    return;

  size_t off = 0;
  while (off < len) {
    ssize_t n = write(Ext(WM, WMConnFd), buf + off, len - off);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EINTR))
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      /* Phase-4A: a slow WM that won't drain its read buffer is its own
       * problem; we drop the message instead of growing an in-server queue.
       * Phase 4F will replace this with RemoteWriteQueue / RemoteFlush. */
      break;
    }
    /* EPIPE / ECONNRESET / other: tear down. */
    log(WARNING) << "twin: WM side-channel: write error, dropping client: "
                 << Chars::from_c(strerror(errno)) << "\n";
    wmCloseConn();
    break;
  }
}

/* Format one JSON line and ship it. Central truncation guard: silent drops
 * are debuggable only post-mortem, so any oversize event surfaces as a
 * warning. New event types added in phases 4B-4E go through here. */
static void wmSendJsonLine(const char *fmt, ...) {
  if (Ext(WM, WMConnFd) < 0)
    return;

  char line[WMSOCK_JSON_LINE_BUFSZ];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  if (n > 0 && (size_t)n < sizeof(line)) {
    wmConnWriteLine(line, (size_t)n);
  } else if (n >= (int)sizeof(line)) {
    log(WARNING) << "twin: WM side-channel: JSON event truncated (" << n
                 << " >= " << (int)sizeof(line) << "), dropped\n";
  }
}

/* Twin uses dat = int16; cast to long for portable %ld printing. */
static void wmSendMap(Tmsg msg) {
  event_map &ev = msg->Event.EventMap;
  Twidget w = ev.W;
  Tscreen screen = ev.Screen;
  if (!w)
    return;
  wmSendJsonLine("{\"type\":\"map\",\"wid\":%lu,\"screen\":%lu,"
                 "\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld}\n",
                 (unsigned long)w->Id, (unsigned long)(screen ? screen->Id : 0),
                 (long)w->Left, (long)w->Up, (long)w->XWidth, (long)w->YWidth);
}

static void wmSendMouse(Tmsg msg) {
  event_mouse &ev = msg->Event.EventMouse;
  wmSendJsonLine("{\"type\":\"mouse\",\"code\":%lu,\"shift\":%lu,"
                 "\"x\":%ld,\"y\":%ld}\n",
                 (unsigned long)ev.Code, (unsigned long)ev.ShiftFlags, (long)ev.X, (long)ev.Y);
}

/* Hex-encode AsciiSeq so control bytes ride through JSON without escaping.
 * Realistic Esc sequences are <= 8 bytes; the cap is a guardrail -- a
 * pathological SeqLen would otherwise inflate past WMSOCK_JSON_LINE_BUFSZ and
 * trip the truncation warning in wmSendJsonLine, dropping the event silently
 * from the WM's perspective. Caller-side cap keeps the warning informative. */
static void wmSendKey(Tmsg msg) {
  event_keyboard &ev = msg->Event.EventKeyboard;
  static const char hexchars[] = "0123456789abcdef";
  char hex[64 * 2 + 1];
  size_t n = (size_t)ev.SeqLen;
  if (n > sizeof(hex) / 2 - 1) {
    n = sizeof(hex) / 2 - 1;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char b = (unsigned char)ev.AsciiSeq[i];
    hex[2 * i] = hexchars[b >> 4];
    hex[2 * i + 1] = hexchars[b & 0xF];
  }
  hex[2 * n] = '\0';

  wmSendJsonLine("{\"type\":\"key\",\"code\":%lu,\"shift\":%lu,\"seq\":\"%s\"}\n",
                 (unsigned long)ev.Code, (unsigned long)ev.ShiftFlags, hex);
}

void WMSockSendFocus(Twidget newW, Twidget oldW) {
  if (Ext(WM, WMConnFd) < 0)
    return;

  wmSendJsonLine("{\"type\":\"focus\",\"wid\":%lu,\"old_wid\":%lu}\n",
                 (unsigned long)(newW ? newW->Id : 0),
                 (unsigned long)(oldW ? oldW->Id : 0));
}

void WMSockSendUnmap(Twidget w, Tscreen screen) {
  if (Ext(WM, WMConnFd) < 0 || !w)
    return;

  wmSendJsonLine("{\"type\":\"unmap\",\"wid\":%lu,\"screen\":%lu}\n",
                 (unsigned long)w->Id, (unsigned long)(screen ? screen->Id : 0));
}

void SendMsgToWM(Tmsg msg) {
  /* Built-in WM stays authoritative -- SendMsg first, mirror second. */
  SendMsg(Ext(WM, MsgPort), msg);

  if (Ext(WM, WMConnFd) < 0)
    return;

  switch (msg->Type) {
  case msg_map:
    wmSendMap(msg);
    break;
  case msg_mouse:
    wmSendMouse(msg);
    break;
  case msg_key:
    wmSendKey(msg);
    break;
  default:
    break;
  }
}

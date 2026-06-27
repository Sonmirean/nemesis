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

#include "obj/id.h"
#include "obj/magic.h"
#include "resize.h"

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
#include <stdlib.h>

/* Per-connection inbound line buffer. One line at a time; excess backpressures
 * the external WM's write() via kernel socket buffer. Phase 4F adds queuing. */
#define WMSOCK_READ_BUFSZ 256
/* Current map event is ~110 bytes; margin left for format growth in phases 4B-4F. */
#define WMSOCK_JSON_LINE_BUFSZ 160
/* One WM at a time, by design. See wmsock.h doc-block. */
#define WMSOCK_LISTEN_BACKLOG 1

static uldat wmListenSlot = NOSLOT;
static uldat wmConnSlot = NOSLOT;
static struct sockaddr_un wmAddr;
static bool wmPathBound = false;

/* Inbound line accumulator. Holds bytes read from the connected WM that have
 * not yet been terminated by '\n'. Sized to the same cap as the read buffer;
 * a line longer than this is rejected with a malformed reply. */
static char wmInBuf[WMSOCK_READ_BUFSZ];
static int wmInBufLen = 0;

/* Phase 4D config flags. Set via WMSockSetFlags(); defaults match the spec
 * (offscreen forbidden, debug echo off). Finalized twinrc integration deferred. */
static bool wmAllowOffscreen = false;
static bool wmDebugReplies = false;

static void wmCloseConn(void) {
  if (wmConnSlot != NOSLOT) {
    UnRegisterRemote(wmConnSlot);
    wmConnSlot = NOSLOT;
  }
  if (Ext(WM, WMConnFd) >= 0) {
    close(Ext(WM, WMConnFd));
    Ext(WM, WMConnFd) = -1;
  }
  wmInBufLen = 0;
}

/* ---------- inbound command infrastructure (Phase 4D) ---------- */

/* Forward declaration: wmSendJsonLine is defined after this block. */
static void wmSendJsonLine(const char *fmt, ...);

/* Emit a reply line. hasId=false means the id field could not be parsed;
 * in that case the reply omits id (§6.3 of the protocol spec). */
static void wmSendReply(bool hasId, unsigned long id, bool ok, const char *error) {
  if (ok) {
    if (hasId)
      wmSendJsonLine("{\"reply\":true,\"id\":%lu,\"ok\":true}\n", id);
    else
      wmSendJsonLine("{\"reply\":true,\"ok\":true}\n");
  } else {
    if (hasId)
      wmSendJsonLine("{\"reply\":true,\"id\":%lu,\"ok\":false,\"error\":\"%s\"}\n", id, error);
    else
      wmSendJsonLine("{\"reply\":true,\"ok\":false,\"error\":\"%s\"}\n", error);
  }
}

static const char *wmSkipWs(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r')
    p++;
  return p;
}

/* Advance past a JSON string literal at p (which must be at the opening ").
 * Does not store the value; used for skipping unknown fields. */
static const char *wmSkipStr(const char *p) {
  if (*p != '"')
    return NULL;
  p++;
  while (*p && *p != '"') {
    if (*p == '\\') {
      p++;
      if (!*p)
        return NULL;
    }
    p++;
  }
  return (*p == '"') ? p + 1 : NULL;
}

/* Read a JSON string literal at p (at opening ") into buf[bufsz].
 * Returns pointer past closing ", or NULL on error/overflow. */
static const char *wmReadStr(const char *p, char *buf, size_t bufsz) {
  if (*p != '"')
    return NULL;
  p++;
  size_t n = 0;
  while (*p && *p != '"' && *p != '\n') {
    char c;
    if (*p == '\\') {
      p++;
      if (!*p)
        return NULL;
      switch (*p) {
      case '"':  c = '"';  break;
      case '\\': c = '\\'; break;
      case '/':  c = '/';  break;
      case 'n':  c = '\n'; break;
      case 'r':  c = '\r'; break;
      case 't':  c = '\t'; break;
      default:   return NULL;
      }
    } else {
      c = *p;
    }
    if (n >= bufsz - 1)
      return NULL;
    buf[n++] = c;
    p++;
  }
  if (*p != '"')
    return NULL;
  buf[n] = '\0';
  return p + 1;
}

struct WMCmd {
  char cmd[32];
  unsigned long id;
  unsigned long wid;
  long x, y, w, h;
  bool has_id, has_wid, has_x, has_y, has_w, has_h;
};

/* Parse a flat JSON command object from line into out.
 * Returns true if cmd and id were successfully extracted.
 * Sets *err to a protocol error code string on failure. */
static bool wmParseCmd(const char *line, WMCmd *out, const char **err) {
  memset(out, 0, sizeof(*out));
  const char *p = wmSkipWs(line);
  if (*p != '{') {
    *err = "malformed";
    return false;
  }
  p++;

  for (;;) {
    p = wmSkipWs(p);
    if (*p == '}')
      break;
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p != '"') {
      *err = "malformed";
      return false;
    }

    char key[32];
    p = wmReadStr(p, key, sizeof(key));
    if (!p) {
      *err = "malformed";
      return false;
    }
    p = wmSkipWs(p);
    if (*p != ':') {
      *err = "malformed";
      return false;
    }
    p = wmSkipWs(p + 1);

    if (strcmp(key, "cmd") == 0) {
      p = wmReadStr(p, out->cmd, sizeof(out->cmd));
      if (!p) {
        *err = "malformed";
        return false;
      }
    } else if (strcmp(key, "id") == 0) {
      char *ep;
      unsigned long v = strtoul(p, &ep, 10);
      if (ep == p || v > 0xFFFFFFFFUL) {
        *err = "malformed";
        return false;
      }
      out->id = v;
      out->has_id = true;
      p = ep;
    } else if (strcmp(key, "wid") == 0) {
      char *ep;
      out->wid = strtoul(p, &ep, 10);
      if (ep == p) {
        *err = "malformed";
        return false;
      }
      out->has_wid = true;
      p = ep;
    } else if (strcmp(key, "x") == 0) {
      char *ep;
      out->x = strtol(p, &ep, 10);
      if (ep == p) {
        *err = "malformed";
        return false;
      }
      out->has_x = true;
      p = ep;
    } else if (strcmp(key, "y") == 0) {
      char *ep;
      out->y = strtol(p, &ep, 10);
      if (ep == p) {
        *err = "malformed";
        return false;
      }
      out->has_y = true;
      p = ep;
    } else if (strcmp(key, "w") == 0) {
      char *ep;
      out->w = strtol(p, &ep, 10);
      if (ep == p) {
        *err = "malformed";
        return false;
      }
      out->has_w = true;
      p = ep;
    } else if (strcmp(key, "h") == 0) {
      char *ep;
      out->h = strtol(p, &ep, 10);
      if (ep == p) {
        *err = "malformed";
        return false;
      }
      out->has_h = true;
      p = ep;
    } else {
      /* Unknown field: skip value (string or scalar). */
      if (*p == '"') {
        p = wmSkipStr(p);
        if (!p) {
          *err = "malformed";
          return false;
        }
      } else {
        while (*p && *p != ',' && *p != '}')
          p++;
      }
    }
  }

  if (!out->cmd[0] || !out->has_id) {
    *err = "malformed";
    return false;
  }
  *err = NULL;
  return true;
}

static void wmDispatchCmd(const char *line, size_t /*len*/) {
  WMCmd cmd;
  const char *err = NULL;

  if (!wmParseCmd(line, &cmd, &err)) {
    wmSendReply(cmd.has_id, cmd.id, false, err ? err : "malformed");
    return;
  }

  /* All current verbs take a wid; reject wid=0 (reserved). */
  if (!cmd.has_wid || cmd.wid == 0) {
    wmSendReply(true, cmd.id, false, "unknown_wid");
    return;
  }

  /* Look up the widget, then verify it is a window. */
  Twidget any = (Twidget)Id2Obj(Twidget_class_byte, (uldat)cmd.wid);
  if (!any) {
    wmSendReply(true, cmd.id, false, "unknown_wid");
    return;
  }
  if (!IS_WINDOW(any)) {
    wmSendReply(true, cmd.id, false, "not_a_window");
    return;
  }
  Twindow w = (Twindow)any;

  if (strcmp(cmd.cmd, "focus") == 0) {
    w->Focus();
    wmSendReply(true, cmd.id, true, NULL);

  } else if (strcmp(cmd.cmd, "move") == 0) {
    if (!cmd.has_x || !cmd.has_y) {
      wmSendReply(true, cmd.id, false, "malformed");
      return;
    }
    /* dat is int16; verify range before narrowing cast. */
    if (cmd.x < -32768 || cmd.x > 32767 || cmd.y < -32768 || cmd.y > 32767) {
      wmSendReply(true, cmd.id, false, "out_of_bounds");
      return;
    }
    if (!wmAllowOffscreen && w->Parent && IS_SCREEN(w->Parent)) {
      Tscreen screen = (Tscreen)w->Parent;
      if (cmd.x < 0) cmd.x = 0;
      if (cmd.y < 0) cmd.y = 0;
      if (cmd.x >= (long)screen->XWidth)  cmd.x = (long)screen->XWidth  - 1;
      if (cmd.y >= (long)screen->YWidth)  cmd.y = (long)screen->YWidth  - 1;
    }
    DragWindow(w, (dat)cmd.x - w->Left, (dat)cmd.y - w->Up);
    if (wmDebugReplies)
      wmSendJsonLine("{\"reply\":true,\"id\":%lu,\"ok\":true,\"x\":%ld,\"y\":%ld}\n",
                     cmd.id, (long)w->Left, (long)w->Up);
    else
      wmSendReply(true, cmd.id, true, NULL);

  } else if (strcmp(cmd.cmd, "resize") == 0) {
    if (!cmd.has_w || !cmd.has_h) {
      wmSendReply(true, cmd.id, false, "malformed");
      return;
    }
    if (cmd.w <= 0 || cmd.h <= 0 || cmd.w > 65535 || cmd.h > 65535) {
      wmSendReply(true, cmd.id, false, "out_of_bounds");
      return;
    }
    ResizeRelWindow(w, (dat)cmd.w - (dat)w->XWidth, (dat)cmd.h - (dat)w->YWidth);
    if (wmDebugReplies)
      wmSendJsonLine("{\"reply\":true,\"id\":%lu,\"ok\":true,\"w\":%ld,\"h\":%ld}\n",
                     cmd.id, (long)w->XWidth, (long)w->YWidth);
    else
      wmSendReply(true, cmd.id, true, NULL);

  } else if (strcmp(cmd.cmd, "raise") == 0) {
    RaiseWidget((Twidget)w, false);
    wmSendReply(true, cmd.id, true, NULL);

  } else if (strcmp(cmd.cmd, "lower") == 0) {
    LowerWidget((Twidget)w, false);
    wmSendReply(true, cmd.id, true, NULL);

  } else if (strcmp(cmd.cmd, "close") == 0) {
    /* Delete fires unmap+close events (§7 ordering) before we reply. */
    w->Delete();
    wmSendReply(true, cmd.id, true, NULL);

  } else {
    wmSendReply(true, cmd.id, false, "unknown_cmd");
  }
}

/* Connected-fd read handler. Accumulates bytes into wmInBuf, dispatches each
 * complete newline-terminated line to wmDispatchCmd. */
static void wmConnRead(int fd, uldat /*slot*/) {
  for (;;) {
    int space = (int)(sizeof(wmInBuf) - 1) - wmInBufLen;
    if (space <= 0) {
      /* Buffer full with no newline: line exceeds cap. */
      log(WARNING) << "twin: WM side-channel: inbound line too long, dropping\n";
      wmSendReply(false, 0, false, "malformed");
      wmInBufLen = 0;
      char discard[64];
      { ssize_t dr = read(fd, discard, sizeof(discard)); (void)dr; }
      return;
    }
    ssize_t n = read(fd, wmInBuf + wmInBufLen, (size_t)space);
    if (n > 0) {
      wmInBufLen += (int)n;
      int base = 0;
      while (base < wmInBufLen) {
        char *nl = (char *)memchr(wmInBuf + base, '\n', wmInBufLen - base);
        if (!nl)
          break;
        *nl = '\0';
        if (nl - (wmInBuf + base) > 0)
          wmDispatchCmd(wmInBuf + base, (size_t)(nl - (wmInBuf + base)));
        base = (int)(nl - wmInBuf) + 1;
      }
      if (base > 0 && base < wmInBufLen)
        memmove(wmInBuf, wmInBuf + base, wmInBufLen - base);
      wmInBufLen = (base < wmInBufLen) ? (wmInBufLen - base) : 0;
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

void WMSockSetFlags(bool allowOffscreen, bool debugReplies) {
  wmAllowOffscreen = allowOffscreen;
  wmDebugReplies = debugReplies;
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

void WMSockSendMove(Twindow w) {
  if (Ext(WM, WMConnFd) < 0 || !w)
    return;

  wmSendJsonLine("{\"type\":\"move\",\"wid\":%lu,\"x\":%ld,\"y\":%ld}\n",
                 (unsigned long)w->Id, (long)w->Left, (long)w->Up);
}

void WMSockSendResize(Twindow w) {
  if (Ext(WM, WMConnFd) < 0 || !w)
    return;

  wmSendJsonLine("{\"type\":\"resize\",\"wid\":%lu,\"w\":%ld,\"h\":%ld}\n",
                 (unsigned long)w->Id, (long)w->XWidth, (long)w->YWidth);
}

void WMSockSendRaise(Twidget w) {
  if (Ext(WM, WMConnFd) < 0 || !w)
    return;

  wmSendJsonLine("{\"type\":\"raise\",\"wid\":%lu}\n", (unsigned long)w->Id);
}

void WMSockSendLower(Twidget w) {
  if (Ext(WM, WMConnFd) < 0 || !w)
    return;

  wmSendJsonLine("{\"type\":\"lower\",\"wid\":%lu}\n", (unsigned long)w->Id);
}

void WMSockSendClose(Twidget w) {
  if (Ext(WM, WMConnFd) < 0 || !w)
    return;

  wmSendJsonLine("{\"type\":\"close\",\"wid\":%lu}\n", (unsigned long)w->Id);
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

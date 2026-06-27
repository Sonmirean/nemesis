/* Copyright (C) 2026 Nemesis project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef NEMESIS_WMSOCK_H
#define NEMESIS_WMSOCK_H

#include "obj/fwd.h"

/*
 * Nemesis WM side-channel socket.
 *
 * Independent of Twin's main libtw socket: the listen path is
 *   <TmpDir>/.Twin<TWDISPLAY>-wm
 * with mode 0600, FD_CLOEXEC, listen-backlog 1.
 *
 * One-at-a-time: a single external WM may attach via accept(); a second
 * connect attempt is rejected by the kernel because the listen backlog
 * is consumed and the accepted fd is held until the WM disconnects.
 *
 * Wire format: line-framed JSON, one event per line, no embedded newlines.
 */

bool WMSockInit(void);
void WMSockShutdown(void);

/* Set Phase 4D config flags. Must be called before the first client attaches.
 * allowOffscreen: if false (default), cmd:move clamps to screen bounds.
 * debugReplies:   if true, cmd:move and cmd:resize success replies include
 *                 post-clamp geometry (§6.2 of docs/wmsock-protocol.md). */
void WMSockSetFlags(bool allowOffscreen, bool debugReplies);

/* Send msg to the built-in WM and mirror it to the attached external WM
 * (no-op mirror if none). Phase 4A invariant: built-in WM stays
 * authoritative, external WM is observer-only -- the SendMsg fires first
 * and the mirror is best-effort. Dispatches mirror by msg->Type;
 * msg types without a wire mapping are forwarded to the built-in WM only. */
void SendMsgToWM(Tmsg msg);

/* Synthesized focus mirror. Focus changes go through direct method calls,
 * not messages, so Swidget::Focus() calls this directly. newW=NULL means
 * all windows unfocused; oldW=NULL means no prior focus was tracked. */
void WMSockSendFocus(Twidget newW, Twidget oldW);

/* Synthesized unmap mirror for the external WM. The internal protocol has
 * no msg_unmap, so Swidget::UnMap() calls this directly to keep the external
 * WM in lock-step with map mirror sites. */
void WMSockSendUnmap(Twidget w, Tscreen screen);

/* Synthesized move/resize/raise/lower/close mirrors. None of these have an
 * internal msg_* counterpart, so the relevant top-level paths in
 * server/resize.cpp and obj/{widget,window}.cpp call these directly.
 *
 *   move/resize  reflect post-update w->Left/Up and w->XWidth/YWidth.
 *   raise/lower  fire only when stacking actually changed.
 *   close        fires once per Swindow destruction, after the paired unmap.
 */
void WMSockSendMove(Twindow w);
void WMSockSendResize(Twindow w);
void WMSockSendRaise(Twidget w);
void WMSockSendLower(Twidget w);
void WMSockSendClose(Twidget w);

#endif /* NEMESIS_WMSOCK_H */

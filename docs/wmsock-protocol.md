# Nemesis WM side-channel protocol

**Status:** Phase 4D landed — inbound commands implemented and verified.
Outbound events (Phases 4A–4C) are landed and unchanged.
Phase 4E (twinrc integration for `wm_sidechannel_*` flags) and Phase 4F
(outbound write queue) are planned.

This document is the wire-level contract between `twin_server` and an
external window manager attached to the WM side-channel socket. If
you are writing an external WM, or modifying the server side of the
protocol, read this first.

## 1. Background

The WM side-channel is a second AF_UNIX socket exposed by
`twin_server` (path `<TmpDir>/.Twin<TWDISPLAY>-wm`, mode `0600`,
listen-backlog 1). The original socket carries the legacy libtw client
protocol; the side-channel carries this WM protocol. They are
independent.

Phases 4A–4C made the side-channel **observer-only**: the server
emits one JSON event per line whenever a window changes state, and
the external WM listens. Phase 4D makes the channel **bidirectional**:
the external WM can also send commands, and the server replies.

After 4D, the built-in WM and the external WM are **co-authoritative
peers**. Both can move/resize/focus/raise/lower/close windows; both
see every event; the single-threaded main loop serializes them, so
there is no race. There is no priority mechanism — either peer is
free to ignore events caused by the other.

## 2. Transport

- Same socket, same connection, full duplex.
- Same line-framed JSON: one object per line, terminated by `\n`,
  no embedded newlines in payloads, UTF-8.
- Maximum line length: `WMSOCK_READ_BUFSZ` bytes (a compile-time
  constant in `server/wmsock.cpp`). Inbound lines longer than that are
  rejected with `malformed` and the line buffer is reset.

## 3. Message kinds

The outbound stream (server → external WM) now carries two kinds of
object. Clients dispatch on the top-level discriminator:

| Kind   | Discriminator     | Origin              | When                                    |
|--------|-------------------|---------------------|-----------------------------------------|
| event  | `"type":"<verb>"` | server-initiated    | Whenever window state changes.          |
| reply  | `"reply":true`    | response to inbound | Exactly one per inbound command line.   |

The inbound stream (external WM → server) carries one kind of object:

| Kind    | Discriminator    | When                                  |
|---------|------------------|---------------------------------------|
| command | `"cmd":"<verb>"` | Whenever the external WM wants action. |

## 4. Outbound events

Unchanged from Phases 4A–4C. Reference table:

| `type`   | Fields                                             | Fires when                          |
|----------|----------------------------------------------------|-------------------------------------|
| `map`    | `wid`, `screen`, `x`, `y`, `w`, `h`                | Window mapped to a screen.          |
| `unmap`  | `wid`, `screen`                                    | Window hidden or destroyed.         |
| `mouse`  | `code`, `shift`, `x`, `y`                          | Mouse input dispatched to WM.       |
| `key`    | `code`, `shift`, `seq` (hex)                       | Key input dispatched to WM.         |
| `focus`  | `wid`, `old_wid`                                   | Focused window actually changed.    |
| `move`   | `wid`, `x`, `y`                                    | After geometry update.              |
| `resize` | `wid`, `w`, `h`                                    | After geometry update.              |
| `raise`  | `wid`                                              | When stacking actually changed.     |
| `lower`  | `wid`                                              | When stacking actually changed.     |
| `close`  | `wid`                                              | After paired `unmap` on destroy.    |

All coordinates are signed cell offsets relative to the parent
screen. All dimensions are unsigned cell counts.

## 5. Inbound commands (Phase 4D)

Every command line has the shape:

```
{"cmd": "<verb>", "id": <uint32>, ...args}
```

- `cmd` (string, required) — one of the verbs in §5.2.
- `id` (uint32, required) — client-supplied correlation id, echoed
  back in the reply. The server treats it as opaque. Clients should
  pick monotonic ids per connection so they can detect lost replies,
  but the server does not enforce.

### 5.1 Field types

| Field   | JSON type           | C type   | Notes                                                   |
|---------|---------------------|----------|---------------------------------------------------------|
| `id`    | integer, 0 … 2³²–1  | `uint32_t` | Correlation id.                                       |
| `wid`   | integer, 1 … 2⁶⁴–1  | `uldat`    | Window id. Must refer to an existing window; `wid:0` is reserved (rejected). |
| `x`,`y` | integer, –32768 … 32767 | `dat`  | Absolute cell coords relative to parent screen.       |
| `w`,`h` | integer, 1 … 65535  | `udat`     | Absolute cell dimensions, must be > 0.                |

### 5.2 Verbs

```jsonc
{"cmd":"focus",  "id":N, "wid":W}
{"cmd":"move",   "id":N, "wid":W, "x":X, "y":Y}       // absolute coords
{"cmd":"resize", "id":N, "wid":W, "w":Wd, "h":H}      // absolute dimensions
{"cmd":"raise",  "id":N, "wid":W}
{"cmd":"lower",  "id":N, "wid":W}
{"cmd":"close",  "id":N, "wid":W}
```

### 5.3 Semantics

**`focus`** — make `wid` the focused window.
- `wid` must refer to an existing, mapped window. There is no
  active-unfocus command in 4D — external WMs that want to clear
  focus should focus a different window instead.
- Maps internally to the focus path used by `Swidget::Focus`.
- The outbound `focus` event fires if and only if the focused window
  actually changed.

**`move`** — move `wid` to absolute screen coordinates `(x, y)`.
- Wmsock computes `DeltaX = x - w->Left`, `DeltaY = y - w->Up` and
  calls `DragWindow(w, DeltaX, DeltaY)`. The translation makes the
  external WM stateless: every command names the target, never a
  delta relative to a possibly-stale local model.
- **Offscreen policy is configurable** (see §10). By default,
  `(x, y)` is clamped server-side so that the window's top-left
  remains within the parent screen's rectangle. The reply stays
  `ok:true`; the outbound `move` event carries the actual post-clamp
  position. When `wm_sidechannel_allow_offscreen = true`, no clamp
  is applied (matches the built-in WM's behavior — windows may move
  entirely offscreen).
- The outbound `move` event fires after the geometry update.

**`resize`** — resize `wid` to absolute dimensions `(w, h)`.
- Wmsock computes the delta against `w->XWidth`/`w->YWidth` and calls
  `ResizeRelWindow`.
- Every window carries a strict minimum size (`MinXWidth`,
  `MinYWidth` — same model as Win32 `WM_GETMINMAXINFO`). Requests
  below the minimum are clamped server-side to the minimum. The
  reply is still `ok:true`; the outbound `resize` event reports the
  actual post-clamp dimensions. **The authoritative geometry source
  is the outbound event, not the command.**
- Fires `resize` event on success.

**`raise`** — raise `wid` to the top of its screen's stack.
- Maps to `RaiseWidget`.
- Fires `raise` event if stacking actually changed (no-op if `wid`
  was already on top).

**`lower`** — lower `wid` to the bottom of its screen's stack.
- Maps to `LowerWidget`.
- Fires `lower` event if stacking actually changed.

**`close`** — destroy `wid` and its children.
- Maps to `Swidget::Delete`.
- Fires outbound `unmap` then `close` events before the reply (see
  §7 ordering).

## 6. Replies

Every well-formed inbound line — successful or not — produces exactly
one reply line.

Success (default):

```jsonc
{"reply":true, "id":N, "ok":true}
```

Failure:

```jsonc
{"reply":true, "id":N, "ok":false, "error":"<code>"}
```

Failures may carry an optional `"detail":"<text>"` field for human
log output. Clients **must not parse `detail`** — it is free-form.

### 6.1 Error codes

| Code             | Meaning                                                              |
|------------------|----------------------------------------------------------------------|
| `unknown_cmd`    | `cmd` is not a recognized verb.                                      |
| `malformed`      | JSON parse failed, required field missing, wrong type, or line too long. |
| `unknown_wid`    | `wid` does not refer to any existing object.                         |
| `not_a_window`   | `wid` exists but is not a `Twindow` (e.g. sub-widget).               |
| `out_of_bounds`  | A numeric arg is outside the representable range for its C type.     |
| `internal`       | Server-side failure unrelated to the command (reserved).             |

### 6.2 Debug-mode geometry echo

When `wm_sidechannel_debug = true` in twinrc (see §10), the server
echoes the post-command geometry on `move` and `resize` success
replies (it is otherwise a strict superset of §6 default shape):

```jsonc
{"reply":true, "id":N, "ok":true, "x":X, "y":Y}              // move
{"reply":true, "id":N, "ok":true, "w":Wd, "h":H}             // resize
```

The values are the actual post-clamp coordinates/dimensions —
identical to what the paired outbound `move`/`resize` event carries.
Useful when developing or debugging an external WM; default is off
because the outbound event already carries the same data.

Other verbs (`focus`, `raise`, `lower`, `close`) have no debug-mode
extension — their effect is fully captured by the existing outbound
events.

### 6.3 Replies when `id` is unparseable

If a malformed line cannot supply an `id` (e.g. invalid JSON, missing
`id` field), the reply omits `id` entirely:

```jsonc
{"reply":true, "ok":false, "error":"malformed"}
```

Clients should treat replies without `id` as protocol-level errors,
not correlated to a specific command.

## 7. Ordering

Per connection, the server processes commands strictly in arrival
order on the main loop. Replies follow the same order — clients can
correlate by `id` or rely on FIFO.

**Side effects precede the reply.** For each command, all outbound
events that the command directly causes are written before the
reply. So the externally observable sequence for `move` is:

```
WM → server : {"cmd":"move","id":42,"wid":7,"x":40,"y":10}
server → WM : {"type":"move","wid":7,"x":40,"y":10}
server → WM : {"reply":true,"id":42,"ok":true}
```

And for `close`:

```
WM → server : {"cmd":"close","id":99,"wid":7}
server → WM : {"type":"unmap","wid":7,"screen":0}
server → WM : {"type":"close","wid":7}
server → WM : {"reply":true,"id":99,"ok":true}
```

Events caused by **other** activity (user input, internal WM
decisions, unrelated windows) may interleave between a command's
direct effects and its reply. Clients must not assume "the next
message after a command is the reply" — match by `id`.

## 8. Authority model after 4D

- The built-in WM remains running and reacts to user input the same
  as before.
- The external WM can issue commands at any time; they dispatch
  through the same internal entrypoints the built-in WM uses.
- Both peers see every event (including events caused by the other).
- Twin's main loop is single-threaded — there is no concurrency
  race. If both peers move the same window in the same tick, the
  later-processed action takes effect last.
- There is no priority mechanism and no veto. Either peer is free to
  ignore the other.

## 9. Backpressure (deferred to Phase 4F)

In Phase 4D there is **no separate command queue**. On a readable-fd
event the server drains the kernel socket buffer into a per-connection
line buffer (size `WMSOCK_READ_BUFSZ`), parses every complete line in
it, and dispatches each command sequentially in the same main-loop
tick before yielding back to the loop. Anything that does not fit
backpressures naturally via the kernel — the external WM's `write()`
begins to return `EAGAIN` (or block, if the WM is using a blocking
socket).

The outbound write path is best-effort: an `EAGAIN` on the write
drops the message (event or reply). A slow client can lose replies.

Phase 4F replaces the outbound side with a real write queue
(`RemoteWriteQueue` / `RemoteFlush`). After 4F, replies and events are
delivered reliably as long as the connection stays open.

Until 4F lands, clients should treat ack timeouts as a soft signal,
not a hard error: log, retry only if necessary, and rely on event
correlation for ground truth.

## 10. Configuration

Phase 4D introduces two twinrc flags that gate command semantics.
Both default to the conservative setting so that an unconfigured
server behaves predictably for any external WM author.

| Flag                              | Type | Default | Effect                                                         |
|-----------------------------------|------|---------|----------------------------------------------------------------|
| `wm_sidechannel_allow_offscreen`  | bool | `false` | When `false`, `cmd:move` clamps target `(x, y)` to keep the window's top-left inside the parent screen. When `true`, the server passes the request through unmodified — windows can move entirely offscreen, matching built-in WM behavior. |
| `wm_sidechannel_debug`            | bool | `false` | When `true`, success replies for `cmd:move` and `cmd:resize` include the post-clamp geometry (see §6.2). External WM debug tools opt in; production stays minimal-latency. |

Both flags are read once at server start (no live reload in 4D). The
exact twinrc syntax follows the conventions in the existing
`twinrc` example file — to be finalized during implementation.

## 11. Out of scope

Anticipated future work that is **not** part of 4D:

- **Input injection** (`cmd:key`, `cmd:mouse`) — would let the
  external WM synthesize input. Larger trust question, deferred.
- **Query verbs** (`cmd:list`, `cmd:get`) — would let a
  late-attaching WM bootstrap state without waiting for events.
- **Transactional commands** — atomic stacking swaps, batch moves.
- **Multi-connection authority** — today the listen backlog is 1.
  Multi-peer arbitration would need a priority/veto model.
- **Active unfocus** (`focus wid:0` or `cmd:unfocus`) — no
  use case in 4D; external WMs that need to clear focus should focus
  a different window. Add later if a real use case appears.
- **Live config reload** — `wm_sidechannel_*` flags are read once at
  server start.

## 12. Decisions log

Closes the open questions raised during the spec review. Recorded so
future readers can see the rationale, not just the final shape.

1. **No active-unfocus command.** `focus` requires a valid existing
   `wid`. No use case in 4D for actively clearing focus from outside;
   refocusing elsewhere covers the realistic scenarios. `wid:0` is
   reserved (rejected with `unknown_wid`) so we can repurpose it
   later without a wire break.
2. **Strict per-window minimum size, silent clamp.** Mirrors Win32
   `WM_GETMINMAXINFO`. Twin already has `MinXWidth` / `MinYWidth`;
   `cmd:resize` below those clamps to the minimum. Reply is
   `ok:true`; the outbound `resize` event carries the actual
   post-clamp dimensions. The event is authoritative for resulting
   geometry, not the command.
3. **Offscreen `move` forbidden by default, configurable.** Added
   twinrc flag `wm_sidechannel_allow_offscreen` (default `false`).
   Default behavior clamps the requested coords to keep the window's
   top-left inside the screen rect; opt-in unlocks built-in-WM-style
   offscreen freedom.
4. **Bare-`ok:true` reply by default, debug echo opt-in.** Added
   twinrc flag `wm_sidechannel_debug` (default `false`). Production
   keeps minimum latency; debug builds get geometry echoed on
   `move`/`resize` replies (§6.2) for easier troubleshooting.
5. **No explicit in-flight command cap.** The kernel socket buffer
   plus `WMSOCK_READ_BUFSZ` line buffer provide the natural bound;
   excess backpressures the external WM's `write()` side. No
   denial-of-service surface beyond what the authenticated socket
   already implies.

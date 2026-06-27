# Nemesis (fork of Twin) — project rules

Read this before editing. Lessons here came out of real audits and the
nemesis-specific work that's already landed.

## What this is

Nemesis is a fork of [twin](https://github.com/cosmos72/twin) — a textmode
windowing environment in C++. The fork's active workstream is the **WM
side-channel socket** (`server/wmsock.{cpp,h}`): a parallel `AF_UNIX`
listener that mirrors window events as line-framed JSON so an external
window manager can observe the built-in WM. See `README.md` for the
project overview and the in-tree run recipe.

Roadmap shorthand used in source comments:
- **Phase 4A — observer-only (landed)**: built-in WM authoritative,
  external WM receives `map` events only.
- **Phase 4B — event coverage (landed)**: `unmap`, `mouse`, `key` events
  mirrored. `mouse`/`key` ride the `SendMsgToWM(Tmsg msg)` dispatcher;
  `unmap` is synthesized from `Swidget::UnMap()` (no `msg_unmap` exists).
- **Phase 4C — window-state coverage (landed)**: `focus`, `move`,
  `resize`, `raise`, `lower`, `close` events. All synthesized — none of
  these go through `Tmsg` either, so the `Swidget`/`Swindow` /
  `RaiseWidget`/`LowerWidget`/`DragWindow`/`ResizeRelWindow` paths call
  `WMSockSend<Type>` directly.
- **Phase 4D — inbound commands (landed)**: external WM sends
  line-framed JSON commands (`focus`, `move`, `resize`, `raise`, `lower`,
  `close`) on the same socket; server dispatches to the same internal
  entrypoints as the built-in WM and replies with `{"reply":true,...}`.
  Hand-rolled zero-alloc JSON parser in `wmsock.cpp`. Config flags
  (`wmAllowOffscreen`, `wmDebugReplies`) are file-static bools set via
  `WMSockSetFlags()`; twinrc integration deferred to 4E.
- **Phase 4E — twinrc integration (landed)**: `WmSidechannelAllowOffscreen`
  and `WmSidechannelDebug` GlobalFlags keywords wired through the
  flex/bison parser (`rcparse.l`, `rcparse.y`, `rcparse.h`). State is
  shipped from the parse-child to the parent via a parallel
  `GlobalWMSidechannelFlags[2]` OR/XOR array in the shm pool and applied
  in `ReadGlobals()` via `WMSockSetFlags()`. The existing 8-bit
  `Ssetup::Flags` field was already 7/8 bits used, so adding a new
  shipped array isolates wmsock policy from core setup flags. Both
  default off; example `twinrc` documents them.
- **Phase 4F — outbound write queue (landed)**: `wmConnWriteLine` now
  routes every JSON line through `RemoteWriteQueue` + `RemoteFlush`
  instead of issuing a raw `write()`. A slow or paused external WM no
  longer drops events: bytes sit in the per-slot queue, `RemoteFlushAll`
  in the main loop drains them, and `select(2)` registers the fd as
  writable via `save_wfds`. Tear-down on the read path still calls
  `wmCloseConn` → `UnRegisterRemote`, which frees the queue. Accepts a
  brief `EPIPE` busy-loop window between peer close and next select cycle.

Nemesis-specific commits are prefixed `nemesis:`. Upstream commits land
on top via merge; do not rewrite upstream history.

## Build, run, verify

- `./configure && make -j`. Build must complete with no new warnings;
  the codebase is `.clang-format`-clean — respect it.
- In-tree run (no install): use the staging recipe in `README.md`
  (`/tmp/twin-plugins` symlink farm + `--plugindir=`).
- Verify the side-channel after a code change to `wmsock` or `util`:
  - `ls -l /tmp/.Twin:0-wm` while server is up → `srw-------` (0600).
  - Drive the socket: `socat - UNIX-CONNECT:/tmp/.Twin:0-wm` if available,
    or `python3 -c 'import socket;s=socket.socket(socket.AF_UNIX,
    socket.SOCK_STREAM);s.connect("/tmp/.Twin:0-wm");print(s.recv(4096).decode())'`,
    then open any small twin client (e.g. `./clients/twcuckoo`); a JSON
    line `{"type":"map",…}` must appear.
  - **Do not smoke-test with `twclutter`.** It is an unbounded stress
    test ("Useless client that spams twin with windows") — on X11 it can
    create hundreds of thousands of windows in seconds and overwhelm the
    backend. Use it only when you specifically want load.
  - For coverage beyond `map`, build a small libtw exerciser pattern
    after `clients/clutter.c` (needs a real `tmenu` from `TwCreateMenu`,
    not NULL) and exercise `TwSetXYWindow`/`TwResizeWindow`/`TwRaise/Lower
    Window`/`TwDeleteWindow` to drive the synthesized mirrors.
  - `kill <pid>` the server cleanly; `ls /tmp/.Twin:0-wm` must report
    no such file. That proves `WMSockShutdown` ran via `QuitTWDisplay`.

## Architecture invariants — do not silently break these

1. **Built-in WM stays authoritative.** The external WM is observer-only
   in phase 4A. Mirror calls must come *after* `SendMsg(Ext(WM, MsgPort), msg)`,
   never before, never instead of.
2. **The mirror is best-effort.** A slow or absent external WM must
   never block the server. Phase 4F routes outbound JSON through
   `RemoteWriteQueue`/`RemoteFlush`, so transient slowness is absorbed by
   the per-slot queue; the queue is bounded by the same backpressure
   logic that applies to the libtw socket. The mirror still never blocks
   the WM event loop.
3. **`InitWM` loads `SocketSo` unconditionally** (`server/wm.cpp`).
   This is a nemesis policy change vs upstream so external clients
   (`twclutter`, `twterm`, `twattach`) can attach regardless of `--hw=…`.
   Don't gate it back on `--nohw`.
4. **State location for the WM side-channel:** lifecycle bookkeeping
   (`wmListenSlot`, `wmConnSlot`, `wmAddr`, `wmPathBound`) is file-static
   in `wmsock.cpp`; the public fds (`WMListenFd`, `WMConnFd`) live in
   `Ext(WM, …)` (`extreg.h`). This split is intentional — keep it.

## Signal model

`InitSignals` (`server/hw.cpp:205-217`) installs `SIG_IGN` for every
signal in `signals_ignore[]`, **including `SIGPIPE`** (`hw.cpp:155`).
That handler stays in place for the life of the process.

- **Do not save/restore `SIGPIPE` per write call.** It's redundant and
  fragile. `write()` already returns `-1`/`EPIPE` because the global
  handler is `SIG_IGN`.
- Same rule for any new write path you add — trust the global handler;
  inspect `errno` on the return value.

## Unix-socket bootstrap pattern

Canonical sequence (see `util.cpp:1191-1296` for the libtw listener,
`wmsock.cpp` for the WM side-channel):

```
socket(AF_UNIX, SOCK_STREAM, 0)
  → build path with CopyToSockaddrUn (chain returns running pos)
  → CHECK: pos < sizeof(sun_path) - 1   ← path is silently truncated otherwise
  → probe connect; on failure unlink; on success refuse to bind
  → bind
  → chmod (0600 for WM side-channel; 0700 for libtw)
  → listen (backlog = WMSOCK_LISTEN_BACKLOG for WM; 3 for libtw)
  → fcntl FD_CLOEXEC (and O_NONBLOCK on the WM listen fd)
  → RegisterRemoteFd
```

**Every listener path bound during init must be `unlink`ed in
`QuitTWDisplay`** (or chained from it). `QuitTWDisplay` already calls
`WMSockShutdown()`; if you add another listener, add it the same way.

## Auth & trust

The WM side-channel has **no TwinAuth**. The trust model:

- Socket file mode `0600` + kernel UID check on `connect(2)` = only the
  server's own uid can attach. `TmpDir` is typically `/tmp` (mode 1777,
  world-writable but sticky); directory mode is *not* the gate.
- Same trust model as Twin's main libtw socket. Document the mode (0600)
  and the UID check, not the parent directory.

The `~/.TwinAuth` file is for the libtw socket only. Its creation is
TOCTOU-safe via `O_EXCL` + `fchmod(fd, …)` (`server/socket.cpp:1950+`,
upstream PR #86). Don't regress that to `chmod(path, …)`.

## Wire format

Line-framed JSON, one event per line, no embedded newlines. Buffer
sizes use named constants at the top of `wmsock.cpp`
(`WMSOCK_JSON_LINE_BUFSZ`, `WMSOCK_READ_BUFSZ`). If a message would
exceed the buffer, the truncation case **must log a warning** — silent
drops are debuggable only post-mortem.

## Event-mirror sites

Mirrors come in two flavors:

**Dispatcher route** — for events that already flow through `Tmsg`. The
relevant `SendMsg(Ext(WM, MsgPort), msg)` callsite is replaced with
`SendMsgToWM(msg)`, which calls `SendMsg` first (invariant 1) and then
dispatches by `msg->Type`. Today: `msg_map`, `msg_mouse`, `msg_key`. Add a
new mirrored msg type by extending the switch in `SendMsgToWM` and adding
a file-static `wmSend<Type>` formatter.

**Synthesized route** — for state changes that have no `Tmsg`
counterpart. The state-changing function calls a public `WMSockSend<Type>`
helper directly (also defined in `wmsock.cpp`, sitting beside the dispatcher
helpers). Today:

| Event   | Helper                | Call site                                              |
|---------|-----------------------|--------------------------------------------------------|
| unmap   | `WMSockSendUnmap`     | `Swidget::UnMap` (paired with the map mirror)          |
| focus   | `WMSockSendFocus`     | `Swidget::Focus` (called only when focus actually shifted) |
| move    | `WMSockSendMove`      | `DragFirstWindow`, `DragWindow`, `Swindow::SetXY`      |
| resize  | `WMSockSendResize`    | `ResizeRelFirstWindow`, `ResizeRelWindow` (inside `if (DeltaX||DeltaY)`) |
| raise   | `WMSockSendRaise`     | `RaiseWidget` (inside `if (screen->Widgets.First != w)`) |
| lower   | `WMSockSendLower`     | `LowerWidget` (inside `if (screen->Widgets.Last != w)`) |
| close   | `WMSockSendClose`     | `Swidget::Delete` (after children UnMap, `IS_WINDOW` only) |

Pairing/ordering rules:
- **map ↔ unmap** are paired in `obj/widget.cpp` — keep them in lock-step.
- **close** fires after the corresponding **unmap** when a mapped window is
  destroyed. An unmap without a close means "hidden temporarily"; a close
  means "wid is gone, drop your bookkeeping."
- **raise/lower** fire only when stacking actually changed (the helpers are
  inside the position-change `if`).
- **move/resize** fire after the geometry update, so they reflect the
  post-change `w->Left/Up` and `w->XWidth/YWidth`.

The mouse merge-coalesce path in `StdAddMouseEvent`
(`server/hw_multi.cpp:1122-1128`) mutates the tail-queued msg without
calling `SendMsg`, so it does not reach `SendMsgToWM` and does not
mirror. Effect: when MOVE events arrive faster than the built-in WM
drains its queue, only the first MOVE in each burst mirrors to the
external WM; intermediate positions are folded into the queued msg and
visible only to the built-in WM. Acceptable for observer semantics;
revisit if smooth external tracking is required.

## Code style — what Twin does

Adapt to the existing house style, don't impose new conventions:

- **Functions**: `PascalCase` for exported (`WMSockInit`, `InitTWDisplay`),
  `camelCase` for file-static helpers (`wmCloseConn`, `wmConnRead`).
- **Logging**:
  `log(WARNING) << "twin: <subsystem>: " << ... << Chars::from_c(strerror(errno)) << "\n";`
- **Types**: use Twin's typedefs (`uldat`, `udat`, `dat`, `tcell`,
  `tcolor`, `byte`, `bool`), not `int32_t`/`stdint`. They have
  cross-platform semantics Twin depends on.
- **Resource cleanup**: `goto cleanup;` is idiomatic in
  `server/socket.cpp` (e.g. `CreateAuth`). Don't refactor it away to
  `try/finally`-style RAII just because it's C-flavored.
- **Magic numbers**: prefer a `#define` at the top of the file. Refer
  to `wmsock.cpp` for the layout (`WMSOCK_READ_BUFSZ` etc.).
- **Phase markers**: forward-looking `Phase 4F` comments at use sites
  are fine if they're short and concrete. Avoid roadmap blocks in
  source — those belong in `../decisions/` or issue trackers.

## Caveats observed in the codebase

These are real surprises a future editor will hit. Pre-empt them:

1. **`CopyToSockaddrUn` silently truncates** when `pos >= sun_path - 1`
   (`util.cpp:1171-1180`). Any chain of calls must check the final pos.
2. **Chained `chmod && listen && fcntl`** clobbers `errno` between
   calls (`util.cpp:1243-1244`, `wmsock.cpp:175-178`). The current code
   logs a generic "chmod/listen/fcntl" message. When you add new
   failure paths, split the chain for diagnostic clarity.
3. **`extreg.h` is a function-pointer swap registry**, but the WM
   fields (`WMListenFd`, `WMConnFd`) are raw `int`. That's fine for
   module-local fds; don't extend the pattern to other modules without
   thinking through whether you actually need swap-in semantics.
4. **Stale-socket cleanup logic in `wmsock.cpp` treats all `connect()`
   errors as stale.** It's correct for `ECONNREFUSED` (no listener) but
   would unlink a path even on `EACCES`. Acceptable today (single-user
   tree); revisit if multi-user trust ever matters.

## When in doubt

- For library/API questions, prefer `context7` MCP over web search.
- For irreversible changes (force-push, history rewrite, deletion of
  files outside the build tree), confirm with the user first.
- Verify changes end-to-end: build, in-tree run, probe the side-channel
  with `socat`. Don't claim "done" on type-check alone.

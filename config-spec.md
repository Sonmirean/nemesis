# nemesis — Configuration Specification

## Overview

Two config files, TOML format:

- `desktop.config` — desktop bars, global keybindings, background
- `windows.config` — window borders, per-window-class overrides

**Fallback policy:** each top-level section loads independently. A parse error or validation failure in one section falls back that section to hardcoded defaults without affecting other sections.

---

## Position Notation

Used uniformly in all bars and window borders:

| Notation | Meaning |
|---|---|
| `A`, `B`, `C`, … | Counted from the left (or top for vertical bars) |
| `Z`, `X`, `Y`, … | Counted from the right (or bottom for vertical bars) |
| `0`, `1`, `2`, … | Centered group, counted outward |
| `TL` | Top-left corner |
| `TR` | Top-right corner |
| `BL` | Bottom-left corner |
| `BR` | Bottom-right corner |

Corner positions (`TL`, `TR`, `BL`, `BR`) are single-cell slots shared between adjacent bars. Positional slots fill the remaining space.

**Collision rule:** if two buttons are assigned the same `pos`, the config section fails validation and that bar falls back to its hardcoded default.

---

## desktop.config

### `[topline]`

The top bar (1 row tall by default).

```toml
[topline]
height = 1        # rows
bg = 2            # ANSI color number
fg = 7
fill = "="        # character for empty space (single char)
```

Buttons:

```toml
[[topline.button]]
pos = "TL"
text = "╔"

[[topline.button]]
pos = "TR"
text = "╗"

[[topline.button]]
pos = "A"
text = "[Browser]"
exec = "/bin/qutebrowser"
bg = 4            # optional per-button background override
```

### `[bottomline]`

The bottom bar. Same fields as `[topline]`.

```toml
[bottomline]
height = 1
bg = 0
fg = 3
fill = " "

[[bottomline.button]]
pos = "Z"
text = "[12:34]"
exec = "date +%H:%M"
```

### `[leftline]`

Left vertical bar.

```toml
[leftline]
width = 3         # columns
bg = 0
fg = 7
fill = " "
```

For vertical bars, `pos` notation counts top-to-bottom (`A` = topmost, `Z` = bottommost). If a button's `text` is wider than `width`, it is truncated to fit — no wrapping, no rotation.

```toml
[[leftline.button]]
pos = "A"
text = "[WS1]"    # truncated to `width` chars if too long
bg = 1
```

### `[rightline]`

Right vertical bar. Same fields as `[leftline]`.

---

## windows.config

Defines how window borders look. Uses the same position notation — buttons appear on the border cells of each window.

```toml
[border]
bg = 8
fg = 7
fill = "─"        # horizontal fill character for top/bottom border

[[border.button]]
pos = "TL"
text = "┌"

[[border.button]]
pos = "TR"
text = "┐"

[[border.button]]
pos = "BL"
text = "└"

[[border.button]]
pos = "BR"
text = "┘"

[[border.button]]
pos = "A"
text = "[$title]"   # magic token — substituted with window title at runtime
bg = 0

[[border.button]]
pos = "Z"
text = "[X]"
action = "close"    # built-in WM action (see Actions below)
```

### Magic tokens in `text`

Token syntax: `${token}`. Tokens are evaluated on every draw.
**TODO:** profile draw frequency and add a dirty/interval cache for tokens that are expensive to query (cwd, pid) — evaluate on every draw is correct for now.

#### Desktop bar tokens

| Token | Substituted with |
|---|---|
| `${time}` | Current time (default format `HH:MM`; configurable: `${time:%H:%M:%S}`) |
| `${date}` | Current date (default format `YYYY-MM-DD`) |
| `${hostname}` | Machine hostname |
| `${user}` | Current username |
| `${directory}` | Current working directory of the WM process |
| `${workspace}` | Current workspace name or number |

#### Window border tokens

| Token | Substituted with |
|---|---|
| `${title}` | Window title (from OSC 0/2; falls back to process name) |
| `${cmd}` | Process name without arguments (e.g. `vim`, `bash`) |
| `${pid}` | PID of the child process |
| `${cwd}` | Current working directory of the child process |
| `${index}` | Window number within current workspace |
| `${bel}` | Non-empty indicator string when window has unread bell activity; empty string otherwise (disappears when no activity) |

### Button fields

| Field | Required | Description |
|---|---|---|
| `pos` | yes | Position slot (see notation above) |
| `text` | yes | Display text; supports magic tokens |
| `exec` | no | Shell command to run on click |
| `action` | no | Built-in WM action to invoke on click |
| `bg` | no | Per-button background color (ANSI), overrides bar/border `bg` |
| `fg` | no | Per-button foreground color (ANSI), overrides bar/border `fg` |

`exec` and `action` are mutually exclusive. If both are present, the section fails validation and that bar/border falls back to its hardcoded default.

### Built-in actions (WM commands for `action = "..."`)

TBD — to be defined in Phase 3 synthesis. Candidates: `close`, `minimize`, `maximize`, `focus-next`, `focus-prev`, `split-h`, `split-v`, `workspace-N`.

---

---

## Desktops (Workspaces)

### Behaviour

- Up to 9 desktops, numbered 1–9.
- Desktop 1 is always present and cannot be closed.
- On startup only desktop 1 exists; its button `[D1]` appears in the topline.
- `Super+N` (N = 1–9) switches to desktop N. If desktop N does not exist yet, it is created and its button `[DN]` is added to the topline.
- When switching away from a desktop: if it has no open windows, its button is removed from the topline. If it has open windows, the button is preserved.
- Desktop buttons are clickable (equivalent to the corresponding `Super+N` binding).

### Topline integration

Desktop buttons are declared as a range button in `[[topline.button]]`. The position range `"A:I"` reserves 9 slots (one per possible desktop). `${index}` is substituted with the desktop number (1–9) per entry. Per-state styling uses `label-<state>` / `fg-<state>` fields. Click runs `exec` with `${index}` substituted.

```toml
[[topline.button]]
pos = "A:I"
show = "nemctl desktop ${index} exists"
label-active = "[D${index}]"
label-inactive = "[D${index}]"
fg-active = 93
fg-inactive = 96
exec = "nemctl desktop switch ${index}"
```

**`show`** takes a shell command. Exit code 0 = show this entry, non-zero = hide. `${index}` is substituted before execution. Any command may be used, not only `nemctl`.

**`label-active` / `fg-active`** apply to the entry whose index matches the currently focused desktop (known natively by the WM — no separate condition needed).

`nemctl desktop ${index} exists` returns true for D1 always; for D2–D9 only if the desktop has been created and has at least one open window. This collapses the "D1 is always visible" special case into the command rather than into config syntax.

Fallback defaults (hardcoded): `pos = "A:I"`, `show = "nemctl desktop ${index} exists"`, `label-active = "[D${index}]"`, `label-inactive = "[D${index}]"`, `fg-active = 93`, `fg-inactive = 96`, `exec = "nemctl desktop switch ${index}"`.

---

## Open questions

- `exec` on a button: does it run once on click, or toggle? Does it capture output to the button's `text`?
- Notification model for `[bottomline]`: can child apps push text to a designated slot, or is it static/exec-driven only?
- Per-window-class overrides in `windows.config`: e.g., dialogs get a different border style than terminals.
- Keybindings section: format TBD — `[keybindings]` with `action = "key-chord"` pairs?

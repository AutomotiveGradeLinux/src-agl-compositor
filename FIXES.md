//Assisted-by: opencode:Claude Sonnet 4.6//

# Security and Code Quality Fixes

This document records all coding flaws and security issues identified and fixed
in the agl-compositor codebase. Fixes span 7 files across `src/`, `clients/`,
`grpc-proxy/`, and `shared/`.

---

## Critical

### C-1 — NULL dereference before NULL check (`src/layout.c`)

**Function:** `ivi_layout_activate_by_surf()`

`surf->dsurface` was dereferenced and passed to
`weston_desktop_surface_get_app_id()` before the `if (!surf) return` guard,
causing an immediate NULL dereference crash when `surf` is NULL.

**Fix:** Moved the `if (!surf) return` guard to the top of the function, before
any dereference of `surf`.

---

### C-2 — Broken NULL guard logic causing NULL dereference (`src/shell.c`)

**Functions:** `remove_black_curtain()`, `insert_black_curtain()`

Both functions used `&&`-chained NULL guards of the form:
```c
if ((!output &&
    !output->fullscreen_view.fs &&   // NULL deref if output is NULL
    !output->fullscreen_view.fs->view) || ...)
```
If `output` is NULL the second operand of `&&` is still evaluated, immediately
dereferencing NULL. The error-path log also used `output->name` while `output`
could be NULL.

**Fix:** Replaced the broken compound condition with a straightforward
`!output || !output->fullscreen_view.fs || !output->fullscreen_view.fs->view`
chain that short-circuits safely.

---

### C-3 — Short-circuit evaluation bug causing NULL dereference (`src/desktop.c`)

**Function:** `desktop_surface_removed()`

The original guard:
```c
if ((output == NULL && (surface->role == ... || ...)) ||
     output->output == NULL)
```
When `output` is NULL but the surface role is neither `NONE` nor `DESKTOP`,
the first operand of `||` evaluates to `false`, causing the second operand
`output->output == NULL` to be evaluated — dereferencing the NULL `output`.
A Wayland client creating a surface with roles such as `REMOTE`, `POPUP`, or
`FULLSCREEN` and destroying it before output assignment could crash the
compositor.

**Fix:** Split into two separate guards: first unconditionally check
`output == NULL`, then check `output->output == NULL`. This ensures no
dereference occurs when `output` is NULL regardless of surface role.

---

### C-4 — Unchecked `ivi_layout_find_bg_output()` return value (`src/layout.c`)

**Function:** `ivi_layout_fullscreen_committed()`

`ivi_layout_find_bg_output()` can return NULL when no background surface has
been set. The return value was immediately used to access
`bg_output->output->width` and `bg_output->output->height` without a NULL
check, crashing the compositor when a fullscreen surface is committed before
any background is configured.

**Fix:** Added an explicit NULL check on `bg_output` and `bg_output->output`
with an early return and log message on failure.

---

### C-5 — Unchecked `weston_head_get_destroy_listener()` return (`src/compositor.c`)

**Function:** `simple_head_disable()`

`weston_head_get_destroy_listener()` can return NULL if no matching listener is
registered. The return value was used without a NULL check in the subsequent
call, causing a NULL dereference crash.

This was compounded by issue H-1 (see below): the call used `wl_list_empty`
instead of `wl_list_remove`.

**Fix:** Added a NULL check before using the listener pointer (combined fix
with H-1).

---

## High

### H-1 — `wl_list_empty` used instead of `wl_list_remove` (`src/compositor.c`)

**Function:** `simple_head_disable()`

`wl_list_empty()` is a read-only predicate returning whether a list is empty —
it does not remove anything. The head destroy listener was never actually
unregistered, leaving a dangling listener that would fire again on subsequent
head destruction events, potentially triggering a handler on already-freed data.

**Fix:** Replaced `wl_list_empty(&listener->link)` with
`wl_list_remove(&listener->link)`, guarded by the NULL check added for C-5.

---

### H-2 — Memory leak of transform string in `drm_configure_output()` (`src/compositor.c`)

**Function:** `drm_configure_output()`

`weston_config_section_get_string` was called twice using the same `char *t`
variable. The first allocation (for the `transform` key) was overwritten by the
second call (for `activation-area`) without being freed first. Only the second
allocation was freed at the end of the block, leaking the transform string on
every output configuration. With display hotplugging this leak accumulates.

**Fix:** Added `free(t)` between the two `weston_config_section_get_string`
calls.

---

### H-3 — Unchecked `zalloc`/`strdup` in `ivi_layout_save()` (`src/layout.c`)

**Function:** `ivi_layout_save()`

`zalloc(sizeof(*new_output))` was not checked for NULL. All subsequent member
assignments would immediately dereference a NULL pointer on OOM. Similarly,
`strdup(output->name)` was not checked, leaving a NULL `name` silently inserted
into the saved output list.

**Fix:** Added NULL checks after both `zalloc` and `strdup` with early returns
and log messages. On `strdup` failure the partially-constructed `new_output` is
freed before returning.

---

### H-5 — Dangling struct with NULL `app_id` on allocation failure (`src/shell.c`)

**Functions:** `ivi_update_popup()`, `ivi_update_fullscreen()`, `ivi_update_remote()`

All three update functions freed the old `app_id` string before allocating the
new one. On OOM, the function returned with the struct removed from its list
(via `wl_list_remove`) and `app_id` set to NULL. Subsequent `strcmp` calls on
this struct's `app_id` would dereference NULL.

**Fix:** Rewrote all three functions to allocate the new `app_id` string first.
Only on successful allocation is the old string freed and replaced. On
allocation failure the function returns early with the struct's state unchanged.

---

### H-6 — Unchecked `strdup` return in `_ivi_set_pending_desktop_surface_split()` (`src/shell.c`)

**Function:** `_ivi_set_pending_desktop_surface_split()`

The return value of `strdup(app_id)` was not checked. If it returned NULL on
OOM, the struct was inserted into `pending_apps` with a NULL `app_id`. Any
subsequent `strcmp` on that entry would dereference NULL.

**Fix:** Added NULL checks on both `zalloc` and `strdup` return values, freeing
any partially-constructed struct before returning on failure.

---

### H-7 — `pending_apps` list not cleaned up on destroy (`src/shell.c`)

**Function:** `ivi_compositor_destroy_pending_surfaces()`

The function cleaned up `popup_pending_apps`, `split_pending_apps`,
`fullscreen_pending_apps`, and `remote_pending_apps`, but omitted
`ivi->pending_apps` (the list used for tile/split orientation entries of type
`pending_app_tile`). These entries and their `app_id` strings were leaked on
compositor shutdown or shell-client disconnect.

**Fix:** Added a `wl_list_for_each_safe` loop to drain and free all entries in
`ivi->pending_apps`.

---

### H-8 — Integer overflow in screenshot buffer size calculation (`clients/screenshot.c`)

**Function:** `screenshot_create_shm_buffer()`, `screenshooter_output_capture()`

Buffer dimensions were `int` fields that could be negative (wrapping to large
`size_t` values) or could produce integer overflow when multiplied to compute
stride and total buffer length. Size validation relied entirely on
`assert()` statements that are compiled out in release builds (`NDEBUG`).

**Fix:**
- Replaced all `assert()` size checks with explicit runtime checks that return
  `NULL` on invalid input.
- Added division-before-multiplication overflow guards for `stride` and total
  buffer length using `SIZE_MAX / bytes_pp` and `SIZE_MAX / stride` patterns.
- Added an explicit `<= 0` check on `buffer_width`/`buffer_height` at the call
  site in `screenshooter_output_capture()` before they are cast to `size_t`.

---

### H-10 — `ivi_agl_systemd_notify()` return value ignored (`src/compositor.c`)

**Location:** `wet_main()`, compositor startup

`ivi_agl_systemd_notify()` returns `-1` on failure (e.g., when
`add_systemd_sockets()` fails). The return value was silently discarded. In a
systemd socket-activated deployment this means the compositor continues running
without the expected socket, causing silent loss of connections.

**Fix:** Added a check on the return value; logs a warning on failure.

---

## Medium

### M-2 — Unvalidated orientation value causes DoS via `assert()` (`src/shell.c`)

**Function:** `_ivi_set_shell_surface_split()`

An unrecognized `orientation` value from the Wayland protocol reached the
`default:` branch of a `switch` statement containing
`assert(!"Invalid orientation passed")`. This aborts the compositor process
(`SIGABRT`) in debug builds — a Denial-of-Service vector exploitable by any
Wayland client. In release builds with `NDEBUG` the assert is a no-op, leaving
`new_width`/`new_height` uninitialized.

**Fix:** Replaced the `assert()` with `weston_log()` + `return`, gracefully
ignoring the invalid request.

---

### M-4 — `DEBUG` unconditionally defined in gRPC proxy (`grpc-proxy/log.h`)

`#define DEBUG` was present unconditionally, causing the `LOG()` macro to emit
verbose output (app IDs, function names, gRPC operation details) to `stderr` in
all builds. Any process with access to the compositor's stderr could capture
this information.

**Fix:** Removed the unconditional `#define DEBUG`. Verbose logging is now only
enabled when `-DDEBUG` is explicitly passed at build time. A comment documents
this intent.

---

### M-5 — Unchecked `strdup` return in `choose_default_backend()` (`src/compositor.c`)

**Function:** `wet_main()`

`choose_default_backend()` returns the result of `strdup()` which can be NULL
on OOM. The returned value was used without a NULL check in the backend loading
path, which would eventually reach `strcmp()` and dereference NULL.

**Fix:** Added a NULL check after `choose_default_backend()` returns with a
`goto error_signals` on failure.

---

## Low

### L-1 — `assert()` used to check allocation results (`shared/process-util.c`)

**Function:** `custom_env_init_from_environ()`

`strdup(*it)` was validated with `assert(*ep)`. In release builds compiled with
`NDEBUG` the assertion is removed, silently storing NULL into the environment
array. The NULL would later be passed to `execve()`.

**Fix:** Replaced `strdup() + assert()` with `xstrdup()`, which calls
`abort()` with an informative error message on NULL in both debug and release
builds.

---

### L-3 — Memory leak of `transform_string` on error path (`src/compositor.c`)

**Function:** `rdp_backend_output_configure()`

`transform_string` was heap-allocated by `weston_config_section_get_string`.
When `parse_transform()` failed, the function returned `-1` without freeing
`transform_string`.

**Fix:** Added `free(transform_string)` on the error return path. Added a
corresponding `free(transform_string)` on the success path as well since the
string is no longer needed after parsing.

---

### L-4 — `cmdline` leaked on `--version` exit path (`src/compositor.c`)

**Function:** `wet_main()`

`cmdline` is allocated at startup and freed after being logged. On the
`--version` early-exit path, execution jumped to `exit_signals` before reaching
the `free(cmdline)` call, leaking the allocation.

**Fix:** Added `free(cmdline); cmdline = NULL;` on the `--version` branch before
the `goto`.

---

### L-5 — Typo and logic bug in `ivi_shell_advertise_xdg_surfaces()` (`src/shell.c`)

**Function:** `ivi_shell_advertise_xdg_surfaces()`

Two bugs in the NULL `app_id` guard:

1. Log message typo: `"app_is is null"` instead of `"app_id is null"`.
2. `return` instead of `continue`: when any one surface has a NULL `app_id`
   the entire loop was aborted, preventing all remaining surfaces from being
   advertised.

**Fix:** Corrected the typo and changed `return` to `continue`.

---

## Files Modified

| File | Issues Fixed |
|------|-------------|
| `src/layout.c` | C-1, C-4, H-3 |
| `src/shell.c` | C-2, H-5, H-6, H-7, M-2, L-5 |
| `src/desktop.c` | C-3 |
| `src/compositor.c` | C-5, H-1, H-2, H-10, M-5, L-3, L-4 |
| `clients/screenshot.c` | H-8 |
| `grpc-proxy/log.h` | M-4 |
| `shared/process-util.c` | L-1 |

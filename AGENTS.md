# Maintenance

Hark is a synchronous C99 program. Runtime dependencies are libcurl and Tcl;
yyjson and Isocline are vendored. README.md documents the supported interface.

## Build and checks

```sh
nix develop
make
make test
make integration
make terminal
make CC=clang
```

The build must remain warning-free with `-Wall -Wextra -pedantic`. Python and
`tclsh` are test tools. Tests must isolate `HOME`, `USERPROFILE` and `XDG_*` in
temporary directories. Network tests use loopback mock providers, never accounts.

Keep first-party code straightforward: one statement per line, short functions,
early returns, explicit ownership, and shared cleanup paths for acquired resources.
Comments should explain contracts and non-obvious constraints. Use ordinary C
helpers and data tables where they remove repetition. Do not edit `vendor/`.

## Source map

| File | Responsibility |
| --- | --- |
| `hark.c` | Startup flags, configuration, Tcl bindings, requests, tools, approvals, history and turn loop |
| `platform.c` | POSIX/Win32 execution, jobs, interrupts, directories, private I/O and terminal-safe printing |
| `net.c` | libcurl, shared SSE parser, response assembly and provider diagnostics |
| `auth.c` | Device login and token refresh |
| `json.c` | Owning tree adapter over yyjson |
| `store.c` | Safe startup reads, explicit saves and sidecar locks |
| `session.c` | Per-directory sessions and resume locks |
| `line.c`, `render.c` | Input editing and terminal presentation |

## Configuration and Tcl

`/` only selects Tcl evaluation for terminal input. Startup and sourced files
are Tcl scripts. Ordinary terminal input and the `prompt` command submit text;
model responses and imported files are never evaluated as Tcl.

`set hark(key) value` and `config key value` share one array trace. `config key`
reads the same variable and returns its current value. Convert Tcl values at
this boundary; lists and booleans use Tcl's types. `set_option` copies
the current C configuration, updates the affected field, validates the candidate,
and commits it. Failed writes must preserve both the variable and C state,
working directory, system prompt and history. Accepted values are owned by the
per-setting JSON tree; other fields borrow stable CLI/default strings. Changing a
setting must not reconstruct unrelated state or reset process options.

`sync_settings` publishes canonical values with tracing suppressed. Optional
empty values clear overrides. Saved scripts use Tcl list construction to quote
arguments; never interpolate data into executable Tcl text. Keep the bindings
within the Tcl 8.4 public API for older hosts. Tk is not a dependency.

Ctrl-C sets the host flag and marks a Tcl asynchronous handler. The hook must only
perform signal-safe work; interpreter errors are raised later by the async
callback. Disarm Hark's handler while Isocline reads input. Keep async handlers
alive until the host hook has been removed.

## Runtime contracts

- Validate an entire tool-call batch before any command executes. Every accepted
  call needs a result, including calls skipped after interruption.
- Keep history append-only between resets/compaction. Project instructions are
  user messages appended when changed. Model/provider changes drop opaque
  reasoning. Imports must reject unfinished calls and duplicate IDs.
- Compaction uses the same system prompt and tools with a summary request
  appended. Replace history only with a complete, smaller summary that fits the
  threshold; retain the latest user request verbatim.
- Only complete provider responses may execute tools. One SSE parser serves live
  display and final assembly. Codex may omit final output; preserve completed
  items. Do not send `parallel_tool_calls` to compatible providers.
- HTTP timeout measures idle transfer time. Review and compaction stream silently
  and cap output. Authentication refresh is the only transport retry; context
  recovery gets one compaction attempt.
- Never send an inherited API key to a different reviewer URL. Provider error
  messages must be escaped, bounded and credential-redacted.
- Print untrusted text through `host_print`/`host_eprint`. Only the renderer emits
  terminal escapes. Shell output belongs in tool results, not terminal output.
- POSIX jobs use supervisors so draining/timeouts continue during input. Windows
  uses threads and job objects. Exiting must stop jobs and release session locks,
  including Tcl's `exit` path.
- Hark's save commands must not overwrite startup files. Private writes remain
  atomic. Tcl's own commands retain their normal semantics.

Sessions live under `<state>/sessions/<folder>/<id>.json`; `latest` names the
last saved ID. Folder names double hyphens and replace path separators with `-`,
keeping the last 200 bytes for long paths. Adjacent hyphens/separators can collide,
so resume must verify the recorded cwd. Files are 0600 in private directories.

## Portability

Use C99 and avoid architecture assumptions. The host layer supports POSIX and
MinGW. Old macOS needs the wall-clock fallback and libcurl version guards.
Cross-compile host changes when a MinGW toolchain is available; runtime results
on Linux do not establish behavior on Windows, BSD or old macOS. Tcl linkage,
headers and script-library installation are platform build inputs.

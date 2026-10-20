# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Hark is a small synchronous coding agent in C99. Its only external library is libcurl. JSON uses vendored yyjson, and line editing uses vendored Isocline. README.md is the user-facing specification; keep it accurate when behavior changes.

## Build and test

```sh
make                      # build ./hark (warning-free under -Wall -Wextra -pedantic is expected)
make CC=clang             # also keep clang warning-free
make test                 # tests/unit (C) + tests/smoke.sh
make integration          # tests/integration.py, tests/persistence.py (loopback mock servers)
make terminal             # tests/terminal.py (real pty: line editing, Ctrl-C, Ctrl-D)
```

Run one Python test function:

```sh
python3 -c "import sys; sys.argv=['x','./hark']; sys.path.insert(0,'tests'); import integration; integration.test_jobs()"
```

The same pattern works for `persistence` and `terminal`. `tests/integration.py` exports `Handler`, `response()` and `run()`, which the other suites reuse for mock providers. A tool call in `response(calls=...)` is `(call_id, arguments_json[, tool_name])`.

Tests must never touch the real user's directories. Every suite sets `HOME` to a temp directory and strips `XDG_*`. Keep doing that in new tests, because the developer's shell exports `XDG_*_HOME`.

Portability checks: the code targets Linux, BSD, macOS back to 10.4 Tiger (libcurl 7.13.1, gcc 4.0, no `clock_gettime`) and Windows (MinGW). Native code cannot be run for those here, so at least compile-check them:

```sh
x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -pedantic -c platform.c -o /dev/null   # also store.c, session.c
```

`hark.c` needs libcurl headers. Syntax-check it for Windows with a stub `curl/curl.h` that declares `CURL_GLOBAL_DEFAULT`, `curl_global_init` and `curl_global_cleanup`, passed via `-I`. Guard newer libcurl APIs with `LIBCURL_VERSION_NUM`. Do not edit `vendor/` (it records unmodified upstream); Makefile flags such as `ISOCLINE_CFLAGS` handle vendor warnings.

## Architecture

- `hark.c`: nearly everything above the host layer: options and config, the input loop, the agent turn loop, tools, background jobs, approval, compaction and session checkpoints.
- `platform.c`/`.h`: the host boundary, with separate POSIX and Win32 implementations of process execution (`host_run`), background jobs (`host_job_*`), the Ctrl-C flag (`host_interrupt_*`), XDG directories (`host_dir`), private atomic file I/O and escaped printing (`host_print`/`host_eprint`).
- `net.c`: libcurl requests, Responses SSE reassembly (`response_json`) and provider error reporting (`http_report`).
- `auth.c`: ChatGPT device login and OAuth refresh.
- `store.c`: reads the startup rc and writes user-named files.
- `session.c`: sessions for `hark resume`.
- `line.c`: the Isocline wrapper.

### Config and input

- **Precedence:** CLI flags and environment variables are parsed first (`options()`). The rc file (`$XDG_CONFIG_HOME/hark/harkrc`, falling back to legacy `~/.harkrc`) is then replayed as ordinary input lines, so rc commands override flags.
- **`/set` rebuilds the whole config:** `set_option()` serializes the current state with `configuration()`, applies the key, and re-runs `options()` with no CLI arguments. Any `Config` field missing from `configuration()` silently resets on every `/set`. Process-wide state such as the active session or job table lives in module statics for this reason.
- **Path settings:** user-file paths (`auth_file`, `system_files`) are made absolute when set: CLI values in `main()`, and `/set` values in `set_option()`. A later `/set cwd` therefore cannot redirect them. `instruction_files` intentionally stay relative.
- **Recoverable config errors:** during `set_option()`, `die()` long-jumps back through `config_recovery`, so a bad setting is rejected, not fatal. Call `die()`/`bad_setting()` for validation in `options()` and its helpers. Do not change state (history, cwd) before the point where a rebuild can still fail, or undo it in the recovery path.
- **Input sources:** the main loop in `main()` takes input from the rc, `--include` files, the initial prompt and the terminal (Isocline, or plain stdio with `--no-line-edit`). Slash commands from files run exactly like terminal input.

### Turn loop and tools (`turn()`)

- **Batches:** each model response's batch of tool calls is checked as a whole by `validate_calls()` before anything runs.
- **History validity:** every call must get a result appended. After Ctrl-C, the remaining calls get "interrupted; not run" results, so the next request stays valid.
- **The four tools:** `sh`, `job_list`, `job_wait` and `job_kill`, defined in `tool_schemas()`.
  - Schemas are `strict`: every field is listed as required, with optional ones typed as nullable.
  - Arguments are re-checked by `known_args()`/`arg_number()`.
  - A foreground `sh` uses the smaller of the agent's `timeout` and `--shell-timeout`. Background jobs use the agent's timeout (default 3600 s).
- **Job supervisors:** on POSIX each background job is a forked supervisor process running `host_run`, so output is drained and timeouts are enforced while Hark sits at the prompt. It reports `status timed truncated\n` plus the output over a pipe, and stops if Hark exits. On Windows, jobs run on threads with job objects.
- **Display:** the terminal shows only `$ command` and an outcome line. Output goes to the model, never the terminal. Print any provider or tool text with `host_print`/`host_eprint`.
- **Presentation (`render.c`):** `color` and `markdown` are tri-state (`render_mode()`), resolved against stdout by `render_setup()`, which `main()` and `set_option()` call. Only `render_style()` and the renderer emit escapes. Model text goes through `render_text()`/`render_thinking()` and ends with `render_end()`. Markdown renders a line at a time and buffers table rows until the table ends.

### Prompt construction and caching

- **Order:** the system prompt (`system_text()`) is the user's `system` guidance (or `default_guidance`), then the fixed `system_rules`, then the working directory and shell, then `system_files`. Tool definitions come next. The conversation history follows.
- **Append-only history:** history is append-only so providers can reuse the cached prefix. Do not rewrite earlier messages.
- **Project instructions:** files such as `AGENTS.md` (`instruction_files`) are user-role messages tagged `[Project instructions]`. `sync_instructions()` appends a new version when the files change.
- **Compaction:** `compact()` appends its summary request to the full conversation, using the same system prompt and tools. Only the compacted result starts a new history.
- **Prefix changes:** the only expected one is `drop_reasoning()` when the model or provider changes.

### Files Hark writes

These are deliberate design constraints.

- **Never the rc:** Hark never writes the startup rc or any file named `harkrc`/`.harkrc`. `store_write()` enforces this by path, name and file identity.
- **Only named files:** user writes go only to files named with `/save` or `/export`.
- **Never the working directory:** nothing is written there, because it may be an untrusted checkout.
- **Sessions:** saved under `$XDG_STATE_HOME/hark/sessions/<folder>/`. `<folder>` is the directory's path with `-` doubled and separators turned into `-`. Each session records its `cwd`, and `hark resume` checks it. Files are 0600, directories 0700, and an advisory lock prevents two Hark processes from resuming the same session.
- **Device login:** stored in `$XDG_DATA_HOME/hark/auth.json`, with legacy `~/.hark-auth.json` still read.

### Ctrl-C

- **Armed handler:** `host_interrupt_arm()` installs a flag-setting SIGINT handler without `SA_RESTART`. It is disarmed only while Isocline reads a line, because Isocline handles ^C as a key.
- **Checking the flag:** long operations check `host_interrupted()`. libcurl aborts through its progress callback, and `host_run` kills the command's process group.
- **Recovery:** the main loop prints `[interrupted]`, drops pending include files and returns to the prompt. One-shot and piped runs exit with status 130.
- **Background jobs:** they live in their own process groups and are not affected by ^C.

### Provider quirks

- **Wire APIs:** Responses (default) or Chat Completions (`--api chat`). Some OpenAI-compatible servers only have `/chat/completions`.
- **Streaming:** main agent requests stream (`stream` setting) through `http_post_stream()`. A shared incremental SSE parser in `net.c` feeds the live printer (`Live` in `hark.c`) and final assembly (`response_json()`, `chat_response_json()`). Tools run only from the assembled, complete response. Compaction and reviewer requests pass no `Live` and stay silent, but still stream (`generate(..., stream)`) so slow models stay under the idle timeout. `--http-timeout` is enforced in `net.c`'s progress callback as time since bytes last moved, not as a total. `tests/integration.py` has `StreamHandler` for scripted, pausable streams.
- **Context usage:** `record_usage()` keeps the latest main reply's token count, and `context_window()` uses the `context_window` setting or a quiet `fetch_models()` lookup cached per provider and model. `context_label()` puts the percentage before the prompt marker.
- **Fields hark omits:** do not send `parallel_tool_calls`, because some servers reject `false`.
- **Error messages:** `http_report()` shows only the provider's error message, escaped, truncated, and with the credential redacted. Never print raw response bodies.

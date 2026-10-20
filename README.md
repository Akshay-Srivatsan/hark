# Hark

A small, synchronous coding agent in C99. Standard terminal I/O, one shell tool,
OpenAI Responses or Chat Completions, model approval by default, automatic
conversation compaction. Original code is GPL-3.0-or-later; see [LICENSE](LICENSE)
and [third-party notices](THIRD_PARTY_NOTICES.md).

The only external library is **libcurl** (7.13 or later), with an HTTPS-capable build and a trusted
CA store. JSON parsing and serialization use vendored **yyjson 0.12.0** (MIT); interactive
line editing uses vendored **Isocline 1.1.0** (MIT).
There are no runtime dependencies on curl's command-line program, Python, Node,
Git, jq, bash, a tokenizer, or any SDK. Commands the agent chooses to run may of
course need tools from your project.

## Build

Install a C99 compiler and libcurl's development headers/library, then:

```sh
make
```

Or compile directly, without make or pkg-config:

```sh
cc -std=c99 -O2 -o hark hark.c json.c net.c auth.c platform.c line.c store.c session.c render.c vendor/yyjson/yyjson.c vendor/isocline/src/isocline.c -lcurl
```

The Makefile uses POSIX make syntax and standard POSIX utilities only:

```sh
make CC=clang CFLAGS='-O2 -std=c99 -Wall -Wextra -pedantic'
make CPPFLAGS='-I/path/to/curl/include' LDFLAGS='-L/path/to/curl/lib'
make install PREFIX="$HOME/.local"
make clean
```

`DESTDIR` is supported for staged installation. Static libcurl builds may need
additional libraries in `LDLIBS`; use the flags for that libcurl build. The makefile
does not run curl-config or pkg-config. Vendored sources are included in the tree,
so building requires no network access. Isocline is built with `-Wno-unused-function`
because it leaves helpers unused when built as one file. For a compiler without
that flag, use `make ISOCLINE_CFLAGS=`.

### Platforms

The harness makes no assumptions about byte order, pointer width, alignment, or
instruction set. It targets hosted C99 implementations with 8-bit bytes; POSIX
hosts also need fork/exec, process groups, poll, and a monotonic clock.

| Target | Implementation / validation |
| --- | --- |
| Linux | Built and tested here on x86_64 |
| Darwin (macOS 10.12+) and BSD | POSIX implementation; not run here |
| x86, aarch64, PPC32/PPC64, rv32/rv64 | No architecture-specific harness code; needs a matching C99 toolchain, OS and libcurl; not run here |
| Native Windows | Win32 implementation with process pipes, timeout job objects and protected credential writes; host code cross-compiled for x86_64, not run here |
| wasm32/wasm64 | JSON and agent logic are portable, but a full executable needs host HTTP and shell bridges; browser/WASI execution is **not implemented** |

On Mac OS X before 10.12, hark should build against the system libcurl (not tested), but its timeouts use
wall-clock time because `clock_gettime` is unavailable. The libcurl and OpenSSL
shipped with older releases such as Tiger (10.4) cannot negotiate TLS 1.2, which current
HTTPS APIs require; link against a newer libcurl with
`make CPPFLAGS=... LDFLAGS=...` to reach them.

On native Windows use a C99-capable compiler such as MinGW-w64 and a Windows
libcurl build:

```sh
x86_64-w64-mingw32-gcc -std=c99 -O2 -o hark.exe hark.c json.c net.c auth.c platform.c line.c store.c session.c render.c vendor/yyjson/yyjson.c vendor/isocline/src/isocline.c -lcurl -ladvapi32
```

Or, with a POSIX make installed, `make CC=... LDLIBS='-lcurl -ladvapi32'`.
Windows itself does not need POSIX utilities at runtime. The default shell there
is `cmd.exe /d /s /c`; the tool is still named `sh`. Use `--shell C:/path/to/sh.exe`
for POSIX shell commands. The shell option names one executable, with no flags.
The Windows implementation uses narrow Win32 paths and the current code page;
full Unicode Windows filenames and older Windows job-object/owner-rights behavior
are not guaranteed. Windows ARM/PPC additionally depend on available OS/toolchain
and libcurl ports; ISA neutrality alone does not establish support.

## Use

There is no hard-coded model catalog or default. Query the configured provider
without selecting a model first:

```sh
./hark models
./hark models --chatgpt
./hark models --base-url http://127.0.0.1:8080/v1
```

`--list-models` is an equivalent flag. Model IDs are sorted and printed one per
line. Listing uses `GET /models` for API-key and compatible providers, and the
Codex model catalog for ChatGPT login, with the same credentials, timeout and
token refresh as inference. Unsupported discovery endpoints report an error;
there is no bundled fallback catalog. The Codex catalog request advertises
compatibility with Codex `0.163.0`, independently of Hark's own version. Its
`client_version` parameter can filter out newer models when set too low. Override
it with `--codex-client-version X.Y.Z` or `HARK_CODEX_CLIENT_VERSION` if the backend
changes. This is a catalog compatibility setting; Hark still identifies itself
as Hark in its HTTP headers. An empty ChatGPT catalog is reported as an error,
with the requested compatibility version, rather than a successful empty list.
The request format follows [Codex's model catalog client](https://github.com/openai/codex/blob/rust-v0.163.0/codex-rs/codex-api/src/endpoint/models.rs).
A listed model may still lack tool calling
or support for the selected API/settings. See the [OpenAI model-list API](https://developers.openai.com/api/reference/resources/models/methods/list).

Select a model when supplying a one-shot prompt:

```sh
export OPENAI_API_KEY='...'
./hark --model YOUR_MODEL 'Inspect this project and fix the failing test'
./hark --model YOUR_MODEL
```

Without a prompt, input is a line-oriented conversation. You can start without
`--model`, then use `/models` and `/model ID` to select one. `/quit` exits, `/reset`
clears the active conversation, and `/compact` requests a summary. `/help` lists the
commands. A literal one-shot prompt of `models` needs `--` to distinguish
it from a subcommand. Quote a one-shot
prompt as one argument; `--` lets it begin with a dash. Piped lines work too, but
piped input never supplies human approvals. Startup configuration lives in your per-user `harkrc`, which Hark reads but never writes. `hark resume` continues the last conversation started in the current directory.

Interactive prompts have Readline-style editing through vendored Isocline:

* Left/Right or Ctrl-B/F move the cursor; Home/End or Ctrl-A/E move to boundaries.
* Backspace/Delete edit characters; Ctrl-U/K/W delete a prefix, suffix, or word.
* Up/Down browse session history; Ctrl-R searches it; Ctrl-Z undoes edits.
* Tab completes slash commands and speed/thinking values; Shift-Tab inserts a
  newline for a multi-line prompt.
* Ctrl-C cancels the current input. Ctrl-D on an empty input exits.

While Hark is working, Ctrl-C interrupts the current response and returns to the
prompt. It stops a model request within about a second and kills a running shell
command with its whole process group. At an approval prompt it answers no. The
conversation is kept: tool calls in the interrupted batch are recorded as
interrupted (unfinished ones as not run), so you can continue or redirect the task.
Commands from `.harkrc` or `--include` files still pending are skipped. With
`--no-line-edit`, Ctrl-C at the prompt discards the partial line and Ctrl-D on an
empty line exits. In a one-shot or piped run, Ctrl-C stops Hark with exit status 130.

### Color, Markdown and thinking

Three independent settings control presentation:

| Setting | Values | Effect |
| --- | --- | --- |
| `color` (`--color[=WHEN]`) | `always`/`yes`/`force`, `never`/`no`/`none`, `auto`/`tty`/`if-tty`; default `never` | ANSI colors for commands, outcomes (green, red, yellow), jobs, notes and thinking |
| `markdown` (`--markdown[=WHEN]`) | the same; default `never` | Lay out replies as Markdown |
| `show_thinking` (`--show-thinking`) | boolean; default `false` | Show the model's reasoning, when the provider returns it |

As with `ls`, `--color` alone means `always`, and `--color=WHEN` chooses. `auto` is on
when standard output is a terminal; for color it also respects `NO_COLOR` and
`TERM=dumb`. The same values work with `/set color auto` and `/set markdown yes`.

Markdown keeps the markup visible and adds layout: tables are aligned (their
`:`-alignment honored), and with color, headings, inline code, code blocks, bold,
italic, links, quotes and list markers are styled. Without color, Markdown only
adjusts spacing: aligned tables, indented code blocks, a blank line before headings.
Streamed replies are rendered a line at a time, and a table appears when its last
row arrives. Model text is still sanitized: escape sequences in replies are shown
escaped, so only Hark's own styling reaches the terminal.

With `show_thinking`, reasoning is printed dimmed (when colored) before the reply,
instead of a single `[thinking]` line. Chat Completions providers return it as
`reasoning_content`; on the Responses API Hark requests reasoning summaries
(`reasoning.summary: auto`), which models without reasoning support may reject.

When the provider reports token usage and the context window is known, the prompt
shows how full the context is, as in `12% > `. The figure is the latest request's
tokens (input plus output) out of the model's window. The window comes from the
`context_window` setting (`--context-window N`), or else from the provider's model
list (fields such as `context_window`, `context_length` or `max_model_len`, or
llama.cpp's `meta.n_ctx` for a loaded model), looked up
once per provider and model. `/status` shows the token counts. Compaction and
`/reset` clear the figure until the next reply. Chat streams request usage with
`stream_options`.

The most recent 200 input lines are retained in memory only; no history file is
written. `--no-line-edit` selects plain terminal input. Piped input always uses
plain stdio, and approval answers are kept out of the editor history. This is a
Readline alternative, not GNU Readline itself; it does not read `.inputrc`.

Custom OpenAI-compatible providers can use either wire API:

```sh
export LOCAL_API_KEY='...'
./hark --base-url https://example.org/v1 --api chat \
  --key-env LOCAL_API_KEY --model model-name 'Read the project and explain it'

# Local providers may omit authentication.
./hark --base-url http://127.0.0.1:8080/v1 --api chat --model local-model
```

The base URL excludes `/responses`, `/chat/completions`, or `/decisions`.
`HARK_MODEL`, `HARK_BASE_URL`, `HARK_API` and `HARK_API_KEY` provide defaults.
`OPENAI_API_KEY` is a fallback only for the default OpenAI API URL (a trailing
slash is allowed). `HARK_API_KEY` takes precedence over `OPENAI_API_KEY`.
`--key-env NAME` selects an explicit key variable, overriding both defaults. A different reviewer base URL
never inherits the main provider key; use `--review-key-env` for it.

HTTPS certificate verification is always enabled; redirects are not followed.
HTTP and HTTPS base URLs are accepted directly. Plain HTTP sends credentials and conversation data without TLS;
use it only on networks you trust.
Configure proxies and CA certificates using your libcurl build's supported
settings/environment. Responses are capped at 32 MiB. `--http-timeout SEC` (default
300) is an idle timeout: a request fails only after that long with no data sent or
received, reported as `no data from the provider for SEC seconds`, so long streamed
replies are never cut off. Connecting has its own 30-second limit. Compaction and
approval-review requests also stream (without being shown), so a model that reasons
for a long time keeps them active; with `--no-stream`, a slow reply must arrive
within the timeout. Because an idle timeout cannot stop a model that keeps
generating, those two unattended requests are capped at 16384 output tokens,
reasoning included (`max_output_tokens`, or `max_completion_tokens` for Chat
Completions; not sent to the ChatGPT backend). A capped summary fails compaction
and keeps the original history; a capped review denies the command. Agent turns are
not capped: interrupt them with Ctrl-C.

Replies stream: the model's text appears as it arrives, and `[thinking]` is shown
once while a reasoning model works (its reasoning is not printed). Tool calls are
shown and run only after the whole reply has arrived and succeeded. A Responses
stream must end with a successful `response.completed` event; completed output items
are collected, including Codex streams whose final event has only metadata. A Chat
Completions stream is rebuilt from its deltas, with tool calls reassembled from
fragments, and must include a `finish_reason`. A failed, interrupted or incomplete
reply never executes tools, though text already shown stays on screen. The error
says what was wrong, such as `model request failed (HTTP 200, the stream ended
without response.completed)`, followed by any message the provider sent (escaped,
truncated, credential redacted). llama.cpp's Responses streams, which omit
`output_index`, are accepted. Use
`--no-stream` (or `/set stream false`) for servers that stream badly; the ChatGPT
backend always streams, but is then shown only when complete. Compaction and
approval-review requests are never shown. Requests offering the shell tool leave `parallel_tool_calls` at the
provider default; Hark validates a whole batch of calls, then runs them in order.
There are no general automatic retries, so tool actions are not
replayed following a transport error.

### Switch model, speed and thinking

Startup flags set the initial settings:

```sh
./hark --model YOUR_MODEL --speed fast --thinking high
```

In a running conversation:

```text
/models
/model ANOTHER_MODEL
/speed fast
/thinking high
/status
```

`/model`, `/speed` and `/thinking` without a value show the current setting.
Changes apply to subsequent agent requests and compaction. The approval reviewer
follows the agent model unless explicitly configured; its speed and thinking
settings remain at provider defaults. The visible conversation and tool results
survive a model switch. Opaque reasoning items and provider-specific reasoning
fields are discarded when switching, since another model may not accept them.
Changes survive `/reset`. Use `/save FILE` to write configuration and context to
a command file you can include later.

Speed modes are `auto` (omit the parameter; the provider chooses), `standard`
(`service_tier: "default"`), `fast` (`"priority"`), `flex` (`"flex"`), and
`ultrafast` (`"ultrafast"`). Fast and ultrafast may cost more and require account
access. These select a requested processing tier, not a guaranteed latency; a
provider may reject or downgrade an unsupported tier.

Thinking levels are `default` (omit the parameter), `none`, `minimal`, `low`,
`medium`, `high`, `xhigh`, and `max`. Responses sends `reasoning.effort`; Chat
Completions sends `reasoning_effort`. Not every model/provider supports every
level. Use `/thinking default` and `/speed auto` to restore provider defaults.
Unsupported settings produce the provider's normal request error.
`HARK_SPEED` and `HARK_THINKING` provide startup defaults, overridden by flags.
See [OpenAI's request fields](https://developers.openai.com/api/reference/resources/responses/methods/create).

### Files and directories

Hark follows the [XDG base directory specification](https://specifications.freedesktop.org/basedir-spec/latest/):

| Purpose | Path | Default when the variable is unset |
| --- | --- | --- |
| Startup commands | `$XDG_CONFIG_HOME/hark/harkrc` | `~/.config/hark/harkrc` |
| Device login | `$XDG_DATA_HOME/hark/` | `~/.local/share/hark/` |
| Session history | `$XDG_STATE_HOME/hark/sessions/` | `~/.local/state/hark/sessions/` |
| Cache (currently unused) | `$XDG_CACHE_HOME/hark/` | `~/.cache/hark/` |

Relative values of these variables are ignored, as the specification requires. On
Windows the defaults are `%APPDATA%\hark` for configuration, `%LOCALAPPDATA%\hark`
for data, and its `state` and `cache` subdirectories. For systems and installs that predate XDG, Hark still reads `~/.harkrc`
and `~/.hark-auth.json` when the XDG file does not exist; new logins go to the XDG
location. Hark writes nothing to the working directory unless you ask it to.

### harkrc is Hark input

Hark executes the per-user `harkrc` before processing `--include` files, the
initial prompt, or terminal input. It runs from any working directory; without a
home directory there is no default rc. There is no configuration file format:
`harkrc` contains exactly the commands you could enter at the Hark prompt. For
example:

```text
# Personal defaults
/set base_url http://localhost:8080/v1
/set api chat
/model local
/approval model
/set allow ["pwd", "git status --short"]
/set shell_timeout 60
/include shared-commands.hark
```

A `.harkrc` in a project directory is not run automatically. To use one, include
it: type `/include .harkrc`, pass `--include .harkrc`, or add `/include .harkrc` to
your `harkrc` to run every project's file. An included file is ordinary input and
can change any setting, including `/approval allow`, so include only files you
trust. Unlike the startup file, included files are not checked for ownership or
permissions.

`/set KEY VALUE` exposes every runtime setting. Hyphens and underscores in keys
are interchangeable. Strings can be unquoted text (the whole remaining line)
or JSON-quoted strings, which support escaped newlines. Numbers, booleans and
arrays use JSON literals; a number or boolean given for a text setting is used as
text, booleans also accept `yes`/`no`/`on`/`off`, and a list of one item may be a
plain string. `/set KEY` displays a value; `/set` displays the current
configuration. No shell evaluates these values.

A bad `/set` (an unknown key, a wrong type, an out-of-range number, an invalid URL
or a missing system file) is reported, for example `hark: http_timeout must be a
whole number from 1 to 86400`, and rejected: the previous settings stay, including
the working directory. A scripted run still exits with status 1 afterwards. In
`harkrc` or an included file, an error stops Hark before later lines run, so a
broken startup file never leaves a partial configuration.

| Settings | Values / purpose |
| --- | --- |
| `base_url`, `api`, `model` | Provider URL, `responses` or `chat`, and model ID |
| `key_env`, `chatgpt`, `auth_file` | Key environment variable name, device-login boolean, and credential-file path |
| `speed`, `thinking` | The same choices as `--speed` and `--thinking` |
| `codex_client_version`, `http_timeout` | Codex catalog compatibility version, and seconds without data before a request fails |
| `approval`, `allow`, `deny`, `policy` | Approval mode, arrays of command globs, and additional reviewer policy |
| `review_model`, `review_base_url`, `review_api`, `review_key_env` | Reviewer overrides; `review_model default` follows the agent |
| `cwd`, `shell`, `shell_timeout` | Working directory, shell executable and execution timeout in seconds |
| `context_window` | Model context size in tokens for the prompt's usage figure (`0`: ask the provider) |
| `output_bytes`, `compact_bytes`, `max_steps` | Tool-output limit, compaction threshold (`0` disables), maximum rounds per turn |
| `system` | General guidance at the start of the system prompt (replaces the built-in guidance; `default` restores it) |
| `system_files` | Files of additional trusted system instructions, appended in order (a missing file is an error) |
| `instruction_files` | Project files such as `AGENTS.md`, given to the model when present (see below) |
| `no_line_edit`, `stream` | Booleans: plain terminal input (default `false`), and showing replies as they arrive (default `true`) |
| `color`, `markdown`, `show_thinking` | Presentation; see Color, Markdown and thinking |

`/model`, `/speed`, `/thinking`, `/approval` and `/review-model` are convenient
commands for their respective settings. Use `/include`, `/import` and `/prompt`
for startup actions as well as during a conversation. To clear an optional
setting, use `/set KEY null`; reviewer base/API and key-variable overrides also
accept `default` to restore inheritance/default selection. `/reset` is required
before switching wire APIs when conversation history is present.

Environment variables and CLI flags set the initial state. The rc file then runs
as input, so its commands can change that state. To override it, execute
commands afterward using `--include overrides.hark` or the terminal. `--rc PATH`
selects a different startup file; `--no-rc` skips executing it. `--cwd` sets the
working directory but does not select the rc; a later `/set cwd PATH` changes the
working directory. Relative paths, including in `harkrc`, are
resolved from the working directory at the time the command runs.
Blank lines and lines beginning with `#` are ignored. Command files support CRLF
and a final line without a newline.

### Include commands or import context

These are distinct operations:

| Command | Effect |
| --- | --- |
| `/include FILE` | Execute the file as Hark input: slash commands run, and ordinary lines submit prompts |
| `/import FILE` | Append file contents to conversation history, without executing commands or invoking a model |
| `/prompt FILE` | Submit the entire file as one literal user prompt |
| `/export FILE` | Export conversation context as JSON for a later `/import` |
| `/context JSON` | Append serialized conversation context; used in saved session files |

`--include FILE` and `--context-file FILE` are repeatable startup equivalents.
`--prompt-file FILE` supplies the initial prompt. Includes run in order before
the initial prompt and terminal input; nested includes finish before their
parent resumes. Nesting is limited to 16 files. Included lines use the same
approval rules as terminal input; they do not supply answers to human approval
prompts. `/quit` in an included file exits Hark.

For example, including a file containing `/approval allow` changes the approval
mode. Importing that same file adds the literal text `/approval allow` to history
and leaves the approval mode unchanged. `/prompt` likewise submits slash-prefixed
text literally. Additional trusted model instructions belong in
`/set system_files ["instructions.txt"]` or `--system-file`.

### System prompt and project instructions

The system prompt is your guidance, then rules Hark always includes (approval,
fresh shells, background jobs, untrusted tool output), then the working directory
and shell, then any `system_files`. `/set system TEXT` replaces the guidance, which
defaults to a short description of a careful coding agent:

```text
/set system You are reviewing code. Prefer small, well-tested changes.
/set system default
```

Project instruction files are opt-in. Add this to your `harkrc` to use `AGENTS.md`
or `CLAUDE.md` in whatever directory Hark runs in:

```text
/set instruction_files ["AGENTS.md", "CLAUDE.md"]
```

Each listed file that exists is read relative to the working directory, and missing
ones are skipped. Because a checkout may be untrusted, their contents reach the model
as a user message, not the system prompt, labeled as project instructions that do
not override the system rules or approval. Hark prints `[loaded AGENTS.md]` when it
adds them. They are added before the first prompt and re-checked before every
request. When a file changes (for example, after the agent edits it), the new
version is appended to the conversation rather than replacing the earlier one, and
removing the files is noted the same way. Resumed sessions are not given a duplicate,
and compaction keeps the current version.

### Prompt caching

Requests are ordered for provider prompt caching: the system prompt and tool
definitions come first and change only when you change a setting they include
(`/set system`, `system_files`, `cwd`, `shell`, `shell_timeout`), and the
conversation only grows at the end. Instruction-file changes are appended. Compaction
sends the normal system prompt, tools and full conversation with the summary request
appended, so that request reuses the cached prefix; the compacted conversation then
starts with the same system prompt, tools and project instructions. Switching model
or provider drops opaque reasoning from earlier messages, which changes the prefix.

```sh
./hark --include setup.hark --context-file requirements.txt --prompt-file task.txt
./hark --context-file previous.json 'Continue with the remaining tests'
```

Text context and prompts are limited to 4 MiB and must not contain NUL bytes.
Command files and structured context are limited to 32 MiB; ordinary prompt lines
remain limited to 4 MiB. An imported JSON object with a `history` array is treated
as exported context using the active wire API. Other imported text, including an
rc file, is added literally as a user message. Context imports do not change
configuration. Unfinished tool calls and duplicate call IDs are rejected.
Importing context never replays historical tool calls. Model/provider changes
discard opaque reasoning while retaining visible conversation.

### Resume

Hark saves each conversation as it goes, so `hark resume` can pick up the most
recent one started in the current directory:

```sh
cd ~/src/project
hark 'Add tests for the parser'
# later
hark resume
hark resume 'Now fix the failing test'
```

Sessions are stored only under the state directory, never in the working
directory itself, which may be untrusted. Each directory's sessions are in a folder
named after its path, with hyphens doubled and separators replaced by `-`:
`/home/me/my-project` uses `sessions/home-me-my--project/`. Very long paths keep
their last 200 bytes. Only a hyphen next to a separator is ambiguous (`a-/b` and
`a/-b` share a folder); each session records its directory, so the other's
sessions are never resumed. Files are owner-only (0600 in 0700 directories), and Hark refuses to
load a session that is unsafe or recorded for a different directory. Nothing is
written until the conversation has content. Each session keeps being updated after
every request and tool round; resuming continues the same session. A session open
in one Hark cannot be resumed by another until it exits.

`hark resume` restores conversation context only. Settings come from `harkrc` and
flags as usual; if neither selects a model, the session's model is used. The wire
API must match the session's. `--no-session` keeps a conversation from being saved.
Transcripts include tool output, which can contain sensitive information.

### Saving

Hark never writes `harkrc`. It is input you edit by hand, like a shell rc file.
`/save FILE` writes the current configuration and context together as an
executable Hark command file. It contains ordinary `/set` commands and a
`/context` command. Including it runs those commands through the same input loop;
it does not resubmit old prompts or replay historical tools. Besides sessions in the
state directory and `hark login`'s credential file in the data directory, Hark itself writes only the
files you name with `/save` or `/export` (plus a sidecar `.lock`). Shell commands the agent runs
are not covered by this; use approval rules for those.

```text
/save session.hark
/quit
```

```sh
./hark --no-rc --include session.hark
```

`--no-rc` keeps `harkrc` from running first, so its startup prompts or imports
are not repeated. `/reset` clears conversation context while retaining
configuration. Input-editing history is memory-only. `/status` shows the active rc path.

`/save` and `/export` refuse to write the active startup file (by any path,
including a hard link), the `--rc` file, or any file named `harkrc` or `.harkrc`. Older
generated rc files that end in `/autosave` now stop with an error; delete that line.

Writes are atomic, use private temporary files, and are capped at 32 MiB.
Hark locks writes using a sidecar `.lock` file. On POSIX,
the startup rc file may be 0644 but must be owned by the current user and not
writable by other users; Hark writes saved files as 0600. Symlinks, directories, and
Windows reparse points are rejected as startup rc files and save targets. New Windows
files use owner-rights ACLs. Keep saved session files out of version control:
transcripts and tool output can contain sensitive information. Credential values and OAuth
tokens are never saved as configuration; only key variable names and the
auth-file path are saved. Device login still uses its separate credential file.

### ChatGPT device login

```sh
./hark login
./hark --chatgpt --model YOUR_CODEX_MODEL 'Fix the test'
```

Open the printed URL, sign in, and enter the device code. For the default
Responses provider, when no API key is configured, both `hark models` and normal
agent runs automatically reuse a safe saved Hark login. An API key takes
precedence; `--chatgpt` explicitly selects the saved account even when a key is
configured. Custom providers and Chat Completions never auto-select the saved
login. Login polls for up to
15 minutes. Device login must be enabled for the account/workspace, and the
account must have access to the requested model. Device login is separate from
OpenAI API billing and API keys; it uses ChatGPT's Codex Responses backend.

Credentials default to `$XDG_DATA_HOME/hark/auth.json` (see Files and directories;
an existing pre-XDG `~/.hark-auth.json` is still used). A relative `--auth-file` means
the directory Hark was started from, not `--cwd`, and a relative `/set auth_file` is
fixed to the current directory when set, so credentials are never read from or
written into a working directory that changes later.
Override with `--auth-file PATH` on both commands. POSIX writes use an atomic
0600 temporary file and rename; reads reject symlinks, non-owner files and group/
world permissions. Windows writes use a protected owner-rights DACL and atomic
replacement, and reads reject reparse points. Keep the parent directory private;
Windows reads do not audit permissions on pre-existing files. Tokens are plaintext
and must be treated as passwords. Remove the auth file to forget this local
login; that does not revoke a token on the server. Refresh tokens are renewed
when needed and after one authentication failure. Concurrent processes sharing
one auth file are not supported (refresh-token rotation can race).

If model listing reports 401, check which credential source the diagnostic names.
An old `HARK_API_KEY` can shadow `OPENAI_API_KEY`; select the latter with
`--key-env OPENAI_API_KEY` if needed. For device login, `hark models --chatgpt`
explicitly selects the saved session; a rejected session is refreshed once, then
reports that another `hark login` is needed. Missing OpenAI credentials are
reported locally before a request is sent. No token or arbitrary server error
body is printed. A failed request shows the HTTP status and only the provider's
error message (`error.message`, or a top-level `message`/`detail`), with control
characters escaped, at most 400 bytes, and the request's key or token redacted.

This is a direct implementation of the public Codex device-code protocol and
public client identifier, rather than a dependency on the Codex executable. That
backend is subject to upstream changes and account restrictions. Use API-key
mode if it is unavailable. Live account login was not performed during the local
tests. References: [OpenAI authentication](https://learn.chatgpt.com/docs/auth),
[Codex device login implementation](https://github.com/openai/codex/blob/main/codex-rs/login/src/device_code_auth.rs).

### Shell approval

Every proposed command is shown before approval. `--approval allow` unconditionally
approves every valid shell request, bypassing allow/deny patterns, prompts and
model review:

```sh
./hark --model YOUR_MODEL --approval allow 'Run the project test suite'
```

In the other approval modes, the order is:

1. Any matching `--deny` rule denies it, unconditionally.
2. Any matching `--allow` rule permits it.
3. Otherwise, `--approval ask` prompts for an explicit `y` or `yes`,
   `--approval deny` denies, and `--approval model` asks a separate model.

Model approval is the default: a separate request to the current agent model
reviews each command, with its own instructions and no tools. It inherits the
agent provider and credentials. Switching `/model` also switches the reviewer
unless you have explicitly selected a reviewer model.

Change approval settings during a conversation:

```text
/approval ask
/approval model
/review-model REVIEW_MODEL
/review-model default
/status
/save session.hark
```

`/approval` accepts `model`, `ask`, `deny`, or `allow`. `/review-model default`
(or `--review-model default`) restores following the agent model. Without an
argument, either command shows its current setting. These choices survive
`/reset`; `/save FILE` writes them together with the conversation.
An imported context file cannot change approval settings; an included command file can. Use the
`--review-base-url`, `--review-api`, and `--review-key-env` startup flags to select
a different reviewer provider, including a decision model (examples below).

In `ask` mode, EOF (Ctrl-D), Ctrl-C, noninteractive stdin, or an empty answer means deny; Ctrl-C also interrupts the response. Rules can be repeated up to 128 times each. They are case-sensitive,
whole-command globs: `*` matches any sequence, including newlines; `?` matches one
byte. There are no regexes, bracket classes or escaping rules.

```sh
./hark --model YOUR_MODEL \
  --allow 'pwd' --allow 'git status --short' --deny '*sudo*'
```

Globs inspect text, not shell semantics. For example, `--allow 'git *'` also
matches `git status; dangerous-command`. Use exact allows where possible. A deny
pattern can also be bypassed semantically through an equivalent spelling; it is
not an isolation mechanism. Shell commands run with the harness's permissions
and inherited environment. There is no filesystem/network sandbox. Model review
is fallible and repository content can influence what a command executes.

Each command is shown as `$ COMMAND` followed by its outcome, such as `[exit 0]`,
`[exit 124, timed out]` or `[exit 2, output truncated]`. Its output (stdout and
stderr, up to `--output-bytes`) goes only to the model, not the terminal; transcripts
from `/export`, `/save` and sessions still contain it. A foreground command blocks:
Hark waits for it to exit, time out or be interrupted with Ctrl-C before continuing.
The model may give a foreground command a `timeout`; Hark uses the shorter of that
and `--shell-timeout` (default 120 seconds), so the model can only lower your limit. Standard input is `/dev/null` (`NUL` on
Windows).

### Background jobs

The model has basic job control, so it can run work in the background and in
parallel:

| Tool | Effect |
| --- | --- |
| `sh` with `background: true` | Start the command as a job and return its ID at once. `timeout` (seconds, default 3600, at most 86400) is chosen by the model |
| `job_wait` | Wait up to `timeout` seconds (0 just checks; at most `--shell-timeout`) for the listed jobs, or for every job that is running or whose result has not been returned. Returns each job's state, exit status and, once finished, output |
| `job_list` | List jobs with state (`running`, `exited`, `timed_out`, `killed`), exit status and elapsed time |
| `job_kill` | Stop a job and every process it started |

Starting several jobs and then waiting for them runs them in parallel. Background
commands need the same approval as foreground ones. A job shows as `$ COMMAND &`
and `[job 1 started]`; results appear as `[job 1: exit 0] COMMAND`. Jobs keep
running between turns and while you are at the prompt, where Hark reports any that
finished. Ctrl-C interrupts the current response, including a `job_wait`, but not
background jobs. `/jobs` lists jobs and `/kill JOB` stops one. At most 16 jobs run at
once (256 per Hark process), each keeping up to `--output-bytes` of output; output
is available once the job finishes. When Hark exits it stops every job still
running, and a job also stops if Hark is killed. Jobs are not restored by `hark resume`.

On POSIX each job runs under a small supervisor process in its own process group,
which captures output and enforces the timeout even while Hark waits for input; on
Windows each job runs on a thread with a job object that is closed with Hark.

```sh
./hark --model YOUR_MODEL --approval model \
  --review-model REVIEW_MODEL --review-api responses \
  --policy 'Permit reading files and running this project test suite.'

# Separate OpenAI-compatible reviewer:
./hark --model YOUR_MODEL --approval model \
  --review-base-url https://review.example.org/v1 --review-api chat \
  --review-key-env REVIEW_API_KEY --review-model REVIEW_MODEL

# OpenAI Decisions API (API key required for this reviewer):
./hark --chatgpt --model YOUR_CODEX_MODEL --approval model \
  --review-base-url https://api.openai.com/v1 --review-api decisions \
  --review-key-env OPENAI_API_KEY --review-model YOUR_DECISION_MODEL
```

The reviewer sees the command, shell, working directory, latest user request,
conversation and additional policy. This sends that context to the selected
review provider. It has no tools. LLM reviewers must return exactly `ALLOW`
(surrounding whitespace accepted); every other response, refusal, unexpected tool
call, malformed output or network error denies execution. Decisions review asks
one `choice` question named `shell_approval` with `ALLOW`/`DENY` choices, and accepts
only a matching typed `ALLOW` answer. Use a model supporting that endpoint; see
[the Decisions API](https://developers.openai.com/api/docs/guides/decisions).

Each shell call has closed stdin, merged stdout/stderr, a default 120-second
limit and a default 65536-byte retained output limit. Excess output is drained and
discarded. Results include exit status, timeout and truncation flags. Fresh shells
do not preserve `cd` or environment changes; `--cwd` sets the shared working
directory. POSIX process groups / Windows job objects clean up ordinary child
processes on completion and timeout. This is process cleanup, not a sandbox;
programs that deliberately escape process groups are outside that guarantee.

### Compaction and limits

At a completed tool-round boundary, when serialized history reaches
`--compact-bytes` (default 196608), the same provider/model summarizes it. The
summary request is appended to the conversation, with the usual system prompt and
tools (the model is told not to call them), so it reuses the cached prompt prefix.
The replacement contains the current project instructions, the summary and the
latest user request verbatim; old tool-call/result pairs are removed together. Summary replacement is
atomic in memory: an empty, incomplete, failed or nonshrinking summary leaves the
original history intact and ends the turn with an error. A summary must also fit
under the configured threshold. Set `--compact-bytes 0` to disable automatic
compaction and context-error recovery; explicit `/compact` still works.

A context-limit API error triggers one compaction attempt and retry. This is a
byte heuristic, **not a tokenizer**: choose a threshold comfortably below your
model's input window, allowing for system instructions, tool schemas and output.
If even the summarization request exceeds the provider's context limit, the run
stops; no history is silently dropped. Summarization is lossy, so inspect its
consequences in important workflows. The system prompt, including `system_files`,
is never compacted, and project instruction files are re-added after compaction.

`--max-steps` bounds model rounds per user turn (default 100). Reaching it exits
nonzero even if the last tool succeeded. Other useful controls are `--shell`,
`--shell-timeout`, `--http-timeout`, `--output-bytes`, and `--cwd`; see `--help`.
Terminal control characters in model/shell text are escaped. Embedded NUL in API
JSON is rejected; NUL in shell output becomes `?`, and invalid UTF-8 is replaced
when encoding results. A separate execution audit log is not implemented; saved rc files include
tool requests and results.

## Tests

```sh
make test           # C unit tests + POSIX sh CLI smoke tests
make integration    # optional Python 3 standard-library mock HTTP tests
make terminal       # optional POSIX pseudo-terminal editing tests (Python 3)
```

Integration tests use only temporary directories and loopback HTTP; no credentials,
external provider, or billable model calls are needed. They exercise both wire
formats, tool results, human-approval fail-closed behavior without a terminal,
allow/deny precedence, reviewer errors, Decisions, compaction, malformed batches,
SSE completion (including metadata-only Codex completion events), context-limit recovery, replay rejection, round limits, and a
mocked device-login / OAuth refresh flow, model discovery without an initial model,
runtime setting changes, unconditional approval, rc persistence, streamed replies (live
text, reassembled tool calls, truncated streams), color/Markdown/thinking presentation, and background jobs
(parallel runs, agent timeouts, kill, listing, argument checks, and cleanup at exit).
Persistence tests cover all rc configuration keys, precedence, the read-only startup file,
context/prompt imports, exports, safe transcript continuation, concurrent sessions,
`hark resume` (locking, per-directory keys, unsafe sessions), and XDG paths with
pre-XDG fallbacks. Tests never use the real user's directories.
validation, raw integer preservation, Unicode, shell output limits/timeouts,
closed stdin and POSIX credential permissions. Terminal tests exercise cursor
editing, history recall/search, completion, input cancellation, EOF, restoring
terminal settings, and Ctrl-C during model requests, shell commands and approval prompts. A live, read-only catalog comparison confirmed that compatibility
version `0.1.0` returned zero models while `0.163.0` returned ten for the same
session. A minimal live Codex response was also reconstructed successfully from
completed stream items. Live service entitlements and non-Linux runtime behavior
still require separate validation.

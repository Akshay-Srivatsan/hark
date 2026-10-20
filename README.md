# Hark

A synchronous coding agent in C99. It uses libcurl for HTTP, Tcl for commands and
configuration, vendored yyjson for JSON, and vendored Isocline for line editing.
Original code is GPL-3.0-or-later; see [LICENSE](LICENSE) and
[third-party notices](THIRD_PARTY_NOTICES.md).

## Build and test

Install a C99 compiler, libcurl development files, and Tcl development files and
script library. Tk is not needed. The bindings use Tcl 8.4 APIs; the default
linker flag selects Tcl 8.6. Set the include and linker flags for your installation:

```sh
make
make TCL_CFLAGS=-I/usr/include/tcl8.6 TCL_LIBS=-ltcl8.6
make CC=clang
make install PREFIX="$HOME/.local"
```

The Makefile is POSIX make. `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, `LDLIBS`, `PREFIX` and
`DESTDIR` are supported. Static builds need the dependencies of both libcurl and
Tcl in their linker flags. The Tcl script library must remain available at runtime;
linking Tcl statically does not embed that library. No `tclsh` subprocess is used
by Hark. To build Isocline with a compiler lacking `-Wno-unused-function`, set
`ISOCLINE_CFLAGS=`. Vendored sources are built without modification.

```sh
nix build                # ./result/bin/hark
nix run -- --help
nix develop              # compiler, libraries, Python and tclsh
nix flake check          # build, unit tests and CLI smoke tests
make test                # C unit tests and POSIX sh smoke tests
make integration         # Python mock-provider and persistence tests
make terminal            # Python POSIX pseudo-terminal tests
```

Network tests use temporary directories and loopback servers, without credentials
or external providers. Persistence tests also use `tclsh` to read saved scripts.

The host implementations target Linux, BSD, macOS and native Windows (MinGW).
Hark requires hosted C99 with 8-bit bytes. POSIX execution uses fork/exec, process
groups and poll. Before macOS 10.12, timeouts use wall time instead of
`clock_gettime`. Older systems need suitable builds of Tcl and an HTTPS-capable
libcurl with a trusted CA store; old system TLS libraries may be insufficient.
Only Linux runtime behavior is tested here. Windows paths use the current code
page, and full Unicode filename support is not guaranteed. Browser and WASI host
implementations are not provided.

For MinGW, select the target Tcl headers/library and add `-ladvapi32` to `LDLIBS`.
Tcl supports native MinGW builds; Tk and POSIX utilities are not needed at runtime.

## Start a conversation

There is no default model. List the provider's models, then select one:

```sh
export OPENAI_API_KEY='...'
hark models
hark --model MODEL 'Inspect this project and fix the failing test'
hark --model MODEL
```

Without a prompt argument, Hark reads input until EOF or `/quit`. Ordinary input
is sent to the model. A leading `/` routes the rest of the input to Tcl:

```text
/config model MODEL
/set hark(thinking) high
Inspect the parser and explain its failure modes.
/status
/quit
```

A prompt argument must be one shell argument. Use `--` for a prompt beginning
with `-` or equal to a subcommand such as `models`. Piped input follows the same
rules as terminal input, but cannot answer human approval prompts.

The editor supports cursor movement, history/search, completion, undo, and
Shift-Tab for a newline. It keeps the last 200 input lines in memory only.
`--no-line-edit` selects plain stdio. Ctrl-D on an empty input exits. Ctrl-C
cancels input or interrupts a request/foreground command. Interactive runs return
to the prompt; one-shot and piped runs exit with status 130. An approval prompt
treats Ctrl-C and EOF as denial. Background jobs continue until killed or Hark exits.

## Tcl commands and configuration

The startup file and files loaded with `source` are Tcl scripts. They have no `/`
prefix. Standard Tcl quoting, lists, variables, substitutions, procedures and
control flow apply. Use braces for literal text containing `$`, `[` or `;`:

```tcl
config model MODEL
set hark(thinking) high
config allow {pwd {git status --short}}
config system {Review code carefully. Preserve the user's changes.}
source personal-settings.tcl
prompt {Inspect src/parser.c and fix its error handling.}
```

`config KEY VALUE` is equivalent to `set hark(KEY) VALUE`. Both validate the
setting immediately. Invalid writes restore the previous value and raise a Tcl
error. Read a setting with `set hark(KEY)` and list settings with `array get hark`.
Values are canonicalized after assignment: booleans become `true`/`false`, and
user-file paths become absolute. The managed array is restored if unset; use an
empty value to clear optional overrides.

Command errors stop the current script unless caught with Tcl's `catch`.
At the terminal, Hark reports the error and accepts more input. An uncaught error
in a startup or `--include` script stops startup. Earlier successful commands are
retained; a script is not a transaction. An uncaught error makes the eventual
process status nonzero.

| Command | Effect |
| --- | --- |
| `config KEY VALUE` | Write `hark(KEY)` |
| `models` | List provider models |
| `status` | Show active settings, startup path and context usage |
| `prompt TEXT` | Submit one literal string to the model |
| `prompt-file FILE` | Submit a file's entire contents as one prompt |
| `source FILE` | Evaluate a Tcl script, using Tcl's normal file handling |
| `import FILE` | Append text or exported JSON context, without running commands or the model |
| `context JSON` | Append serialized conversation context |
| `export FILE` | Write context as JSON |
| `save FILE` | Write settings and context as a Tcl script |
| `compact` | Summarize conversation history |
| `reset` | Clear history, keeping settings |
| `jobs` | List background jobs |
| `kill JOB` | Stop a background job |
| `help` | List Hark commands |
| `quit` | Exit Hark |

Terminal scripts start with `/`; subsequent lines of an incomplete Tcl command
continue that script. `prompt {/quit}` sends `/quit` as text. Tcl scripts have the
same filesystem, process and network access as Hark itself. Only trusted scripts
should be executed. Imported context and model output are never evaluated as Tcl.

### Startup order and paths

Flags and environment variables establish initial settings. Hark then runs the
startup script, restores context for `resume` when requested, imports
`--context-file` files, runs `--include` scripts in order, and submits the initial
prompt. With no initial prompt, it then reads terminal or piped input.
Startup commands can override flags. Use an additional `--include` script to
apply later settings. `--rc FILE` selects a startup file; `--no-rc` skips it.
Project-local startup files are never loaded automatically.

| Purpose | Path | Default |
| --- | --- | --- |
| Startup script | `$XDG_CONFIG_HOME/hark/harkrc` | `~/.config/hark/harkrc` |
| Device login | `$XDG_DATA_HOME/hark/auth.json` | `~/.local/share/hark/auth.json` |
| Sessions | `$XDG_STATE_HOME/hark/sessions/` | `~/.local/state/hark/sessions/` |
| Cache directory (unused) | `$XDG_CACHE_HOME/hark/` | `~/.cache/hark/` |

Relative XDG variables are ignored. Windows defaults are `%APPDATA%\hark` for
configuration and `%LOCALAPPDATA%\hark` for data, with `state` and `cache`
subdirectories. `~/.harkrc` and `~/.hark-auth.json` are not implicit fallback paths.
Without a home directory, provide explicit startup/credential paths as needed.

`--cwd` changes directory before startup. Paths in scripts are relative to the
current working directory, including paths passed to `source`. `auth_file` and
`system_files` are made absolute when set, so later directory changes do not
redirect them. `--auth-file` is relative to the launch directory; `--system-file`
is relative to `--cwd`. `instruction_files` remain relative to the active directory.

### Settings reference

Keys use underscores. CLI equivalents normally replace underscores with hyphens
and add `--`; exceptions are noted below. `hark --help` lists startup flags.

| Key | Default and accepted values |
| --- | --- |
| `base_url` | `https://api.openai.com/v1`; HTTP(S) API base without the endpoint suffix |
| `api` | `responses`; or `chat` |
| `model` | Empty; select a provider model ID before submitting a prompt |
| `speed` | `auto`; `standard`, `fast`, `flex`, `ultrafast` |
| `thinking` | `default`; `none`, `minimal`, `low`, `medium`, `high`, `xhigh`, `max` |
| `key_env` | Empty; explicit environment variable holding the API key |
| `chatgpt` | `false`; use saved ChatGPT login |
| `auth_file` | Per-user credential path above |
| `codex_client_version` | `0.163.0`; three numeric components `X.Y.Z` |
| `http_timeout` | `300`; 1–86400 seconds without transfer progress |
| `approval` | `model`; `ask`, `deny`, `allow` |
| `allow`, `deny` | Empty Tcl lists of command globs; at most 128 each |
| `policy` | Empty; additional reviewer policy |
| `review_model` | Empty; inherit the agent model |
| `review_base_url`, `review_api`, `review_key_env` | Empty; provider/API/key overrides; reviewer API also accepts `decisions` |
| `cwd` | Current directory |
| `shell` | `/bin/sh`, or `cmd.exe` on Windows; one executable without flags |
| `shell_timeout` | `120`; 1–86400 seconds |
| `output_bytes` | `65536`; 1–4194304 retained bytes per shell command |
| `compact_bytes` | `196608`; 0–16777216 serialized history bytes; 0 disables automatic compaction/recovery |
| `max_steps` | `100`; 1–100000 model rounds per user turn |
| `context_window` | `0`; 0–100000000 tokens; 0 asks the provider |
| `system` | Empty selects built-in guidance |
| `system_files` | Empty Tcl list of trusted system-instruction files; missing files are errors |
| `instruction_files` | Empty Tcl list of project-instruction files; missing files are skipped |
| `no_line_edit` | `false` |
| `stream` | `true`; CLI disables with `--no-stream` |
| `color`, `markdown` | `never`; `always`, `auto`; a bare CLI flag means `always` |
| `show_thinking` | `false` |

Boolean settings use Tcl boolean values. List settings use Tcl lists. Empty values
clear optional reviewer, key, system and file/directory overrides. Model/provider
changes retain visible history but discard opaque reasoning. Changing the wire
API requires `reset` when history is nonempty. Selecting a custom base/API/key
turns off ChatGPT mode. Reviewer keys are inherited only for the same base URL.

`HARK_MODEL`, `HARK_BASE_URL`, `HARK_API`, `HARK_SPEED`, `HARK_THINKING` and
`HARK_CODEX_CLIENT_VERSION` supply startup defaults. `HARK_API_KEY` supplies the
key, falling back to `OPENAI_API_KEY` only for the default OpenAI base URL.
`key_env` selects a specific variable instead. CLI flags override environment
defaults; the startup script runs afterward.

## Providers and authentication

```sh
hark models --base-url http://127.0.0.1:8080/v1
hark --base-url https://example.org/v1 --api chat \
  --key-env LOCAL_API_KEY --model MODEL
hark login
hark models --chatgpt
hark --chatgpt --model MODEL
```

Model discovery uses `GET /models`, or the Codex catalog for ChatGPT login. IDs
are sorted, one per line. There is no fallback catalog; an unsupported endpoint
or empty ChatGPT catalog is an error. `--list-models` is equivalent to `models`.
The Codex catalog compatibility version is separate from Hark's release version
and can affect which models are returned. Listing does not guarantee support for
tool calls, speed tiers or thinking levels.

Without an API key, the default Responses provider automatically uses a safe
saved Hark login. `--chatgpt` explicitly selects that login even when a key is
present. Custom providers and Chat Completions do not auto-select device login.
Login prints a URL and code, then polls for up to 15 minutes. It requires account
access to device login and the requested model; it uses the ChatGPT Codex backend,
separate from API-key billing. Tokens are refreshed when needed and once after
an authentication failure. Concurrent refreshes sharing one file are unsupported.
Remove the credential file to forget the local login; this does not revoke the
server token. Credentials are plaintext and must be protected as passwords.

HTTPS certificate verification is enabled, redirects are disabled, and responses
are capped at 32 MiB. Plain HTTP sends credentials and context without TLS.
Proxy and CA configuration comes from libcurl. Connection timeout is 30 seconds;
`http_timeout` measures idle transfer time rather than total request duration.

Replies normally stream. Tools run only after a complete successful reply and
validation of the entire call batch. Responses streams require
`response.completed`; completed output items are retained when the final event
contains only metadata. Chat streams reassemble deltas and require a successful
`finish_reason`. Failed or interrupted replies execute no tools. There are no
general transport retries; a context-limit error can trigger one compaction and
retry. Provider diagnostics show HTTP status and a short escaped error message,
with the request credential redacted.

`speed` maps to `service_tier`: `auto` omits it, `standard` sends `default`, and
`fast` sends `priority`. `thinking` maps to Responses `reasoning.effort` or Chat
`reasoning_effort`; `default` omits it. Support and pricing depend on the provider.
`show_thinking` shows returned reasoning; Responses requests summaries for this.
Otherwise a streamed reasoning reply shows one `[thinking]` notice.

## Shell execution and approval

The model has four tools: `sh`, `job_list`, `job_wait` and `job_kill`.
Commands are displayed before approval. With `approval allow`, every valid shell
request is approved. Otherwise, matching deny globs take precedence over allow
globs, followed by the selected approval mode:

- `ask`: require `y`, `Y` or `yes` from an interactive terminal; EOF denies.
- `deny`: deny unmatched commands.
- `model`: ask a separate reviewer request, without tools.

Globs match the whole command, case-sensitively: `*` matches any bytes, including
newlines; `?` matches one byte. They do not parse shell syntax. For example,
`git *` also matches `git status; another-command`. Approval rules and model
review are conveniences, not a filesystem or network sandbox. Commands inherit
Hark's environment and permissions.

The reviewer follows the agent model unless overridden. It receives the command,
shell, directory, latest request, conversation and policy. A separate reviewer URL
therefore receives that context. Responses/Chat reviewers must return exactly
`ALLOW` apart from whitespace; errors, refusals, tools and other replies deny.
Decisions reviewers must return one typed `ALLOW` choice named `shell_approval`.
Configure a separate reviewer with `review_base_url`, `review_api`,
`review_key_env` and `review_model`.

Foreground commands use fresh shells, closed stdin and merged stdout/stderr.
Their timeout is the smaller of the model's requested timeout and `shell_timeout`.
`cd` and environment changes do not persist between commands. Hark displays the
command and outcome; captured output goes to the model and saved transcripts.
Output beyond `output_bytes` is drained and discarded. Results include exit
status, timeout and truncation flags. POSIX process groups and Windows job objects
clean up ordinary descendants on completion, interruption and timeout; processes
that deliberately escape those mechanisms are outside this guarantee.

`sh` with `background: true` starts a job and returns its ID. Its model-selected
timeout defaults to 3600 seconds, at most 86400. `job_wait` checks or waits for
specified jobs, or running jobs and undelivered results; its wait is limited by
`shell_timeout`. `job_list` reports states and elapsed time; `job_kill` stops a job.
Up to 16 jobs run at once, with 256 job IDs per process. Jobs continue between
turns and are stopped when Hark exits or dies. They are not restored by `resume`.
POSIX jobs use supervisor processes; Windows jobs use threads and job objects.

## History, files and presentation

Hark saves conversations under the state directory. `hark resume` restores the
most recently saved conversation for the starting directory. Settings still come
from flags and the startup script; a saved model is used only if neither selects
one. The wire API must match. `--no-session` disables automatic session storage.
Sessions are locked against concurrent resumption and store their directory to
prevent a mismatched resume. No session is written until history has content.

`save FILE` writes a Tcl script of settings followed by a `context` command.
`source FILE` restores it without replaying past prompts or tool calls.
`export FILE` writes JSON for `import FILE`. Plain text imports become user
messages. Imported JSON context must match the active API and contain complete
call/result pairs without duplicate call IDs; it cannot change settings.
Use `--no-rc --include FILE` to restore a saved script without running startup
commands first. Secrets are not saved as settings; only key variable names and
credential paths are recorded. Transcripts can still contain sensitive tool output.

Startup scripts must be regular owned files, not writable by other users on
POSIX. Credential/session files require owner-only permissions. Explicit saves
are atomic, capped at 32 MiB, and locked with a sidecar `.lock`. Files are written
as 0600 on POSIX; new Windows private files use owner-rights ACLs. Symlinks and
Windows reparse points are rejected for startup/private reads and save targets.
Windows reads do not audit existing ACLs. Hark refuses to save over its startup
file, hard links to it, or any file named `harkrc` or `.harkrc`. Tcl's own file and
process commands have their normal behavior and are not subject to these save
restrictions.

The system prompt contains guidance, fixed tool/approval rules, the current
directory and shell, then `system_files`. Project instructions are opt-in, for
example `config instruction_files {AGENTS.md CLAUDE.md}`. They are user messages,
read before requests and appended again when changed or removed. Earlier history
is not rewritten, preserving provider cache prefixes. Compaction sends the full
history plus a summary request, then replaces history with current project
instructions, the summary and the latest user request. Empty, incomplete,
nonshrinking or oversized summaries leave history intact and fail the turn.

Automatic compaction uses serialized bytes, not token counts. Set its threshold
below the provider's context limit, allowing for instructions, tools and output.
`compact_bytes 0` disables automatic compaction and context-error recovery;
explicit `compact` remains available. System instructions are never compacted.
Compaction and review stream silently and request at most 16384 output tokens
except on the ChatGPT backend. A capped summary fails; a capped review denies.

With token usage and a known context window, the prompt shows a percentage such
as `12% > `. The count is the latest main request's input plus output tokens.
Hark uses `context_window` or caches a provider model-list lookup. Reset and
compaction clear usage until the next response.

`color auto` follows stdout being a terminal, respecting `NO_COLOR` and
`TERM=dumb`; `markdown auto` follows stdout alone. Markdown keeps markers visible,
styles spans and blocks when colored, and aligns tables. Tables are buffered
until complete; other streaming layout is line-based. Untrusted terminal control
characters are escaped. Shell NUL bytes become `?`; invalid UTF-8 is replaced
when encoded as JSON. Text prompts are limited to 4 MiB; context and startup/
`--include` files to 32 MiB. Tcl's `source` uses Tcl's own file handling.

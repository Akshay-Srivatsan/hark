#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
hark=${1:-./hark}
# Never read or write the real user's hark directories.
home=$(mktemp -d); trap 'rm -rf "$home"' EXIT
HOME=$home; export HOME
unset XDG_CONFIG_HOME XDG_DATA_HOME XDG_STATE_HOME XDG_CACHE_HOME
run_hark() { "$hark" --no-rc "$@"; }
run_hark --help >/dev/null
run_hark --version >/dev/null
if run_hark --model test --api bad >/dev/null 2>&1; then exit 1; fi
if run_hark --model test --shell-timeout 0 >/dev/null 2>&1; then exit 1; fi
if run_hark --model test --approval invalid test >/dev/null 2>&1; then exit 1; fi
if run_hark --model test --chatgpt --base-url https://example.org test >/dev/null 2>&1; then exit 1; fi
if run_hark --model test --speed invalid >/dev/null 2>&1; then exit 1; fi
if run_hark --model test --thinking invalid >/dev/null 2>&1; then exit 1; fi
if run_hark models 'unexpected prompt' >/dev/null 2>&1; then exit 1; fi
if run_hark models --codex-client-version '0.1.0&bad=1' >/dev/null 2>&1; then exit 1; fi
printf '/help\n/status\n/quit\n' | run_hark --auth-file /dev/null >/dev/null
if run_hark --model test --http >/dev/null 2>&1; then exit 1; fi
printf 'smoke tests passed\n'

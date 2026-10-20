#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real terminal editing with a POSIX pseudo-terminal; no network."""
import errno
import fcntl
import json
import os
import pathlib
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
from http.server import ThreadingHTTPServer
from integration import Handler, response

binary = str(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else './hark').resolve())


def test_editor():
    with tempfile.TemporaryDirectory(prefix='hark-terminal-') as tmp:
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
        initial = termios.tcgetattr(slave)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('HARK_', 'XDG_')) and k != 'OPENAI_API_KEY'}
        env.update(HOME=tmp, USERPROFILE=tmp, TERM='xterm', NO_COLOR='1')
        p = subprocess.Popen([binary, '--auth-file', str(pathlib.Path(tmp) / 'absent')], stdin=slave, stdout=slave, stderr=slave, env=env, cwd=tmp, close_fds=True)
        transcript = bytearray()
        def read_until(needle, timeout=8):
            start = len(transcript)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if select.select([master], [], [], .05)[0]:
                    try:
                        data = os.read(master, 65536)
                    except OSError as exc:
                        if exc.errno == errno.EIO:
                            break
                        raise
                    if not data:
                        break
                    transcript.extend(data)
                    # Minimal terminal response for Isocline's cursor queries.
                    for _ in range(data.count(b'\x1b[6n')):
                        os.write(master, b'\x1b[1;1R')
                    if needle in transcript[start:]:
                        return
                if p.poll() is not None:
                    break
            raise AssertionError(f'missing {needle!r}: {bytes(transcript[-3000:])!r}')
        def command(keys, expected):
            os.write(master, keys)
            read_until(expected)
            time.sleep(.06)
        try:
            read_until(b'> ')
            # Ctrl-B moves left; Ctrl-D deletes inside a nonempty line.
            command(b'/model tXst\x02\x02\x02\x04e\r', b'model: test\r\n')
            # Recall the previous complete command.
            command(b'\x1b[A\r', b'model: test\r\n')
            # Ctrl-U clears the prefix; Ctrl-A/E move to line boundaries.
            command(b'garbage\x01\x05\x15/status\r', b'thinking: default\r\n')
            # Completion and setting changes.
            command(b'/thinking hi\t\r', b'thinking: high\r\n')
            command(b'/speed fast\r', b'speed: fast\r\n')
            # Ctrl-R history search, accept found entry, then submit it.
            command(b'\x12/model\r\r', b'model: test\r\n')
            # Ctrl-C cancels input while preserving the session.
            os.write(master, b'/model unwanted\x03')
            time.sleep(.1)
            command(b'/status\r', b'model: test\r\n')
            # Empty-line Ctrl-D exits and restores cooked terminal attributes.
            os.write(master, b'\x04')
            assert p.wait(timeout=5) == 0
            restored = termios.tcgetattr(slave)
            assert (restored[3] & (termios.ICANON | termios.ECHO)) == (initial[3] & (termios.ICANON | termios.ECHO))
            assert not list(pathlib.Path(tmp).iterdir()), 'local commands and editor history must not write files by default'
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
            os.close(master)
            os.close(slave)


def controlling_terminal():
    # Make the pty Hark's controlling terminal, so ^C raises SIGINT as in a real shell.
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


def test_interrupt(line_edit=True):
    release = threading.Event()
    seen = []
    def handler(path, body, headers):
        users = [v['content'] for v in body['input'] if v.get('role') == 'user']
        seen.append(body['input'])
        if users[-1] == 'hang':
            release.wait(30)
            return 200, response('too late'), False
        if users[-1] == 'run tool':
            return 200, response(calls=[('slow', '{"command":"sleep 30; echo finished"}'), ('next', '{"command":"echo never"}')]), False
        if users[-1] == 'ask':
            return 200, response(calls=[('asked', '{"command":"echo asked"}')]), False
        finished = body['input'][-1].get('type') == 'function_call_output'
        if users[-1] == 'background' and not finished:
            return 200, response(calls=[('bg', '{"command":"sleep 30","background":true,"timeout":null}'),
                                        ('wait', '{"ids":null,"timeout":20}', 'job_wait')]), False
        if users[-1] == 'quick' and not finished:
            return 200, response(calls=[('quick', '{"command":"sleep 0.3","background":true,"timeout":null}')]), False
        return 200, response('after ' + users[-1]), False
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.handler, server.requests, server.errors = handler, [], []
    server.handle_error = lambda *_: None  # Hark hangs up on the abandoned request.
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    with tempfile.TemporaryDirectory(prefix='hark-interrupt-') as tmp:
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
        env = {k: v for k, v in os.environ.items() if not k.startswith(('HARK_', 'XDG_')) and k != 'OPENAI_API_KEY'}
        env.update(HOME=tmp, USERPROFILE=tmp, TERM='xterm', NO_COLOR='1')
        args = [binary, '--no-rc', '--base-url', f'http://127.0.0.1:{server.server_port}/v1', '--model', 'main', '--approval', 'allow']
        if not line_edit:
            args.append('--no-line-edit')
        p = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave, env=env, cwd=tmp, preexec_fn=controlling_terminal)
        transcript = bytearray()
        cursor = [0]
        def read_until(needle, timeout=8):
            # Search forward from the previous match; one read may hold several needles.
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                found = transcript.find(needle, cursor[0])
                if found >= 0:
                    cursor[0] = found + len(needle)
                    return
                if select.select([master], [], [], .05)[0]:
                    try:
                        data = os.read(master, 65536)
                    except OSError as exc:
                        if exc.errno == errno.EIO:
                            break
                        raise
                    if not data:
                        break
                    transcript.extend(data)
                    for _ in range(data.count(b'\x1b[6n')):
                        os.write(master, b'\x1b[1;1R')
                    continue
                if p.poll() is not None:
                    break
            raise AssertionError(f'missing {needle!r}: {bytes(transcript[-3000:])!r}')
        try:
            read_until(b'> ')
            # ^C during a model request abandons it and returns to the prompt.
            os.write(master, b'hang\r')
            time.sleep(.5)
            started = time.monotonic()
            os.write(master, b'\x03')
            read_until(b'[interrupted]')
            assert time.monotonic() - started < 3, 'request must stop promptly'
            read_until(b'> ')
            release.set()
            # ^C during a shell command kills it, and skips the rest of the batch.
            os.write(master, b'run tool\r')
            read_until(b'sleep 30')
            time.sleep(.3)
            os.write(master, b'\x03')
            read_until(b'[interrupted]')
            read_until(b'> ')
            # The next request carries a valid history: every call has an interrupted result.
            os.write(master, b'continue\r')
            read_until(b'after continue')
            read_until(b'> ')  # Isocline flushes type-ahead when it enters raw mode.
            outputs = {v['call_id']: json.loads(v['output']) for v in seen[-1] if v.get('type') == 'function_call_output'}
            assert outputs['slow']['interrupted'] and outputs['slow']['exit_status'] == 130, outputs
            assert outputs['next']['interrupted'] and 'not run' in outputs['next']['error'], outputs
            assert 'finished' not in outputs['slow']['output'] and b'$ echo never' not in transcript
            if not line_edit:
                # ^C at the plain prompt discards the partial line.
                os.write(master, b'discard me\x03')
                read_until(b'[interrupted]')
                os.write(master, b'kept\r')
                read_until(b'after kept')
                read_until(b'> ')
                assert not any('discard me' in json.dumps(h) for h in seen)
            # ^C during job_wait stops waiting; the background job keeps running until killed.
            os.write(master, b'background\r')
            read_until(b'[waiting up to 20s for job 1]')
            time.sleep(.3)
            os.write(master, b'\x03')
            read_until(b'[interrupted]')
            read_until(b'> ')
            os.write(master, b'/jobs\r')
            read_until(b'[job 1: running for ')
            read_until(b'> ')
            os.write(master, b'/kill 1\r')
            read_until(b'[job 1: killed] sleep 30')
            read_until(b'> ')
            # A job that finishes between prompts is reported before the next one.
            os.write(master, b'quick\r')
            read_until(b'[job 2 started]')
            read_until(b'after quick')
            read_until(b'> ')
            time.sleep(.8)
            os.write(master, b'/status\r')
            read_until(b'[finished job 2: exit 0] sleep 0.3')
            read_until(b'> ')
            # ^C at a human approval prompt denies and interrupts; the session continues.
            os.write(master, b'/approval ask\r')
            read_until(b'approval: ask')
            read_until(b'> ')
            os.write(master, b'ask\r')
            read_until(b'[y/N] ')
            os.write(master, b'\x03')
            read_until(b'[interrupted]')
            read_until(b'> ')
            os.write(master, b'/status\r')
            read_until(b'approval: ask')
            read_until(b'> ')
            assert b'asked\r\n' not in transcript.split(b'echo asked', 1)[1]
            # ^D on an empty prompt exits normally.
            time.sleep(.1)
            os.write(master, b'\x04')
            assert p.wait(timeout=5) == 0
            assert not server.errors, server.errors
        finally:
            release.set()
            if p.poll() is None:
                p.kill()
                p.wait()
            os.close(master)
            os.close(slave)
            server.shutdown()
            server.server_close()


if __name__ == '__main__':
    test_editor()
    print('terminal editing tests passed')
    test_interrupt()
    print('interrupt tests passed: line editor')
    test_interrupt(line_edit=False)
    print('interrupt tests passed: plain input')

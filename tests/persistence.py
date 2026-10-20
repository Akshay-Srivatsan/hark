#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only harkrc, explicit saves, and file import tests using loopback only."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer
from integration import Handler, response

BINARY = str(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else './hark').resolve())


def environment(root):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('HARK_', 'XDG_')) and k != 'OPENAI_API_KEY'}
    return dict(env, HOME=str(root), USERPROFILE=str(root), TEST_KEY='secret-not-saved', REVIEW_KEY='review-secret')


def startup(home):
    path = home / '.config' / 'hark' / 'harkrc'
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def good(p):
    assert p.returncode == 0, (p.stdout, p.stderr)
    return p


def tcl_word(text):
    escapes = {'\n': r'\n', '\r': r'\r', '\t': r'\t'}
    return ''.join(escapes.get(c, '\\' + c if c in '\\ {}[]$;"' else c) for c in str(text)) or '{}'


def rc_state(path):
    script = r'''
proc hex {value} {binary scan [encoding convertto utf-8 $value] H* result; return $result}
proc context {value} {global saved_context; set saved_context $value}
source [lindex $argv 0]
foreach {key value} [array get hark] {
    if {$key in {allow deny system_files instruction_files}} {
        set values {}; foreach item $value {lappend values [hex $item]}
        puts "$key [join $values ,]"
    } else {puts "$key [hex $value]"}
}
puts "context [hex $saved_context]"
'''
    p = subprocess.run(['tclsh', '/dev/stdin', str(path)], input=script, text=True, capture_output=True, check=True)
    state = {}
    arrays = {'allow', 'deny', 'system_files', 'instruction_files'}
    booleans = {'chatgpt', 'no_line_edit', 'stream', 'show_thinking'}
    numbers = {'http_timeout', 'shell_timeout', 'compact_bytes', 'output_bytes', 'max_steps', 'context_window'}
    for line in p.stdout.splitlines():
        key, value = line.split(' ', 1)
        if key in arrays:
            state[key] = [bytes.fromhex(v).decode() for v in value.split(',')] if value else []
        else:
            value = bytes.fromhex(value).decode()
            if key == 'context': state.update(json.loads(value))
            elif key in booleans: state[key] = value == 'true'
            elif key in numbers: state[key] = int(value)
            else: state[key] = value
    return state


def rc_script(values):
    lines = []
    for key, value in values.items():
        if key == 'version': continue
        if isinstance(value, list): value = ' '.join(tcl_word(v) for v in value)
        elif isinstance(value, bool): value = 'true' if value else 'false'
        lines.append('config ' + key + ' ' + tcl_word(value) + '\n')
    return ''.join(lines)


def test_local():
    with tempfile.TemporaryDirectory(prefix='hark-rc-') as temp:
        root = pathlib.Path(temp)
        project = root / 'project'
        project.mkdir()
        env = environment(root)
        def invoke(*args, text='', directory=project, extra_env=None):
            return subprocess.run([BINARY, *args], cwd=directory, input=text, text=True, capture_output=True, env={**env, **(extra_env or {})}, timeout=15)
        p = good(invoke(text='/config model first\n/config approval allow\n/status\n/quit\n'))
        assert not list(project.iterdir())
        work = project / 'work'
        work.mkdir()
        (work / 'notes.txt').write_text('Imported background\nwith a second line.')
        (work / 'task.txt').write_text('/quit\nThis entire file is a prompt.')
        (work / 'system.txt').write_text('Use portable C99.')
        rc = startup(root)  # HOME: the startup file is per-user.
        config = dict(version=1, base_url='http://127.0.0.1:1/v1', api='chat', model='configured',
                      speed='fast', thinking='high', key_env='TEST_KEY', chatgpt=False,
                      codex_client_version='1.2.3', http_timeout=7, shell='/bin/sh',
                      approval='model', policy='Read only unless asked.', allow=['pwd'], deny=['*sudo*'],
                      review_model='pinned', review_base_url='http://127.0.0.1:2/v1',
                      review_api='decisions', review_key_env='REVIEW_KEY',
                      system_files=['system.txt'], auth_file='auth.json', cwd='work', no_line_edit=True,
                      compact_bytes=0, output_bytes=1234, max_steps=9, shell_timeout=3)
        rc.write_text('config cwd work\n' + rc_script({k:v for k,v in config.items() if k != 'cwd'}) + 'import notes.txt\n'); rc.chmod(0o644)
        before = rc.read_bytes()
        # Every runtime setting can originate in the startup script. /save never calls a model.
        good(invoke(text='/save saved.hark\n/quit\n'))
        assert rc.read_bytes() == before, 'harkrc is never written'
        saved_file = work / 'saved.hark'
        saved = rc_state(saved_file)
        for key, value in config.items():
            if key not in ('version', 'cwd', 'auth_file', 'system_files', 'prompt_file', 'context_files'):
                assert saved[key] == value, (key, saved)
        # A relative auth_file is fixed to the directory current when it was set.
        assert saved['cwd'] == str(work) and saved['auth_file'] == str(work / 'auth.json')
        assert saved['system_files'] == [str(work / 'system.txt')]
        assert [v['content'] for v in saved['history']] == [(work / 'notes.txt').read_text()]
        assert 'prompt_file' not in saved and 'context_files' not in saved and 'prompt' not in saved
        assert 'secret-not-saved' not in saved_file.read_text() and 'review-secret' not in saved_file.read_text()
        assert saved_file.stat().st_mode & 0o777 == 0o600
        p = good(invoke('--model', 'flag', text='/status\n/quit\n', extra_env={'HARK_MODEL': 'env', 'HARK_SPEED': 'flex'}))
        assert 'model: configured' in p.stdout and 'speed: fast' in p.stdout, 'rc commands execute after initial CLI/environment settings'
        # All interactive choices are saved together, including approval and reviewer inheritance.
        good(invoke(text='/config model changed\n/config approval deny\n/config review_model {}\n/config speed standard\n/config thinking low\n/save state.hark\n/quit\n'))
        state = rc_state(work / 'state.hark')
        assert state['model'] == 'changed' and state['approval'] == 'deny'
        assert state['review_model'] == '' and state['speed'] == 'standard' and state['thinking'] == 'low'
        assert len(state['history']) == 1, 'imports and pending prompts must not repeat after saving'
        assert rc.read_bytes() == before
        # Hark refuses every write to a startup file: the active rc (by any path,
        # including a hard link), any other .harkrc, and the --rc file.
        (work / 'sub').mkdir()
        os.link(rc, work / 'hardlink.hark')
        for command in ('/save', '/save ../.harkrc', '/save ./../.harkrc', '/save sub/.harkrc', '/save sub/.HarkRC', '/export ../.harkrc', '/save hardlink.hark'):
            p = invoke(text=command + '\n/quit\n')
            assert p.returncode == 1 or 'usage: save FILE' in p.stderr, (command, p.stderr)
            assert rc.read_bytes() == before and not list((work / 'sub').iterdir()), command
        # --cwd changes the working directory only; the per-user rc still runs.
        p = good(invoke('--cwd', str(project), '--system-file', 'work/system.txt', text='/status\n/quit\n', directory=root))
        assert 'model: configured' in p.stdout
        # --rc selects a different startup file, which is also never written.
        alt = project / 'alternate.rc'
        alt.write_text('config model alternate\nimport pending.txt\n')
        (project / 'pending.txt').write_text('Pending prompt')
        alt_before = alt.read_bytes()
        p = invoke('--rc', 'alternate.rc', text='/status\n/save alternate.rc\n/save alternate.hark\n/quit\n')
        assert p.returncode == 1 and 'model: alternate' in p.stdout and 'refusing to write a startup file' in p.stderr
        assert alt.read_bytes() == alt_before and rc.read_bytes() == before
        assert rc_state(project / 'alternate.hark')['history'][-1]['content'] == 'Pending prompt'
        # Config errors never silently fall back to permissive defaults.
        for value in ('/unknown', '/config typo true', '/config http_timeout 0'):
            rc.write_text(value.removeprefix("/"))
            assert invoke(text='/quit\n').returncode == 1
        good(invoke('--no-rc', text='/status\n/quit\n'))
        rc.write_text('config model safe\n'); rc.chmod(0o666)
        assert invoke(text='/quit\n').returncode == 1
        rc.unlink(); rc.symlink_to(alt)
        assert invoke(text='/quit\n').returncode == 1
        rc.unlink()
        # Bad imports do not clear existing context or change approval policy.
        (project / 'bad.json').write_text(json.dumps({'api': 'responses', 'history': [{'type': 'function_call', 'call_id': 'unfinished'}]}))
        p = invoke('--no-rc', text='/import bad.json\n/save after-bad.hark\n/quit\n')
        assert p.returncode == 1 and rc_state(project / 'after-bad.hark')['history'] == []
        (project / 'nul.txt').write_bytes(b'hello\0world')
        assert invoke('--prompt-file', 'nul.txt', '--model', 'test').returncode == 1
        assert invoke('--context-file', 'missing.txt').returncode == 1
        assert not (project / '.hark').exists()


def test_per_user_rc():
    with tempfile.TemporaryDirectory(prefix='hark-user-rc-') as temp:
        root = pathlib.Path(temp)
        home, project = root / 'home', root / 'project'
        home.mkdir(); project.mkdir()
        (project / '.harkrc').write_text('config model project\nconfig approval deny\n')
        def invoke(*args, text='/status\n/quit\n', directory=project, env=None):
            return subprocess.run([BINARY, *args], cwd=directory, input=text, text=True, capture_output=True, env=env or environment(home), timeout=10)
        fallback = home / '.harkrc'
        fallback.write_text('config model fallback\n')
        p = good(invoke())
        assert 'model: fallback' in p.stdout and str(fallback) in p.stdout
        p = good(invoke(env={**environment(home), 'XDG_CONFIG_HOME': str(root / 'xdg')}))
        assert 'model: fallback' in p.stdout
        p = good(invoke('--no-rc'))
        assert 'model: fallback' not in p.stdout
        p = good(invoke('--rc', 'missing.rc'))
        assert 'model: fallback' not in p.stdout
        startup(home).write_text('config model user\n')
        # The user startup script runs from any directory; a project .harkrc does not run on its own.
        p = good(invoke())
        assert 'model: user' in p.stdout and 'project' not in p.stdout and str(startup(home)) in p.stdout
        p = good(invoke('--cwd', str(project), directory=root))
        assert 'model: user' in p.stdout and 'project' not in p.stdout
        # Opt in to a project file with source, as terminal input or from the startup script.
        p = good(invoke(text='/source .harkrc\n/status\n/quit\n'))
        assert 'model: project' in p.stdout and 'approval: deny' in p.stdout
        startup(home).write_text('config model user\nsource .harkrc\n')
        p = good(invoke())
        assert 'model: project' in p.stdout
        # --rc still selects a file explicitly; without HOME there is no default rc.
        p = good(invoke('--rc', '.harkrc', text='/status\n/quit\n'))
        assert 'model: project' in p.stdout
        env = {k: v for k, v in environment(home).items() if k not in ('HOME', 'USERPROFILE')}
        p = good(invoke(env=env))
        assert 'model: user' not in p.stdout and 'model: project' not in p.stdout
        startup(home).write_text('unknown_command\n')
        assert invoke().returncode == 1
        startup(home).chmod(0o666)
        assert invoke().returncode == 1


def session_key(directory):
    # The session folder is the path with hyphens doubled and separators as '-'.
    return str(directory).strip('/').replace('-', '--').replace('/', '-') or '-'


def test_resume():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.requests, server.errors = [], []
    def handler(path, body, headers):
        users = [v['content'] for v in body['input'] if v.get('role') == 'user']
        return 200, response('answer to ' + users[-1]), False
    server.handler = handler
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='hark-resume-') as temp:
            root = pathlib.Path(temp).resolve()
            home, project, other = root / 'home', root / 'project', root / 'other'
            for d in (home, project, other):
                d.mkdir()
            env = environment(home)
            provider = ['--base-url', f'http://127.0.0.1:{server.server_port}/v1', '--model', 'main', '--key-env', 'TEST_KEY']
            def invoke(*args, directory=project, text='', extra_env=None):
                return subprocess.run([BINARY, *args], cwd=directory, input=text, text=True, capture_output=True, env={**env, **(extra_env or {})}, timeout=15)
            # Nothing is saved before there is a conversation, and nothing in the working directory.
            good(invoke(*provider, text='/status\n/quit\n'))
            assert not (home / '.local').exists()
            good(invoke(*provider, '--', 'first'))
            sessions = home / '.local/state/hark/sessions' / session_key(project)
            saved = json.loads((sessions / ((sessions / 'latest').read_text().strip() + '.json')).read_text())
            assert saved['cwd'] == str(project) and saved['latest_user'] == 'first'
            assert not list(project.iterdir()), 'sessions are never stored in the working directory'
            assert sessions.stat().st_mode & 0o777 == 0o700
            assert all(f.stat().st_mode & 0o777 == 0o600 for f in sessions.iterdir())
            # resume continues the same conversation, and keeps saving to the same session.
            p = good(invoke('resume', *provider, '--', 'second'))
            assert '[resumed session]' in p.stderr and 'answer to second' in p.stdout
            sent = [v.get('content') for v in server.requests[-1][1]['input']]
            assert 'first' in sent and 'second' in sent and any('answer to first' in json.dumps(v) for v in server.requests[-1][1]['input'])
            assert len([f for f in sessions.iterdir() if f.suffix == '.json']) == 1
            # The model may come from the session when nothing else selects one.
            p = good(invoke('resume', '--base-url', provider[1], '--key-env', 'TEST_KEY', '--', 'third'))
            assert server.requests[-1][1]['model'] == 'main'
            # Another directory has no session; a session open in another hark is not shared.
            p = invoke('resume', *provider, directory=other, text='/quit\n')
            assert p.returncode == 1 and 'no saved hark session' in p.stderr
            holder = subprocess.Popen([BINARY, 'resume', *provider], cwd=project, env=env, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                while True:
                    p = invoke('resume', *provider, text='/quit\n')
                    if p.returncode == 1 and 'open in another hark' in p.stderr:
                        break
                    assert time.monotonic() < deadline, p.stderr
                    time.sleep(.05)
            finally:
                holder.communicate('/quit\n', timeout=10)
            # --no-session saves nothing; a new session becomes the one to resume.
            before = sorted(f.name for f in sessions.iterdir())
            good(invoke(*provider, '--no-session', '--', 'private'))
            assert sorted(f.name for f in sessions.iterdir()) == before
            time.sleep(1.1)
            good(invoke(*provider, '--', 'fresh'))
            p = good(invoke('resume', *provider, text='/export ' + str(root / 'resumed.json') + '\n/quit\n'))
            assert json.loads((root / 'resumed.json').read_text())['latest_user'] == 'fresh'
            # Sessions are state: XDG_STATE_HOME is used when absolute and ignored when
            # relative; XDG_DATA_HOME (device login) does not move them.
            assert not (home / '.local/share').exists()
            xdg = root / 'xdg-state'
            good(invoke(*provider, '--', 'xdg', extra_env={'XDG_STATE_HOME': str(xdg), 'XDG_DATA_HOME': str(root / 'xdg-data')}))
            assert (xdg / 'hark/sessions' / session_key(project) / 'latest').exists() and not (root / 'xdg-data').exists()
            good(invoke(*provider, '--', 'relative', extra_env={'XDG_STATE_HOME': 'relative'}))
            assert not (project / 'relative').exists()
            # ~/.harkrc is used only when the XDG startup file is absent.
            (home / '.harkrc').write_text('config model legacy\n')
            p = good(invoke(text='/status\n/quit\n'))
            assert 'model: legacy' in p.stdout
            (home / '.config/hark').mkdir(parents=True, exist_ok=True)
            (home / '.config/hark/harkrc').write_text('config model xdg\n')
            p = good(invoke(text='/status\n/quit\n'))
            assert 'model: xdg' in p.stdout and str(home / '.config/hark/harkrc') in p.stdout
            p = invoke(text='/save ' + str(home / '.config/hark/harkrc') + '\n/quit\n')
            assert p.returncode == 1 and (home / '.config/hark/harkrc').read_text() == 'config model xdg\n'
            # The folder is named after the directory, so it is easy to find.
            assert sessions.name.endswith('-project') and 'hark--resume--' in sessions.name, sessions.name
            # Hyphens are escaped, so my-dir and my/dir have separate folders.
            (root / 'my-dir').mkdir(); (root / 'my' / 'dir').mkdir(parents=True)
            good(invoke(*provider, '--', 'hyphen', directory=root / 'my-dir'))
            p = invoke('resume', *provider, directory=root / 'my' / 'dir', text='/quit\n')
            assert p.returncode == 1 and 'no saved hark session for this directory\n' in p.stderr, p.stderr
            good(invoke(*provider, '--', 'slash', directory=root / 'my' / 'dir'))
            assert (home / '.local/state/hark/sessions' / session_key(root / 'my-dir')).name.endswith('-my--dir')
            p = good(invoke('resume', *provider, directory=root / 'my-dir', text='/export ' + str(root / 'hyphen.json') + '\n/quit\n'))
            assert json.loads((root / 'hyphen.json').read_text())['latest_user'] == 'hyphen'
            # Only a hyphen next to a separator is ambiguous; the recorded cwd keeps them apart.
            (root / 'a-' / 'b').mkdir(parents=True); (root / 'a' / '-b').mkdir(parents=True)
            assert session_key(root / 'a-' / 'b') == session_key(root / 'a' / '-b')
            good(invoke(*provider, '--', 'edge', directory=root / 'a-' / 'b'))
            p = invoke('resume', *provider, directory=root / 'a' / '-b', text='/quit\n')
            assert p.returncode == 1 and 'is for ' + str(root / 'a-' / 'b') in p.stderr, p.stderr
            # Very long paths keep their most specific end within the name limit.
            deep = root.joinpath(*(['d' * 60] * 6), 'deep-project')
            deep.mkdir(parents=True)
            good(invoke(*provider, '--', 'deep', directory=deep))
            folders = [f.name for f in (home / '.local/state/hark/sessions').iterdir()]
            assert any(len(f) <= 200 and f.endswith('-deep--project') for f in folders), folders
            good(invoke('resume', *provider, '--', 'deeper', directory=deep))
            # A tampered or unsafe session is refused.
            target = sessions / ((sessions / 'latest').read_text().strip() + '.json')
            data = json.loads(target.read_text()); data['cwd'] = str(other); target.write_text(json.dumps(data))
            p = invoke('resume', *provider, text='/quit\n')
            assert p.returncode == 1 and 'is for ' + str(other) in p.stderr, p.stderr
            target.chmod(0o644)
            p = invoke('resume', *provider, text='/quit\n')
            assert p.returncode == 1 and 'unsafe' in p.stderr, p.stderr
        assert not server.errors, server.errors
    finally:
        server.shutdown(); server.server_close(); thread.join()


def test_instructions():
    bodies = []
    def handler(path, body, headers):
        bodies.append(body)
        users = [v['content'] for v in body['input'] if v.get('role') == 'user']
        if users[-1] == 'edit' and body['input'][-1].get('type') != 'function_call_output' and not users[-1].startswith('[Project'):
            return 200, response(calls=[('e', json.dumps({'command': "printf 'Use spaces.' > AGENTS.md"}))]), False
        return 200, response('ok'), False
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.requests, server.errors, server.handler = [], [], handler
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='hark-instructions-') as temp:
            root = pathlib.Path(temp).resolve()
            home, project = root / 'home', root / 'project'
            home.mkdir(); project.mkdir()
            env = environment(home)
            startup(home).write_text('config instruction_files AGENTS.md\\ CLAUDE.md\nconfig system Be\\ terse.\n')
            (project / 'AGENTS.md').write_text('Use tabs.')
            provider = ['--base-url', f'http://127.0.0.1:{server.server_port}/v1', '--model', 'main', '--key-env', 'TEST_KEY', '--approval', 'allow']
            def invoke(*args, text=''):
                return subprocess.run([BINARY, *args], cwd=project, input=text, text=True, capture_output=True, env=env, timeout=15)
            def instruction_messages(body):
                return [v['content'] for v in body['input'] if v.get('role') == 'user' and v['content'].startswith('[Project instructions]')]
            # config system replaces the general guidance; the fixed rules stay.
            p = good(invoke(*provider, '--', 'edit'))
            first, second = bodies[-2], bodies[-1]
            assert first['instructions'].startswith('Be terse.\n') and 'You are a coding agent' not in first['instructions']
            assert 'denied requests must be respected' in first['instructions'] and 'Use tabs' not in first['instructions']
            # Instruction files are a user message before the prompt; missing ones are skipped.
            assert first['input'][0]['content'].startswith('[Project instructions]') and '# AGENTS.md\n\nUse tabs.' in first['input'][0]['content']
            assert first['input'][1]['content'] == 'edit' and 'CLAUDE.md' not in first['input'][0]['content']
            assert '[loaded AGENTS.md]' in p.stderr
            # An edit to AGENTS.md is appended; the earlier request is an unchanged prefix (cache reuse).
            assert second['input'][:len(first['input'])] == first['input'], 'the prompt prefix must not change'
            assert instruction_messages(second)[-1].endswith('Use spaces.') and second['input'][-1]['content'].startswith('[Project instructions]')
            # Resuming does not duplicate unchanged instructions; removing the files is announced once.
            good(invoke('resume', *provider, '--', 'again'))
            assert len(instruction_messages(bodies[-1])) == 2 and bodies[-1]['input'][-1]['content'] == 'again'
            (project / 'AGENTS.md').unlink()
            good(invoke('resume', *provider, '--', 'gone'))
            assert 'No instruction files are present' in instruction_messages(bodies[-1])[-1]
            good(invoke('resume', *provider, '--', 'still gone'))
            assert len(instruction_messages(bodies[-1])) == 3
            # An empty system setting restores the built-in guidance; system_files are appended in order.
            (project / 'one.txt').write_text('First file.'); (project / 'two.txt').write_text('Second file.')
            good(invoke(*provider, '--no-session', text='/config system {}\n/config system_files one.txt\\ two.txt\nhello\n/quit\n'))
            assert bodies[-1]['instructions'].startswith('You are a coding agent') and bodies[-1]['instructions'].endswith('\nFirst file.\nSecond file.')
            good(invoke(*provider, '--no-session', '--system-file', 'one.txt', '--system-file', 'two.txt', '--', 'flags'))
            assert bodies[-1]['instructions'].endswith('\nFirst file.\nSecond file.')
            p = invoke(*provider, '--no-session', '--system-file', 'missing.txt', '--', 'x')
            assert p.returncode == 1 and 'cannot read system file' in p.stderr
        assert not server.errors, server.errors
    finally:
        server.shutdown(); server.server_close(); thread.join()


def test_bad_settings():
    with tempfile.TemporaryDirectory(prefix='hark-bad-settings-') as temp:
        root = pathlib.Path(temp)
        start, other = root / 'start', root / 'other'
        start.mkdir(); other.mkdir()
        (start / 'rel.txt').write_text('Relative system file.')
        def invoke(*args, text=''):
            return subprocess.run([BINARY, *args], cwd=start, input=text, text=True, capture_output=True, env=environment(root), timeout=10)
        bad = {
            '/config color sometimes': 'color must be always, never or auto',
            '/config http_timeout abc': 'http_timeout must be a whole number from 1 to 86400',
            '/config http_timeout 0': 'http_timeout must be a whole number from 1 to 86400',
            '/config no_line_edit maybe': 'expected boolean value',
            '/config base_url ftp://example.org': 'provider URL must start with',
            '/config typo 1': 'unknown setting: typo',
            '/config system_files missing.txt': 'cannot read system file',
        }
        p = invoke(text=''.join(line + '\n' for line in bad) + '/config no_line_edit true\n/config model 7\n/set hark(http_timeout)\n/set hark(no_line_edit)\n/status\n')
        for line, message in bad.items():
            assert message in p.stderr, (line, p.stderr)
        assert 'model: 7' in p.stdout and '300' in p.stdout.splitlines() and 'true' in p.stdout.splitlines(), p.stdout
        assert p.returncode == 1
        # A relative system file remains pinned across directory changes.
        invoke(text=f'/config system_files rel.txt\n/config cwd {other}\n/export out.json\n')
        assert (other / 'out.json').exists()
        # A cwd setting that fails after changing directory (here, the pinned system file
        # vanished) is rejected and leaves the working directory where it was.
        p = subprocess.Popen([BINARY], cwd=start, env=environment(root), text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            p.stdin.write('/config system_files rel.txt\n'); p.stdin.flush()
            assert str(start / 'rel.txt') in p.stdout.readline()
            (start / 'rel.txt').unlink()
            out, err = p.communicate(f'/config cwd {other}\n/export moved.json\n', timeout=10)
        finally:
            if p.poll() is None: p.kill()
        assert 'cannot read system file' in err and (start / 'moved.json').exists() and not (other / 'moved.json').exists(), err
        # A relative auth_file is pinned when set, so changing directory cannot move it.
        p = invoke(text=f'/config auth_file creds.json\n/config cwd {other}\n/set hark(auth_file)\n')
        assert str(start / 'creds.json') in p.stdout.splitlines(), p.stdout
        # In a startup file, a bad setting still stops Hark before running anything else.
        rc = root / '.config' / 'hark'
        rc.mkdir(parents=True)
        (rc / 'harkrc').write_text('config color sometimes\nconfig model never-reached\n')
        p = invoke(text='/status\n')
        assert p.returncode == 1 and 'color must be' in p.stderr and 'never-reached' not in p.stdout, (p.stdout, p.stderr)


def test_include_import():
    with tempfile.TemporaryDirectory(prefix='hark-include-') as temp:
        root = pathlib.Path(temp)
        (root / 'commands.hark').write_text('config approval allow\nconfig model included\nsource nested.hark\n')
        (root / 'nested.hark').write_bytes(b'config speed fast\r\nconfig thinking high')
        startup(root).write_text('# Same commands as terminal input\nconfig model startup\nimport commands.hark\n')
        def invoke(*args, text=''):
            return subprocess.run([BINARY, *args], cwd=root, env=environment(root), input=text, text=True, capture_output=True, timeout=10)
        p = good(invoke(text='/status\n/export before.json\n/source commands.hark\n/export after.json\n/status\n/quit\n'))
        assert 'model: startup' in p.stdout and 'model: included' in p.stdout
        assert 'approval: model' in p.stdout and 'approval: allow' in p.stdout
        before = json.loads((root / 'before.json').read_text())
        after = json.loads((root / 'after.json').read_text())
        assert before['history'] == after['history']
        assert before['history'][-1]['content'] == (root / 'commands.hark').read_text()
        (root / 'recursive.hark').write_text('source recursive.hark\n')
        p = invoke('--no-rc', '--include', 'recursive.hark', text='/quit\n')
        assert p.returncode == 1 and 'too many nested evaluations' in p.stderr
        assert not (root / '.hark').exists()


def test_concurrent_saves():
    with tempfile.TemporaryDirectory(prefix='hark-rc-race-') as temp:
        root = pathlib.Path(temp)
        rc = startup(root)
        rc.write_text('config model initial\n')
        before = rc.read_bytes()
        env = environment(root)
        first = subprocess.Popen([BINARY], cwd=root, env=env, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        second = subprocess.Popen([BINARY], cwd=root, env=env, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            # A hand edit while sessions run is never overwritten; sessions save only where asked.
            rc.write_text('config model edited\n'); edited = rc.read_bytes()
            out, err = first.communicate('/config model first\n/reset\n/save first.hark\n/quit\n', timeout=10)
            assert first.returncode == 0, (out, err)
            out, err = second.communicate('/config model second\n/save second.hark\n/quit\n', timeout=10)
            assert second.returncode == 0, (out, err)
            assert rc.read_bytes() == edited != before
            assert rc_state(root / 'first.hark')['model'] == 'first' and rc_state(root / 'second.hark')['model'] == 'second'
        finally:
            for p in (first, second):
                if p.poll() is None:
                    p.kill(); p.wait()


def test_network(chat=False):
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.requests, server.errors = [], []
    def handler(path, body, headers):
        assert path == ('/v1/chat/completions' if chat else '/v1/responses')
        assert headers['Authorization'] == 'Bearer secret-not-saved'
        h = body['messages' if chat else 'input']
        users = [v['content'] for v in h if v.get('role') == 'user']
        assert 'Background context\nsecond line' in users
        if users[-1] == 'first task\nmultiline prompt':
            if h[-1].get('role') != 'tool' and h[-1].get('type') != 'function_call_output':
                r = response(calls=[('first-call', '{"command":"printf x >> executions"}')], chat=chat)
                if chat:
                    r['choices'][0]['message']['reasoning_content'] = 'old-private-reasoning'
                else:
                    r['output'].insert(0, {'type': 'reasoning', 'encrypted_content': 'old-private-reasoning', 'summary': []})
                return 200, r, False
            return 200, response('first answer', chat=chat), False
        assert 'first answer' in json.dumps(h) and 'old-private-reasoning' not in json.dumps(h)
        assert body['model'] == 'model-b'
        return 200, response('continued', chat=chat), False
    server.handler = handler
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='hark-rc-net-') as temp:
            root = pathlib.Path(temp)
            env = environment(root)
            def invoke(*args, text=''):
                return subprocess.run([BINARY, *args], cwd=root, env=env, input=text, text=True, capture_output=True, timeout=15)
            rc = startup(root)
            rc.write_text(rc_script(dict(base_url=f'http://127.0.0.1:{server.server_port}/v1', api='chat' if chat else 'responses', model='model-a', key_env='TEST_KEY', approval='allow')) + 'import notes.txt\nprompt-file task.txt\n')
            (root / 'notes.txt').write_text('Background context\nsecond line')
            (root / 'task.txt').write_text('first task\nmultiline prompt')
            before = rc.read_bytes()
            session = root / 'session.hark'
            good(invoke(text='/save session.hark\n/quit\n'))
            first = rc_state(session)
            assert first['latest_user'] == 'first task\nmultiline prompt'
            assert (root / 'executions').read_text() == 'x' and len(server.requests) == 2
            assert 'secret-not-saved' not in session.read_text()
            # Resume a saved session by including it; .harkrc would rerun its startup prompt.
            def resume(*args, text=''):
                return invoke('--no-rc', '--include', 'session.hark', *args, text=text)
            # Loading saved context never submits a request or replays tools.
            good(resume(text='/status\n/quit\n'))
            assert len(server.requests) == 2 and (root / 'executions').read_text() == 'x'
            good(resume(text='/config model model-b\ncontinue task\n/save session.hark\n/quit\n'))
            continued = rc_state(session)
            assert continued['model'] == 'model-b' and 'old-private-reasoning' not in json.dumps(continued)
            good(resume(text='/export branch.json\n/reset\n/save session.hark\n/quit\n'))
            assert rc_state(session)['history'] == []
            # Import only context from a saved file. Approval and model remain controlled by the session.
            branch = root / 'branch.json'
            data = json.loads(branch.read_text()); data.update(approval='deny', model='foreign')
            branch.write_text(json.dumps(data))
            good(resume(text='/import branch.json\n/status\n/save session.hark\n/quit\n'))
            imported = rc_state(session)
            assert imported['approval'] == 'allow' and imported['model'] == 'model-b'
            assert imported['history'] and (root / 'executions').read_text() == 'x'
            p = resume(text='/import branch.json\n/save session.hark\n/quit\n')
            assert p.returncode == 1 and 'duplicate tool IDs' in p.stderr
            assert rc_state(session)['history'] == imported['history']
            # Slash-prefixed file text is submitted literally, not dispatched as a command.
            (root / 'literal prompt.txt').write_text('/reset\nPlease continue')
            good(resume(text='/prompt-file {literal prompt.txt}\n/save session.hark\n/quit\n'))
            assert rc_state(session)['latest_user'] == '/reset\nPlease continue'
            assert len(server.requests) == 4 and (root / 'executions').read_text() == 'x'
            (root / 'commands.hark').write_bytes(b'config model model-b\r\nprompt {continue task}')
            good(resume('--include', 'commands.hark', text='/quit\n'))
            assert len(server.requests) == 5
            (root / 'next.txt').write_text('continue task')
            good(resume('--prompt-file', 'next.txt'))
            assert len(server.requests) == 6
            assert rc.read_bytes() == before, 'harkrc is never written'
        assert not server.errors, server.errors
    finally:
        server.shutdown(); server.server_close(); thread.join()


def test_tcl():
    with tempfile.TemporaryDirectory(prefix='hark-tcl-') as temp:
        root = pathlib.Path(temp)
        env = environment(root)
        def invoke(*args, text=''):
            return subprocess.run([BINARY, '--no-session', *args], cwd=root, env=env,
                                  input=text, text=True, capture_output=True, timeout=10)
        (root / 'nested.tcl').write_text('config thinking high\n')
        startup(root).write_text('''
set chosen model-one
proc select_model {name} {set ::hark(model) $name}
select_model $chosen
source nested.tcl
array set hark {speed fast approval deny}
''')
        script = r'''
/config model model-two
/set hark(model) model-three
/if {[config model] ne "model-three"} {error {config did not read the native variable}}
/if {[config thinking] ne "high"} {error {config did not read the startup setting}}
/if {[config http_timeout] != 300} {error {config did not read the numeric default}}
/if {[config stream] ne "true"} {error {config did not read the boolean default}}
/if {[config review_model] ne {}} {error {config did not read an empty override}}
/config base_url http://127.0.0.1:8080/v1
/config base_url {}
/if {[config base_url] ne "https://api.openai.com/v1"} {error {config did not reset base_url}}
/set hark(base_url) http://127.0.0.1:8080/v1
/set hark(base_url) {}
/if {[config base_url] ne "https://api.openai.com/v1"} {error {native set did not reset base_url}}
/if {![catch {config base_url ftp://example.org}]} {error {invalid URL accepted}}
/if {[config base_url] ne "https://api.openai.com/v1"} {error {invalid URL changed base_url}}
/config thinking
/config policy {λ😀 $literal [error should-not-run]; text}
/lappend hark(allow) {git status --short}
/if {[config allow] ne {{git status --short}}} {error {config did not read the list}}
/if {![catch {set hark(http_timeout) 0} message]} {error {invalid write accepted}}
/puts $message
/if {$hark(http_timeout) != 300} {error {invalid write changed the variable}}
/if {![catch {config typo 1}]} {error {unknown setting accepted}}
/if {![catch {config typo}]} {error {unknown setting read accepted}}
/if {[info exists hark(typo)]} {error {unknown setting retained}}
/unset hark
/set hark(model) model-four
/if {![catch {set hark(speed) invalid}]} {error {trace lost after unset}}
/save state.tcl
/status
/quit
'''
        p = good(invoke(text=script.lstrip('\n')))
        assert 'model: model-four' in p.stdout and 'thinking: high' in p.stdout, (p.stdout, p.stderr)
        assert 'whole number from 1 to 86400' in p.stdout
        assert 'high' in p.stdout.splitlines(), p.stdout
        saved = rc_state(root / 'state.tcl')
        assert saved['model'] == 'model-four' and saved['speed'] == 'fast'
        assert saved['allow'] == ['git status --short']
        assert saved['policy'] == 'λ😀 $literal [error should-not-run]; text'
        assert not saved['history'] and not (root / '.local').exists()
        p = good(invoke('--no-rc', text='/config review_model pinned\n/source state.tcl\n/save reloaded.tcl\n/quit\n'))
        assert rc_state(root / 'reloaded.tcl') == saved
        # Missing arguments and uncaught script errors stop files before later commands.
        (root / 'bad.tcl').write_text('config model extra extra\nset f [open should-not-exist w]\n')
        p = invoke('--no-rc', '--include', 'bad.tcl')
        assert p.returncode == 1 and 'wrong # args' in p.stderr and not (root / 'should-not-exist').exists()
        # Values with embedded NUL cannot cross the C string boundary.
        p = invoke('--no-rc', text='/config policy "a\\u0000b"\n/prompt "a\\u0000b"\n/quit\n')
        assert p.returncode == 1 and 'NUL' in p.stderr, p.stderr


if __name__ == '__main__':
    test_tcl()
    print('Tcl command tests passed')
    test_local()
    print('harkrc configuration tests passed')
    test_per_user_rc()
    print('per-user harkrc tests passed')
    test_resume()
    print('resume tests passed')
    test_instructions()
    print('instruction file tests passed')
    test_bad_settings()
    print('bad setting tests passed')
    test_include_import()
    print('include/import distinction tests passed')
    test_concurrent_saves()
    print('concurrent session tests passed')
    for chat in (False, True):
        test_network(chat)
        print('context/session tests passed:', 'chat' if chat else 'responses')

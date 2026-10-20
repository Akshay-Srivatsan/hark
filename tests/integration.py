#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local HTTP integration tests. Python is a test-only optional dependency."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BINARY = str(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else './hark').resolve())


def response(text=None, calls=None, chat=False):
    if chat:
        m = {'role': 'assistant', 'content': text}
        if calls:
            m['tool_calls'] = [{'id': cid, 'type': 'function', 'function': {'name': 'sh', 'arguments': args}} for cid, args in calls]
        return {'choices': [{'message': m, 'finish_reason': 'tool_calls' if calls else 'stop'}]}
    out = []
    if text is not None:
        out.append({'type': 'message', 'role': 'assistant', 'content': [{'type': 'output_text', 'text': text}]})
    for cid, args in calls or []:
        out.append({'type': 'function_call', 'call_id': cid, 'name': 'sh', 'arguments': args})
    return {'status': 'completed', 'output': out, 'error': None}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.respond(body)

    def do_GET(self):
        assert not self.headers.get('Content-Length')
        self.respond(None)

    def respond(self, body):
        try:
            self.server.requests.append((self.path, body, dict(self.headers)))
            status, payload, sse = self.server.handler(self.path, body, dict(self.headers))
        except Exception as exc:
            self.server.errors.append(exc)
            status, payload, sse = 500, {'error': {'message': str(exc)}}, False
        raw = json.dumps(payload).encode()
        if sse in ('items', 'stream-failed', 'stream-truncated', 'stream-incomplete'):
            events = [{'type': 'response.output_item.done', 'output_index': i, 'item': item} for i, item in enumerate(payload['output'])]
            if sse == 'items':
                events.append({'type': 'response.completed', 'response': {'id': 'stream-test', 'status': 'completed'}})
            elif sse != 'stream-truncated':
                events.append({'type': 'response.failed' if sse == 'stream-failed' else 'response.incomplete', 'response': {'id': 'stream-test'}})
            raw = b': comment\r\n' + b''.join(b'event: ' + e['type'].encode() + b'\r\n' + b''.join(b'data: ' + line.encode() + b'\r\n' for line in json.dumps(e, indent=2).splitlines()) + b'\r\n' for e in events)
        elif sse:
            raw = b'event: response.completed\r\ndata: ' + json.dumps({'type': 'response.completed', 'response': payload}).encode() + b'\r\n\r\n'
        self.send_response(status)
        self.send_header('Content-Type', 'text/event-stream' if sse else 'application/json')
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


def run(handler, options=(), prompt='Create the requested file', stdin='', env=None, include_model=True, subcommand=None):
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.handler, server.requests, server.errors = handler, [], []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='hark-test-') as tmp:
            args = [BINARY, *([subcommand] if subcommand else []), '--base-url', f'http://127.0.0.1:{server.server_port}/v1', *(['--model', 'main'] if include_model else []), '--cwd', tmp, *options]
            if prompt is not None:
                args += ['--', prompt]
            clean_env = {k: v for k, v in os.environ.items() if not k.startswith('XDG_') and k not in ('HARK_MODEL', 'HARK_API', 'HARK_API_KEY', 'HARK_BASE_URL', 'OPENAI_API_KEY', 'HARK_SPEED', 'HARK_THINKING', 'HARK_CODEX_CLIENT_VERSION')}
            clean_env['HOME'] = clean_env['USERPROFILE'] = tmp
            clean_env.update(env or {})
            p = subprocess.run(args, input=stdin, text=True, capture_output=True, timeout=20, env=clean_env)
            files = {f.name: f.read_text() for f in pathlib.Path(tmp).iterdir() if f.is_file()}
        assert not server.errors, server.errors
        return p, server.requests, files
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def tool_round(chat=False, command="printf 'done' > result", expected='ok', sse=False):
    n = 0
    def handle(path, body, headers):
        nonlocal n
        assert path == ('/v1/chat/completions' if chat else '/v1/responses')
        history = body['messages' if chat else 'input']
        n += 1
        if n == 1:
            assert body['tools'][0]['type'] == 'function'
            # Some servers require parallel calls; Hark runs a validated batch in order.
            assert 'parallel_tool_calls' not in body
            assert body['tools'][0].get('function', body['tools'][0])['name'] == 'sh'
            return 200, response(calls=[('call1', json.dumps({'command': command}))], chat=chat), sse
        result = json.loads(history[-1]['content' if chat else 'output'])
        if expected == 'ok':
            assert result['exit_status'] == 0
        elif expected == 'denied':
            assert result['denied'] is True
        else:
            assert result['error']
        return 200, response('finished', chat=chat), sse
    return handle


def test_tools():
    for chat in (False, True):
        p, req, files = run(tool_round(chat), ['--allow', "printf 'done' > result", '--api', 'chat' if chat else 'responses'])
        assert p.returncode == 0 and files['result'] == 'done', p.stderr
        assert len(req) == 2
    p, _, files = run(tool_round(expected='denied'), ['--approval', 'ask'])
    assert p.returncode == 0 and not files
    p, _, files = run(tool_round(expected='denied'), ['--allow', '*', '--deny', '*result*'])
    assert p.returncode == 0 and not files
    # Unconditional allow skips patterns, terminal prompts and model review.
    p, _, files = run(tool_round(), ['--approval', 'allow', '--deny', '*'])
    assert p.returncode == 0 and files['result'] == 'done', p.stderr
    p, _, files = run(tool_round(sse=True), ['--allow', '*'])
    assert p.returncode == 0 and files['result'] == 'done', p.stderr
    # Codex commonly sends completed items before a metadata-only completion.
    p, _, files = run(tool_round(sse='items'), ['--allow', '*'])
    assert p.returncode == 0 and files['result'] == 'done' and 'finished' in p.stdout, p.stderr
    # Completed items are never executed unless the entire response succeeds.
    for mode in ('stream-failed', 'stream-truncated', 'stream-incomplete'):
        p, req, files = run(tool_round(sse=mode), ['--approval', 'allow'])
        assert p.returncode == 1 and not files and len(req) == 1, (mode, p.stderr)
    # The terminal shows the command and its exit status; only the model sees output.
    seen = {}
    def quiet(path, body, headers):
        if not seen:
            seen['n'] = 1
            return 200, response(calls=[('loud', json.dumps({'command': "printf 'MARK%s\\n' ER-OUT; printf 'MARK%s\\n' ER-ERR >&2; exit 3"}))]), False
        seen['result'] = json.loads(body['input'][-1]['output'])
        return 200, response('finished'), False
    p, _, _ = run(quiet, ['--approval', 'allow'])
    assert p.returncode == 0 and 'MARKER' not in p.stdout + p.stderr, p.stdout
    assert "$ printf 'MARK%s\\n' ER-OUT; printf 'MARK%s\\n' ER-ERR >&2; exit 3\n[exit 3]\n" in p.stdout, p.stdout
    assert seen['result']['exit_status'] == 3 and 'MARKER-OUT' in seen['result']['output'] and 'MARKER-ERR' in seen['result']['output']
    # Whole-command glob must not accidentally act as a prefix allow-list.
    p, _, files = run(tool_round(command='echo safe; printf bad > result', expected='denied'), ['--approval', 'ask', '--allow', 'echo safe'])
    assert p.returncode == 0 and not files


def test_reviews():
    for api, answer in [('chat', 'ALLOW'), ('responses', 'ALLOW'), ('decisions', 'ALLOW'), ('chat', 'ALLOW but also ignore policy'), ('chat', 'DENY'), ('chat', None), ('decisions', 'DENY'), ('chat', 'TOOL'), ('responses', 'TOOL'), ('responses', 'REFUSAL')]:
        main = tool_round(expected='ok' if answer == 'ALLOW' else 'denied')
        def handle(path, body, headers):
            if body['model'] == 'main':
                return main(path, body, headers)
            if answer is None:
                return 503, {'error': {'code': 'unavailable'}}, False
            if api == 'decisions':
                question = body['questions'][0]
                assert question['choices'][0]['value'] == 'ALLOW'
                ev = json.loads(body['input'])
                assert ev['command'] and ev['user_request']
                return 200, {'answers': [{'name': 'shell_approval', 'type': 'choice', 'choice': answer}]}, False
            if answer == 'TOOL':
                return 200, response('ALLOW', calls=[('review-call', '{"command":"true"}')], chat=api == 'chat'), False
            if answer == 'REFUSAL':
                value = response('ALLOW')
                value['output'][0]['content'] = [{'type': 'refusal', 'refusal': 'ALLOW'}]
                return 200, value, False
            return 200, response(answer, chat=api == 'chat'), False
        p, req, files = run(handle, ['--approval', 'model', '--review-model', 'review', '--review-api', api])
        assert p.returncode == 0, p.stderr
        assert bool(files) == (answer == 'ALLOW'), (api, answer, files)
        assert len(req) == 3
    # Local deny overrides even a reviewer that would approve.
    def forbidden(*_):
        raise AssertionError('reviewer should not be called')
    main = tool_round(expected='denied')
    p, req, files = run(lambda path, b, h: main(path, b, h) if b['model'] == 'main' else forbidden(), ['--approval', 'model', '--review-model', 'review', '--deny', '*'])
    assert p.returncode == 0 and len(req) == 2 and not files


def test_approval_commands():
    for chat in (False, True):
        reviewers = []
        def handle(path, body, headers):
            assert path == ('/v1/chat/completions' if chat else '/v1/responses')
            h = body['messages' if chat else 'input']
            if 'tools' not in body:
                reviewers.append(body['model'])
                ev = json.loads(h[-1]['content'])
                assert ev['command'] == 'printf x >> result' and ev['user_request']
                assert 'service_tier' not in body and 'reasoning_effort' not in body and 'reasoning' not in body
                return 200, response('ALLOW', chat=chat), False
            last = h[-1]
            if last.get('role') == 'tool' or last.get('type') == 'function_call_output':
                result = json.loads(last['content' if chat else 'output'])
                task = [v['content'] for v in h if v.get('role') == 'user'][-1]
                assert bool(result.get('denied')) == (task in ('deny task', 'ask task'))
                return 200, response('finished', chat=chat), False
            return 200, response(calls=[('call-' + str(len(h)), '{"command":"printf x >> result"}')], chat=chat), False
        commands = ('/approval\nfirst task\n/model alternate\nsecond task\n'
                    '/review-model pinned\nthird task\n/model third\nfourth task\n'
                    '/review-model default\nfifth task\n/approval deny\ndeny task\n'
                    '/approval allow\nallow task\n/approval ask\nask task\n'
                    '/approval model\nlast task\n/approval invalid\n/review-model invalid id\n/status\n/quit\n')
        p, req, files = run(handle, ['--api', 'chat' if chat else 'responses', '--speed', 'fast', '--thinking', 'high'], prompt=None, stdin=commands)
        assert p.returncode == 0 and files['result'] == 'xxxxxxx', (p.stdout, p.stderr, files)
        assert reviewers == ['main', 'alternate', 'pinned', 'pinned', 'third', 'third'], reviewers
        assert 'approval: model' in p.stdout and 'review-model: third' in p.stdout
        assert 'invalid approval mode' in p.stderr and 'invalid reviewer model ID' in p.stderr
    # CLI reset also follows the agent, rather than requesting a model named default.
    main = tool_round()
    p, _, files = run(lambda path, b, h: main(path, b, h) if 'tools' in b else (200, response('ALLOW'), False), ['--review-model', 'default'])
    assert p.returncode == 0 and files['result'] == 'done', p.stderr


def test_compact():
    for chat in (False, True):
        n = 0
        def handle(path, body, headers):
            nonlocal n
            history = body['messages' if chat else 'input']
            if 'tools' not in body:
                assert 'Summarize' in (history[0]['content'] if chat else body['instructions'])
                assert 'payload' in json.dumps(history)
                return 200, response('Task still pending; inspection completed.', chat=chat), False
            n += 1
            if n == 1:
                return 200, response(calls=[('call1', json.dumps({'command': "printf 'payload%.0s' 1 2 3 4 5 6 7 8 9; printf '%08000d' 0"}))], chat=chat), False
            assert history[-1]['content'] == 'Keep this user request exactly'
            assert 'Task still pending' in json.dumps(history)
            assert 'tool_calls' not in json.dumps(history)
            return 200, response('done', chat=chat), False
        p, req, _ = run(handle, ['--allow', '*', '--compact-bytes', '2000', '--api', 'chat' if chat else 'responses'], prompt='Keep this user request exactly')
        assert p.returncode == 0 and len(req) == 3, (p.stderr, p.stdout)
    # Refuse a summary that makes the transcript larger.
    n = 0
    def no_shrink(path, body, headers):
        nonlocal n
        n += 1
        return 200, response('x' * 5000), False
    p, req, _ = run(no_shrink, ['--compact-bytes', '10'])
    assert p.returncode == 1 and len(req) == 1 and 'original retained' in p.stderr


def test_invalid_batches():
    for bad in ('duplicate', 'missing', 'malformed', 'incomplete', 'badargs'):
        n = 0
        def handle(path, body, headers):
            nonlocal n
            n += 1
            if n > 1:
                assert json.loads(body['input'][-1]['output'])['error']
                return 200, response('done'), False
            r = response(calls=[('id', json.dumps({'command': 'printf bad > result'}))])
            if bad == 'duplicate':
                r['output'] += r['output'][:]
            elif bad == 'missing':
                del r['output'][0]['call_id']
            elif bad == 'malformed':
                return 200, {'hello': 'world'}, False
            elif bad == 'incomplete':
                r['status'] = 'incomplete'
            else:
                r['output'][0]['arguments'] = '{"command":"printf bad > result","command":"true"}'
            return 200, r, False
        p, req, files = run(handle, ['--allow', '*'])
        assert not files
        assert p.returncode == (0 if bad == 'badargs' else 1), (bad, p.stderr)


def test_context_retry():
    n = 0
    def handle(path, body, headers):
        nonlocal n
        n += 1
        if n == 1:
            return 200, response('Prior result ' * 100), False
        if n == 2:
            return 400, {'error': {'code': 'context_length_exceeded'}}, False
        if n == 3:
            assert 'tools' not in body
            return 200, response('Prior result retained.'), False
        assert body['input'][-1]['content'] == 'second request'
        return 200, response('recovered'), False
    p, req, _ = run(handle, prompt=None, stdin='first request\nsecond request\n')
    assert p.returncode == 0 and len(req) == 4 and 'recovered' in p.stdout, p.stderr


def test_no_retry_side_effects():
    def handle(path, body, headers):
        return 503, {'error': {'code': 'unavailable'}}, False
    p, req, files = run(handle)
    assert p.returncode == 1 and len(req) == 1 and not files


def test_isolation_and_limit():
    main = tool_round()
    p, req, files = run(main, ['--allow', '*', '--max-steps', '1'])
    assert p.returncode == 1 and len(req) == 1 and files['result'] == 'done'
    p, req, _ = run(lambda path, b, h: (200, response('hi'), False), env={'HARK_API_KEY': 'secret-test-key'})
    assert p.returncode == 0 and req[0][2]['Authorization'] == 'Bearer secret-test-key'


def test_replay_rejected():
    for chat in (False, True):
        def handle(path, body, headers):
            return 200, response(calls=[('repeated', '{"command":"printf x >> result"}')], chat=chat), False
        p, req, files = run(handle, ['--allow', '*', '--api', 'chat' if chat else 'responses'])
        assert p.returncode == 1 and len(req) == 2 and files['result'] == 'x'


def test_models():
    for chat in (False, True):
        def handle(path, body, headers):
            assert path == '/v1/models' and body is None
            assert headers['Authorization'] == 'Bearer models-test-key'
            return 200, {'data': [{'id': 'z-model'}, {'id': 'a-model'}, {'id': 'z-model'}]}, False
        p, req, _ = run(handle, ['--api', 'chat' if chat else 'responses'], prompt=None, include_model=False, subcommand='models', env={'HARK_API_KEY': 'models-test-key'})
        assert p.returncode == 0 and p.stdout == 'a-model\nz-model\n' and len(req) == 1, p.stderr
    # The subcommand also works after provider options, still without a model.
    p, req, _ = run(lambda *_: (200, {'data': [{'id': 'test'}]}, False), ['models'], prompt=None, include_model=False)
    assert p.returncode == 0 and p.stdout == 'test\n' and len(req) == 1
    for payload, code in [({'data': []}, 0), ({'models': []}, 1), ({'data': {}}, 1), ({'data': [{'id': 'ok'}, {'name': 'bad'}]}, 1), ({'data': [{'id': 'bad\nID'}]}, 1), ({'error': {'code': 'not_supported'}}, 1)]:
        p, req, _ = run(lambda *_: (200, payload, False), ['--list-models'], prompt=None, include_model=False)
        assert p.returncode == code and p.stdout == '' and len(req) == 1, (payload, p.stderr)
    p, req, _ = run(lambda *_: (404, {}, False), ['--list-models'], prompt=None, include_model=False)
    assert p.returncode == 1 and len(req) == 1
    # No selected model is needed to use local commands or query the catalog.
    p, req, _ = run(lambda *_: (200, {'data': [{'id': 'local-model'}]}, False), prompt=None, include_model=False, stdin='/models\n/status\n/quit\n')
    assert p.returncode == 0 and len(req) == 1 and 'local-model' in p.stdout and '(unset)' in p.stdout


def test_settings():
    for chat in (False, True):
        n = 0
        def handle(path, body, headers):
            nonlocal n
            n += 1
            assert body['model'] == ('main' if n == 1 else 'alternate')
            h = body['messages' if chat else 'input']
            if n == 1:
                assert body['service_tier'] == 'priority'
                assert (body['reasoning_effort'] if chat else body['reasoning']['effort']) == 'high'
                r = response('first answer', chat=chat)
                if chat:
                    r['choices'][0]['message']['reasoning_content'] = 'model-specific-private-data'
                else:
                    r['output'].insert(0, {'type': 'reasoning', 'encrypted_content': 'model-specific-private-data', 'summary': []})
                return 200, r, False
            assert 'first answer' in json.dumps(h) and 'first request' in json.dumps(h)
            assert 'model-specific-private-data' not in json.dumps(h)
            if n == 2:
                assert body['service_tier'] == 'default'
                assert (body['reasoning_effort'] if chat else body['reasoning']['effort']) == 'none'
            else:
                assert 'service_tier' not in body and 'reasoning_effort' not in body and 'reasoning' not in body
            return 200, response('done', chat=chat), False
        commands = '/model main\n/speed fast\n/thinking high\nfirst request\n/model alternate\n/speed standard\n/thinking none\nsecond request\n/speed auto\n/thinking default\nthird request\n/status\n/quit\n'
        p, req, _ = run(handle, ['--api', 'chat' if chat else 'responses'], prompt=None, include_model=False, stdin=commands)
        assert p.returncode == 0 and len(req) == 3, p.stderr
        assert 'model: alternate' in p.stdout and 'speed: auto' in p.stdout and 'thinking: default' in p.stdout
    # Environment defaults and flags use the same wire fields, without affecting review.
    main = tool_round()
    def handle(path, body, headers):
        if body['model'] == 'main':
            assert body['service_tier'] == 'flex' and body['reasoning']['effort'] == 'max'
            return main(path, body, headers)
        assert 'service_tier' not in body and 'reasoning' not in body and 'reasoning_effort' not in body
        return 200, response('ALLOW'), False
    p, _, files = run(handle, ['--approval', 'model', '--review-model', 'review'], env={'HARK_SPEED': 'flex', 'HARK_THINKING': 'max'})
    assert p.returncode == 0 and files['result'] == 'done'
    p, _, _ = run(lambda path, body, headers: (200, response('ok') if body['service_tier'] == 'ultrafast' and body['reasoning']['effort'] == 'low' else {}, False), ['--speed', 'ultrafast', '--thinking', 'low'])
    assert p.returncode == 0, p.stderr
    p, req, _ = run(lambda *_: (_ for _ in ()).throw(AssertionError('must not request')), prompt=None, stdin='/speed invalid\n/thinking invalid\n/model invalid id\n/status\n/quit\n')
    assert p.returncode == 0 and not req and 'speed: auto' in p.stdout and 'thinking: default' in p.stdout and 'model: main' in p.stdout


def test_provider_errors():
    key = 'sk-test-secret-value'
    for chat in (False, True):
        def handle(path, body, headers):
            message = 'parallel_tool_calls must be true \x1b[31mred\x1b[0m; key ' + key + ' rejected'
            return 400, {'error': {'message': message, 'type': 'invalid_request_error'}, 'trace': 'BODY-ONLY'}, False
        p, req, _ = run(handle, ['--api', 'chat'] if chat else [], env={'OPENAI_API_KEY': key, 'HARK_API_KEY': key})
        assert p.returncode == 1 and len(req) == 1, p.stderr
        assert 'model request failed (HTTP 400): parallel_tool_calls must be true' in p.stderr, p.stderr
        assert '\x1b' not in p.stderr and '\\x1b[31mred' in p.stderr, 'control characters are escaped'
        assert key not in p.stderr and 'key [redacted] rejected' in p.stderr
        assert 'BODY-ONLY' not in p.stderr, 'only the error message is shown'
    # Other error shapes, long messages, and model listing.
    def detail(path, body, headers):
        return 422, {'detail': 'x' * 1000}, False
    p, _, _ = run(detail, prompt=None, subcommand='models', include_model=False)
    assert p.returncode == 1 and 'provider request failed (HTTP 422): ' + 'x' * 400 + '...' in p.stderr and 'x' * 401 not in p.stderr, p.stderr


def test_device_auth():
    import base64
    import shlex
    import urllib.parse
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.requests, server.errors = [], []
    origin = f'http://127.0.0.1:{server.server_port}'
    payload = base64.urlsafe_b64encode(json.dumps({'https://api.openai.com/auth': {'chatgpt_account_id': 'test-account'}}).encode()).decode().rstrip('=')
    jwt = 'e30.' + payload + '.sig'
    polls, exchanges, refreshes, inference, model_requests = 0, 0, 0, 0, 0
    current_token = jwt
    def handle(path, body, headers):
        nonlocal polls, exchanges, refreshes, inference, model_requests, current_token
        if path.endswith('/usercode'):
            assert body['client_id']
            return 200, {'user_code': 'ABCD-TEST', 'device_auth_id': 'device', 'interval': '1'}, False
        if path.endswith('/deviceauth/token'):
            polls += 1
            assert body == {'device_auth_id': 'device', 'user_code': 'ABCD-TEST'}
            if polls == 1:
                return 403, {}, False
            return 200, {'authorization_code': 'code &+=', 'code_verifier': 'verifier &+=', 'code_challenge': 'challenge'}, False
        if path == '/oauth/token':
            assert headers['Content-Type'] == 'application/x-www-form-urlencoded'
            if body['grant_type'] == ['authorization_code']:
                exchanges += 1
                assert body['code'] == ['code &+='] and body['code_verifier'] == ['verifier &+=']
                assert body['redirect_uri'] == [origin + '/deviceauth/callback']
                return 200, {'access_token': jwt, 'id_token': jwt, 'refresh_token': 'refresh1', 'expires_in': 3600}, False
            refreshes += 1
            assert body['grant_type'] == ['refresh_token'] and body['refresh_token'] == ['refresh' + str(refreshes)]
            current_token = 'refreshed' + str(refreshes)
            return 200, {'access_token': current_token, 'refresh_token': 'refresh' + str(refreshes + 1), 'expires_in': 3600}, False
        if path.startswith('/backend/models?'):
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(path).query)
            version = query['client_version'][0]
            assert body is None
            # Reproduce the backend's minimum-version filtering: Hark's own
            # release number must not be used as a Codex compatibility version.
            if tuple(map(int, version.split('.'))) < (0, 99, 0):
                return 200, {'models': []}, False
            assert version == '0.163.0'
            model_requests += 1
            assert headers['Authorization'] == 'Bearer ' + current_token
            assert headers['ChatGPT-Account-Id'] == 'test-account'
            if model_requests == 1:
                return 401, {'error': {'code': 'expired'}}, False
            return 200, {'models': [{'slug': 'codex-test'}]}, False
        if path == '/v1/models':
            assert 'ChatGPT-Account-Id' not in headers
            if headers.get('Authorization') != 'Bearer api-good':
                return 401, {'error': {'code': 'invalid_api_key', 'message': 'Never echo stale-secret'}}, False
            return 200, {'data': [{'id': 'api-test'}]}, False
        if path == '/custom/models':
            assert 'Authorization' not in headers and 'ChatGPT-Account-Id' not in headers
            return 200, {'data': [{'id': 'local-test'}]}, False
        assert path == '/backend/responses'
        assert body['stream'] is True and body['store'] is False
        assert headers['ChatGPT-Account-Id'] == 'test-account'
        inference += 1
        if inference == 1:
            assert headers['Authorization'] == 'Bearer ' + current_token
            return 401, {'error': {'code': 'expired'}}, False
        assert headers['Authorization'] == 'Bearer ' + current_token
        return 200, response('authenticated'), True
    server.handler = handle
    # OAuth exchange bodies are form-encoded, unlike inference JSON.
    original = Handler.do_POST
    def oauth_post(self):
        if self.path == '/oauth/token':
            data = urllib.parse.parse_qs(self.rfile.read(int(self.headers['Content-Length'])).decode())
            try:
                code, value, _ = handle(self.path, data, dict(self.headers))
            except Exception as exc:
                server.errors.append(exc)
                code, value = 500, {}
            raw = json.dumps(value).encode()
            self.send_response(code)
            self.send_header('Content-Length', str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        else:
            original(self)
    Handler.do_POST = oauth_post
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='hark-auth-test-') as tmp:
            root = pathlib.Path(__file__).resolve().parent.parent
            binary = str(pathlib.Path(tmp) / 'hark-auth-test')
            compiler = shlex.split(os.environ.get('CC', 'cc'))
            cmd = [*compiler, '-std=c99', '-O0', '-DHARK_AUTH_ISSUER="' + origin + '"', '-DHARK_CHATGPT_BASE="' + origin + '/backend"', '-DHARK_OPENAI_BASE="' + origin + '/v1"', '-o', binary, 'hark.c', 'auth.c', 'json.c', 'net.c', 'platform.c', 'line.c', 'store.c', 'session.c', 'yyjson.o', 'isocline.o', '-lcurl']
            subprocess.run(cmd, cwd=root, check=True, env={**os.environ, 'CCACHE_DISABLE': '1'}, capture_output=True)
            auth = pathlib.Path(tmp) / 'credentials.json'
            clean_env = {k: v for k, v in os.environ.items() if not k.startswith('XDG_') and k not in ('HARK_MODEL', 'HARK_API', 'HARK_API_KEY', 'HARK_BASE_URL', 'OPENAI_API_KEY', 'HARK_SPEED', 'HARK_THINKING', 'HARK_CODEX_CLIENT_VERSION')}
            clean_env['HOME'] = clean_env['USERPROFILE'] = tmp
            def invoke(*args, env=None, cwd=None):
                return subprocess.run([binary, *args], cwd=cwd or tmp, env={**clean_env, **(env or {})}, text=True, capture_output=True, timeout=10)
            p = invoke('models', '--auth-file', str(auth))
            assert p.returncode == 1 and 'OpenAI credentials missing' in p.stderr
            assert not server.requests
            p = invoke('login', '--auth-file', str(auth))
            assert p.returncode == 0 and 'ABCD-TEST' in p.stdout, p.stderr
            assert json.loads(auth.read_text())['account_id'] == 'test-account'
            p = invoke('models', '--chatgpt', '--auth-file', str(auth), '--codex-client-version', '0.1.0')
            assert p.returncode == 1 and 'empty model catalog' in p.stderr and 'client_version=0.1.0' in p.stderr
            p = invoke('models', '--chatgpt', '--auth-file', str(auth))
            assert p.returncode == 0 and p.stdout == 'codex-test\n', p.stderr
            # Plain models now reuses the saved login, including its rotated token.
            p = invoke('models', '--auth-file', str(auth))
            assert p.returncode == 0 and p.stdout == 'codex-test\n' and 'using saved ChatGPT login' in p.stderr, p.stderr
            # A configured API key takes priority; trailing URL slashes must not drop it.
            p = invoke('models', '--base-url', origin + '/v1///', '--auth-file', str(auth), env={'OPENAI_API_KEY': 'api-good'})
            assert p.returncode == 0 and p.stdout == 'api-test\n', p.stderr
            p = invoke('models', '--auth-file', str(auth), env={'HARK_API_KEY': 'stale-secret', 'OPENAI_API_KEY': 'api-good'})
            assert p.returncode == 1 and 'credentials from HARK_API_KEY' in p.stderr and 'takes precedence' in p.stderr
            assert 'stale-secret' not in p.stdout + p.stderr
            p = invoke('models', '--auth-file', str(auth), '--key-env', 'OPENAI_API_KEY', env={'HARK_API_KEY': 'stale-secret', 'OPENAI_API_KEY': 'api-good'})
            assert p.returncode == 0 and p.stdout == 'api-test\n', p.stderr
            # Saved tokens and OpenAI keys must never be auto-forwarded to a custom provider.
            p = invoke('models', '--base-url', origin + '/custom', '--auth-file', str(auth), env={'OPENAI_API_KEY': 'api-good'})
            assert p.returncode == 0 and p.stdout == 'local-test\n', p.stderr
            sub = pathlib.Path(tmp) / 'subdir'
            sub.mkdir()
            p = invoke('--chatgpt', '--auth-file', 'credentials.json', '--cwd', str(sub), '--model', 'test', 'hello', cwd=tmp)
            assert p.returncode == 0 and 'authenticated' in p.stdout, p.stderr
            assert json.loads(auth.read_text())['refresh_token'] == 'refresh3'
            assert not (sub / 'credentials.json').exists()
            assert polls == 2 and exchanges == 1 and refreshes == 2 and inference == 2 and model_requests == 3
        assert not server.errors, server.errors
    finally:
        Handler.do_POST = original
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == '__main__':
    for test in (test_tools, test_reviews, test_approval_commands, test_compact, test_invalid_batches, test_context_retry, test_no_retry_side_effects, test_isolation_and_limit, test_replay_rejected, test_models, test_settings, test_provider_errors, test_device_auth):
        test()
        print(test.__name__, 'passed')
    print('integration tests passed')

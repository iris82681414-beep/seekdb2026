"""Loopback embedding batch and row-order fixture for mysqltest, using only the stdlib."""
import json
import os
from pathlib import Path
import secrets
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, urlopen

MODE, PORT = sys.argv[1], int(sys.argv[2])
STATE_DIR = Path(tempfile.gettempdir()) / f'seekdb-embed-batch-fixture-{os.getuid()}'
STATE_DIR.mkdir(mode=0o700, exist_ok=True)
directory_info = STATE_DIR.lstat()
if not stat.S_ISDIR(directory_info.st_mode) or directory_info.st_uid != os.getuid() or stat.S_IMODE(directory_info.st_mode) != 0o700:
    raise PermissionError('fixture state directory must be private and owned by the current user')
STATE = STATE_DIR / f'{PORT}.json'
if STATE.is_symlink():
    raise PermissionError('fixture state must not be a symlink')
URL = f'http://127.0.0.1:{PORT}'


def stop():
    if not STATE.exists():
        return
    token = json.loads(STATE.read_text())['token']
    try:
        with urlopen(URL + '/health', timeout=2) as response:
            if json.load(response).get('fixture') != 'seekdb-embed-batch':
                raise RuntimeError('port belongs to another service')
        request = Request(URL + '/shutdown', data=token.encode(), method='POST')
        with urlopen(request, timeout=2):
            pass
    except OSError:
        pass
    STATE.unlink(missing_ok=True)


def serve():
    token = json.loads(STATE.read_text())['token']
    events = []
    attempts = {}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, status, value):
            data = json.dumps(value).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            self.reply(200, {'events': events} if self.path == '/stats' else {'fixture': 'seekdb-embed-batch'})

        def do_POST(self):
            data = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            if self.path == '/shutdown':
                if data.decode() != token:
                    self.reply(403, {})
                    return
                self.reply(200, {})
                threading.Thread(target=server.shutdown, daemon=True).start()
                return
            body = json.loads(data)
            attempts[self.path] = attempts.get(self.path, 0) + 1
            if self.path == '/always-unavailable':
                self.reply(503, {'error': 'temporary failure'})
                return
            if self.path == '/retry-partial' and attempts[self.path] == 1:
                # libcurl receives a prefix and CURLE_PARTIAL_FILE. The next
                # successful response must not be appended to this prefix.
                self.send_response(200)
                self.send_header('Content-Length', '1000')
                self.end_headers()
                self.wfile.write(b'{"data":[')
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR)
                self.close_connection = True
                return
            inputs = body.get('input')
            dimension = body.get('dimensions', 3)
            if not isinstance(inputs, list) or not 1 <= len(inputs) <= 16 or any(not isinstance(x, str) or not x for x in inputs):
                self.reply(400, {'error': 'expected 1..16 nonempty string inputs'})
                return
            if type(dimension) is not int or not 1 <= dimension <= 4:
                self.reply(400, {'error': 'expected positive integer dimension'})
                return
            events.append({'count': len(inputs), 'dimension': dimension,
                           'model': body['model'], 'lengths': [len(x) for x in inputs]})
            if self.path == '/changing-dimension':
                dimension = 3 if int(inputs[0].split(':')[0]) < 18 else 4
            offset = 10000 if body['model'] == 'model_b' else 0
            items = [{'index': i, 'embedding': [float(int(text.split(':')[0]) + offset),
                                               float(dimension), float(len(text)), 0.5][:dimension]}
                     for i, text in reversed(list(enumerate(inputs)))]
            if self.path == '/invalid-batch':
                items[-1]['index'] = len(inputs)
            elif self.path == '/duplicate-index':
                items[-1]['index'] = items[0]['index']
            elif self.path == '/string-element':
                items[0]['embedding'][0] = '1'
            elif self.path == '/null-element':
                items[0]['embedding'][0] = None
            elif self.path == '/mixed-dimensions' and len(items) > 1:
                items[-1]['embedding'].pop()
            self.reply(200, {'data': items})

    server = ThreadingHTTPServer(('127.0.0.1', PORT), Handler)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if MODE == 'assert-batched':
    with urlopen(URL + '/stats', timeout=2) as response:
        events = json.load(response)['events']
    assert any(x['count'] > 1 for x in events), events
    assert all(1 <= x['count'] <= 16 for x in events), events
    assert any(max(x['lengths']) >= 50000 for x in events), events
    assert {x['model'] for x in events} == {'model_a', 'model_b'}, events
    assert {x['dimension'] for x in events} >= {2, 3}, events
elif MODE == 'stop':
    stop()
elif MODE == 'serve':
    serve()
elif MODE == 'start':
    stop()
    STATE.write_text(json.dumps({'token': secrets.token_hex(16)}))
    os.chmod(STATE, 0o600)
    process = subprocess.Popen([sys.executable, __file__, 'serve', str(PORT)],
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, start_new_session=True)
    for _ in range(50):
        try:
            with urlopen(URL + '/health', timeout=0.2) as response:
                assert json.load(response)['fixture'] == 'seekdb-embed-batch'
            break
        except OSError:
            if process.poll() is not None:
                raise RuntimeError('embedding batch fixture exited')
            time.sleep(0.1)
    else:
        process.terminate()
        process.wait(timeout=5)
        raise RuntimeError('embedding batch fixture readiness timeout')
else:
    raise ValueError('expected start, serve or stop')

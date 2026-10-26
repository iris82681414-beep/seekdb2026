"""Loopback native Ollama protocol fixture for mysqltest, using only the stdlib."""
import json
import os
from pathlib import Path
import secrets
import stat
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, urlopen

MODE, PORT = sys.argv[1], int(sys.argv[2])
STATE_DIR = Path(tempfile.gettempdir()) / f'seekdb-ollama-fixture-{os.getuid()}'
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
            if json.load(response).get('fixture') != 'seekdb-native-ollama':
                raise RuntimeError('port belongs to another service')
        request = Request(URL + '/shutdown', data=token.encode(), method='POST')
        with urlopen(request, timeout=2):
            pass
    except OSError:
        pass
    STATE.unlink(missing_ok=True)


def serve():
    token = json.loads(STATE.read_text())['token']

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
            self.reply(200, {'fixture': 'seekdb-native-ollama'})

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
            if not self.headers.get('Content-Type', '').startswith('application/json'):
                self.reply(400, {'error': 'missing content type'})
            elif self.path in {'/api/generate', '/api/generate/auth'}:
                if self.path.endswith('/auth') and self.headers.get('Authorization') != 'Bearer fixture-token':
                    self.reply(401, {'error': 'authentication required'})
                elif not isinstance(body.get('prompt'), str) or body.get('stream') is not False:
                    self.reply(400, {'error': 'native non-streaming generation required'})
                elif not isinstance(body.get('options', {}), dict) or 'max_tokens' in body.get('options', {}):
                    self.reply(400, {'error': 'native runtime options required'})
                else:
                    self.reply(200, {'response': 'native:' + body['prompt'], 'done': True})
            elif self.path == '/api/embed':
                inputs = body.get('input')
                dimension = body.get('dimensions', 4)
                if not isinstance(inputs, list) or type(dimension) is not int or not 1 <= dimension <= 8:
                    self.reply(400, {'error': 'native embedding inputs required'})
                else:
                    self.reply(200, {'embeddings': [[float(i) for i in range(dimension)] for _ in inputs]})
            else:
                self.reply(404, {'error': 'unknown native endpoint'})

    server = ThreadingHTTPServer(('127.0.0.1', PORT), Handler)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if MODE == 'stop':
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
                assert json.load(response)['fixture'] == 'seekdb-native-ollama'
            break
        except OSError:
            if process.poll() is not None:
                raise RuntimeError('native Ollama fixture exited')
            time.sleep(0.1)
    else:
        process.terminate()
        process.wait(timeout=5)
        raise RuntimeError('native Ollama fixture readiness timeout')
else:
    raise ValueError('expected start, serve or stop')

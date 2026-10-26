"""Loopback rerank protocol and malformed-response fixture for mysqltest, using only the stdlib."""
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
STATE_DIR = Path(tempfile.gettempdir()) / f'seekdb-rerank-fixture-{os.getuid()}'
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
            if json.load(response).get('fixture') != 'seekdb-rerank':
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
            self.reply(200, {'fixture': 'seekdb-rerank'})

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
            dashscope = self.path == '/dashscope'
            request = body.get('input', {}) if dashscope else body
            documents = request.get('documents')
            query = request.get('query')
            config = body.get('parameters', {}) if dashscope else body
            if not isinstance(documents, list) or not 1 <= len(documents) <= 20:
                self.reply(400, {'error': 'expected batches of at most 20 documents'})
                return
            if config.get('top_n') != len(documents) or config.get('return_documents') is not False:
                self.reply(400, {'error': 'request every result without document copies'})
                return
            items = [{'index': i, 'relevance_score': 1 if query == 'tie' else int(doc.split(':')[-1]) + 0.25}
                     for i, doc in reversed(list(enumerate(documents)))]
            if query == 'missing':
                items.pop()
            elif query == 'duplicate':
                items[-1]['index'] = items[0]['index']
            elif query == 'negative':
                items[0]['index'] = -1
            elif query == 'large':
                items[0]['index'] = 18446744073709551615
            elif query == 'float-index':
                items[0]['index'] = 0.0
            elif query == 'boolean-index':
                items[0]['index'] = True
            elif query == 'string-score':
                items[0]['relevance_score'] = '0.7'
            elif query == 'boolean-score':
                items[0]['relevance_score'] = True
            elif query == 'null-score':
                items[0]['relevance_score'] = None
            elif query == 'missing-score':
                del items[0]['relevance_score']
            elif query == 'wrong-item':
                items[0] = []
            elif query == 'wrong-results':
                items = {}
            result = {'results': items}
            if dashscope:
                result = {'output': result}
                if query == 'wrong-output':
                    result = {'output': []}
            if query == 'wrong-root':
                result = []
            self.reply(200, result)

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
                assert json.load(response)['fixture'] == 'seekdb-rerank'
            break
        except OSError:
            if process.poll() is not None:
                raise RuntimeError('rerank fixture exited')
            time.sleep(0.1)
    else:
        process.terminate()
        process.wait(timeout=5)
        raise RuntimeError('rerank fixture readiness timeout')
else:
    raise ValueError('expected start, serve or stop')

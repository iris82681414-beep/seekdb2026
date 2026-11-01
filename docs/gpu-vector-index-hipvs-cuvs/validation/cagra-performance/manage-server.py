import argparse
import os
from pathlib import Path
import signal
import subprocess
import time
import pymysql

parser = argparse.ArgumentParser()
parser.add_argument('bridge')
parser.add_argument('--stop', action='store_true')
args = parser.parse_args()
runtime = Path('/workspace/hipvs-runtime')
runtime.mkdir(exist_ok=True)
pidfile = runtime / 'benchmark.pid'
pids = set()
for saved in (pidfile, runtime / 'run/seekdb.pid'):
    if saved.exists():
        pids.add(int(saved.read_text().strip()))
for pid in pids:
    process_dir = Path(f'/proc/{pid}')
    if not process_dir.exists():
        continue
    status = process_dir / 'stat'
    if not status.exists() or status.read_text().split(') ', 1)[1].startswith('Z'):
        continue
    # seekdb rewrites argv, so verify the executable and working directory.
    if (process_dir / 'cwd').resolve() != runtime or 'seekdb' not in str((process_dir / 'exe').resolve()):
        continue
    os.kill(pid, signal.SIGTERM)
    for _ in range(90):
        status = process_dir / 'stat'
        if not status.exists() or status.read_text().split(') ', 1)[1].startswith('Z'):
            break
        time.sleep(1)
    else:
        raise RuntimeError('seekdb did not stop gracefully')
for stale in (runtime / 'run/seekdb.pid', runtime / 'run/sql.sock'):
    stale.unlink(missing_ok=True)
if args.stop:
    raise SystemExit(0)
environment = dict(os.environ)
environment['LD_LIBRARY_PATH'] = f'{args.bridge}:/opt/hipvs/lib:/opt/rocm/lib'
binary = Path(args.bridge) / 'seekdb'
if not binary.exists():
    binary = Path('/workspace/hipvs-baseline/seekdb')
with (runtime / 'stdout.log').open('ab') as output:
    process = subprocess.Popen([
        str(binary), f'--base-dir={runtime}', '--port=2881', '--nodaemon',
        '--parameter', '_memory_budget=24G', '--parameter', 'datafile_size=8G',
        '--parameter', 'datafile_maxsize=20G', '--parameter', 'log_disk_size=4G', '--log-level', 'INFO',
    ], env=environment, stdout=output, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
pidfile.write_text(str(process.pid))
for _ in range(180):
    if process.poll() is not None:
        raise RuntimeError(f'seekdb exited with {process.returncode}')
    try:
        with pymysql.connect(host='127.0.0.1', port=2881, user='root', connect_timeout=2) as connection:
            with connection.cursor() as cursor:
                cursor.execute('select version()')
                print('observer ready', cursor.fetchone(), 'pid', process.pid, 'bridge', args.bridge, flush=True)
        break
    except pymysql.err.OperationalError:
        time.sleep(1)
else:
    raise RuntimeError('seekdb readiness timeout')

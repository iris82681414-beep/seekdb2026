import argparse
import json
from pathlib import Path
import time
import threading
import numpy as np
import pymysql

parser = argparse.ArgumentParser()
parser.add_argument('label')
parser.add_argument('dataset', choices=['sift', 'gaussian256'])
parser.add_argument('--lib', choices=['cuvs', 'vsag'], default='cuvs')
parser.add_argument('--load', action='store_true')
parser.add_argument('--load-only', action='store_true')
parser.add_argument('--drop-after', action='store_true')
parser.add_argument('--queries', type=int, default=0)
args = parser.parse_args()
root = Path('/workspace/hipvs-data')
queries = np.load(root / f'{args.dataset}-queries.npy')
truth = np.load(root / f'{args.dataset}-truth.npy')
if args.queries:
    queries = queries[:args.queries]
    truth = truth[:args.queries]
dimension = queries.shape[1]
table = f'items_{args.dataset}'
connection = pymysql.connect(host='127.0.0.1', port=2881, user='root', autocommit=True, local_infile=True, read_timeout=600)
cursor = connection.cursor()
cursor.execute('set ob_query_timeout=600000000')
cursor.execute('set ob_trx_timeout=600000000')
cursor.execute('create database if not exists hipvs_bench')
cursor.execute('use hipvs_bench')
cursor.execute('alter system set ob_vector_memory_limit_percentage=50')
if args.load:
    cursor.execute(f'create table {table}(id bigint primary key, v vector({dimension}))')
    start = time.perf_counter()
    cursor.execute(f"load data local infile '{root / (args.dataset + '.tsv')}' into table {table} fields terminated by '\\t' lines terminated by '\\n' (id,v)")
    print('loaded', args.dataset, time.perf_counter() - start, flush=True)
cursor.execute(f'select count(*) from {table}')
assert cursor.fetchone()[0] == 250000
if args.load_only:
    print('LOAD_COMPLETE', args.dataset, flush=True)
    raise SystemExit(0)
cursor.execute(f'show index from {table}')
if any(row[2] == 'idx_items' for row in cursor.fetchall()):
    cursor.execute(f'drop index idx_items on {table}')
def sql(vector, k, approximate=True):
    literal = '[' + ','.join(format(float(value), '.9g') for value in vector) + ']'
    return f"select id from {table} order by l2_distance(v, '{literal}') {'approximate' if approximate else ''} limit {k}"
render = next(Path('/dev/dri').glob('renderD*'))
memory_file = Path('/sys/class/drm') / render.name / 'device/mem_info_vram_used'
pid = int(Path('/workspace/hipvs-runtime/benchmark.pid').read_text())
vram, rss = [], []
stop = threading.Event()
def sample():
    while not stop.is_set():
        if memory_file.exists():
            vram.append(int(memory_file.read_text()))
        for line in Path(f'/proc/{pid}/status').read_text().splitlines():
            if line.startswith('VmRSS:'):
                rss.append(int(line.split()[1]) * 1024)
        stop.wait(0.05)
monitor = threading.Thread(target=sample, daemon=True)
monitor.start()
start = time.perf_counter()
cursor.execute(f'create vector index idx_items on {table}(v) with(distance=l2,type=hnsw,lib={args.lib},m=16,ef_construction=200,ef_search=64)')
ddl = time.perf_counter() - start
cursor.execute(sql(queries[0], 10))
first = cursor.fetchall()
end_to_end = time.perf_counter() - start
assert len(first) == 10 and len(set(first)) == 10
result = dict(label=args.label, dataset=args.dataset, lib=args.lib, rows=250000, dimension=dimension, queries=len(queries), ddl_s=ddl, build_first_query_s=end_to_end)
for k in (10, 100):
    hits = 0
    latencies = []
    for query, gold in zip(queries, truth):
        start = time.perf_counter()
        cursor.execute(sql(query, k))
        actual = [int(row[0]) for row in cursor.fetchall()]
        latencies.append(time.perf_counter() - start)
        assert len(actual) == k and len(set(actual)) == k, actual
        assert all(1000000000 <= value < 1000750000 and (value - 1000000000) % 3 == 0 for value in actual)
        expected = {1000000000 + int(index) * 3 for index in gold[:k]}
        hits += len(expected & set(actual))
    result[f'recall_{k}'] = hits / (len(queries) * k)
    result[f'qps_{k}'] = len(queries) / sum(latencies)
    result[f'p95_ms_{k}'] = float(np.percentile(latencies, 95) * 1000)
    print(json.dumps(result), flush=True)
output = root / f'sql-{args.dataset}-{args.label}.json'
stop.set()
monitor.join()
result['peak_vram_bytes'] = max(vram) if vram else None
result['peak_host_rss_bytes'] = max(rss) if rss else None
result['vram_source'] = str(memory_file)
output.write_text(json.dumps(result, indent=2))
print('COMPLETE', json.dumps(result), flush=True)
if args.drop_after:
    cursor.execute(f'drop index idx_items on {table}')
cursor.close()
connection.close()

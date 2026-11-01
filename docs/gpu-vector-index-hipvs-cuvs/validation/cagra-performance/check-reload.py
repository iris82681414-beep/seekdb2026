"""Run before/after manage-server.py restarts with the SIFT GPU index retained."""
import argparse
import json
from pathlib import Path
import numpy as np
import pymysql

parser = argparse.ArgumentParser()
parser.add_argument('mode', choices=['record', 'check'])
args = parser.parse_args()
root = Path('/workspace/hipvs-data')
queries = np.load(root / 'sift-queries.npy')[:32]
truth = np.load(root / 'sift-truth.npy')[:32]
connection = pymysql.connect(host='127.0.0.1', port=2881, user='root',
                             database='hipvs_bench', autocommit=True, read_timeout=120)
results = []
hits = 0
with connection.cursor() as cursor:
    for index, vector in enumerate(queries):
        literal = '[' + ','.join(format(float(value), '.9g') for value in vector) + ']'
        sql = f"select id from items_sift order by l2_distance(v, '{literal}') approximate limit 100"
        if index == 0:
            cursor.execute('explain ' + sql)
            print('\n'.join(str(row) for row in cursor.fetchall()))
        cursor.execute(sql)
        ids = [int(row[0]) for row in cursor.fetchall()]
        assert len(ids) == 100 and len(set(ids)) == 100
        assert all(1000000000 <= value < 1000750000 and (value - 1000000000) % 3 == 0 for value in ids)
        hits += len(set(ids) & {1000000000 + int(offset) * 3 for offset in truth[index]})
        results.append(ids)
path = root / 'reload-before.json'
if args.mode == 'record':
    path.write_text(json.dumps(results))
else:
    previous = json.loads(path.read_text())
    assert all(set(left) == set(right) for left, right in zip(previous, results))
    summary = dict(queries=len(queries), k=100, unchanged_id_sets=True,
                   recall_100=hits / (len(queries) * 100))
    (root / 'reload-result.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary))
connection.close()

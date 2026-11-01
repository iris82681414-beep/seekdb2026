import os
os.environ['OPENBLAS_NUM_THREADS'] = '8'
from pathlib import Path
import time
import numpy as np

root = Path('/workspace/hipvs-data')
rng = np.random.default_rng(20261102)
if not (root / 'gaussian256-base.npy').exists():
    np.save(root / 'gaussian256-base.npy', rng.normal(size=(250000, 256)).astype(np.float32))
    np.save(root / 'gaussian256-queries.npy', rng.normal(size=(1024, 256)).astype(np.float32))
for name in ('sift', 'gaussian256'):
    base = np.load(root / f'{name}-base.npy')
    queries = np.load(root / f'{name}-queries.npy')
    start = time.perf_counter()
    if not (root / f'{name}-truth.npy').exists():
        squared = np.einsum('ij,ij->i', base, base)
        parts = []
        for offset in range(0, len(queries), 32):
            chunk = queries[offset:offset+32]
            distances = squared[None, :] + np.einsum('ij,ij->i', chunk, chunk)[:, None] - 2 * (chunk @ base.T)
            nearest = np.argpartition(distances, 99, axis=1)[:, :100]
            order = np.argsort(np.take_along_axis(distances, nearest, axis=1), axis=1)
            parts.append(np.take_along_axis(nearest, order, axis=1))
        np.save(root / f'{name}-truth.npy', np.concatenate(parts))
    if not (root / f'{name}.tsv').exists():
        with (root / f'{name}.tsv').open('w') as stream:
            for index, vector in enumerate(base):
                literal = '[' + ','.join(format(float(value), '.9g') for value in vector) + ']'
                stream.write(f'{1000000000 + index * 3}\t{literal}\n')
    print(name, base.shape, len(queries), 'prepared_s', time.perf_counter() - start, flush=True)

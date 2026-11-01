import io
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys
import time
import h5py
import numpy as np

URL = 'https://storage.googleapis.com/ann-datasets/ann-benchmarks/sift-128-euclidean.hdf5'
LENGTH = 525128288
BLOCK = 65536
target = Path(sys.argv[1])
target.mkdir(exist_ok=True)
cache = target / 'sift-http-ranges'
cache.mkdir(exist_ok=True)
prefix = target / 'sift-128-euclidean.hdf5'

def fetch(start, length):
    path = cache / f'{start}-{length}.bin'
    if path.exists() and path.stat().st_size == length:
        return path.read_bytes()
    if prefix.exists() and prefix.stat().st_size >= start + length:
        with prefix.open('rb') as source:
            source.seek(start)
            return source.read(length)
    for attempt in range(6):
        try:
            request = urllib.request.Request(URL, headers={'Range': f'bytes={start}-{start + length - 1}'})
            with urllib.request.urlopen(request, timeout=45) as response:
                if response.status != 206:
                    raise IOError('HTTP byte ranges required')
                data = response.read()
                if len(data) != length:
                    raise IOError(f'length {len(data)} != {length}')
                path.write_bytes(data)
                return data
        except Exception:
            if attempt == 5:
                raise
            time.sleep(attempt + 1)

class RemoteFile(io.RawIOBase):
    def __init__(self):
        self.position = 0
        self.blocks = {}
    def seekable(self): return True
    def readable(self): return True
    def tell(self): return self.position
    def seek(self, offset, whence=0):
        self.position = offset if whence == 0 else self.position + offset if whence == 1 else LENGTH + offset
        return self.position
    def readinto(self, buffer):
        data = self.read(len(buffer))
        buffer[:len(data)] = data
        return len(data)
    def read(self, size=-1):
        size = min(size if size >= 0 else LENGTH, LENGTH - self.position)
        result = bytearray()
        while size:
            block, offset = divmod(self.position, BLOCK)
            if block not in self.blocks:
                self.blocks[block] = fetch(block * BLOCK, min(BLOCK, LENGTH - block * BLOCK))
            data = self.blocks[block][offset:offset + size]
            result.extend(data)
            self.position += len(data)
            size -= len(data)
        return bytes(result)

metadata = []
with h5py.File(RemoteFile(), 'r') as dataset:
    for name, rows in [('train', 250000), ('test', 4096)]:
        array = dataset[name]
        print(name, array.shape, array.dtype, array.id.get_offset(), array.chunks, flush=True)
        assert array.chunks is None and array.id.get_offset() is not None
        metadata.append((name, rows, array.shape[1], array.dtype, array.id.get_offset()))
for name, rows, dimension, dtype, offset in metadata:
    count = rows * dimension * dtype.itemsize
    stride = 4 * 1024 * 1024
    parts = [(offset + start, min(stride, count - start)) for start in range(0, count, stride)]
    with ThreadPoolExecutor(max_workers=4) as executor:
        data = b''.join(executor.map(lambda part: fetch(*part), parts))
    values = np.frombuffer(data, dtype=dtype).reshape(rows, dimension)
    np.save(target / ('sift-base.npy' if name == 'train' else 'sift-queries.npy'), values)
    print('saved', name, rows, flush=True)

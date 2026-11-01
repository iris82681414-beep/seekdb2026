# CAGRA build and single-row SQL validation

These scripts reproduce the local development measurements for the V5-based
bridge optimization. They are development tools, not the contest evaluator.
They require an isolated Linux GPU environment and use `/workspace/hipvs-data`,
`/workspace/hipvs-runtime`, localhost port 2881, and database `hipvs_bench`.
Do not point them at a production instance. Dataset downloads and Python
packages are needed only for local validation, not the offline product build.

## Build both versions from source

Use separate checkouts of frozen V5
`634888d9b037153fa143d2c913ba4b25503e1d62` and the candidate branch.
Use the same frozen third-party dependencies, `/opt/hipvs` and `/opt/rocm`.
For each checkout, set `ARTIFACTS` to `/workspace/hipvs-baseline` or
`/workspace/hipvs-candidate`, respectively:

```sh
mkdir -p "$ARTIFACTS"
export PATH="$PWD/deps/3rd/usr/local/oceanbase/devtools/bin:$PATH"
export CARGO_NET_OFFLINE=true
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh "$ARTIFACTS/libseekdb_cuvs_bridge.so"
cmake -S . -B build_release -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB="$ARTIFACTS/libseekdb_cuvs_bridge.so"
cmake --build build_release --target seekdb
cp build_release/src/observer/seekdb "$ARTIFACTS/seekdb"
```

The candidate changes only the bridge, so the recorded candidate kernel build
reused unchanged object files from the successful V5 source build and relinked
against the candidate bridge. Both configure/build logs are retained in the
development evidence directory; no precompiled executable is submitted.

## Data and exact ground truth

Use Python with NumPy, h5py and PyMySQL installed (the development environment
used `/opt/venv/bin/python`). Run from this directory:

```sh
mkdir -p /workspace/hipvs-data
python fetch-sift-subset.py /workspace/hipvs-data
python prepare-data.py
```

`fetch-sift-subset.py` reads the public ANN-benchmarks SIFT HDF5 file using HTTP
byte ranges. It takes the first 250,000 training vectors and first 4,096 test
vectors. Ground truth is recomputed for this subset, not copied from the
million-row dataset. The public source is
<https://storage.googleapis.com/ann-datasets/ann-benchmarks/sift-128-euclidean.hdf5>.

`prepare-data.py` also generates an independent FP32 Gaussian dataset with
250,000 rows, 256 dimensions, 1,024 queries and NumPy seed 20261102. This is
not Hidden250K and makes no claim about its contents or performance.
Top-100 exact L2 neighbors are computed with NumPy; TSV vectors use nine
significant digits to preserve FP32 values. SQL primary keys are deliberately
sparse (`1000000000 + row_offset * 3`) to test row-offset/ID mapping.

## Offline DDL, then individual SQL queries

```sh
python manage-server.py /workspace/hipvs-baseline
python benchmark-sql.py initial sift --load --load-only
python benchmark-sql.py initial gaussian256 --load --load-only

# Opposite ordering of baseline/candidate for the two datasets.
python manage-server.py /workspace/hipvs-candidate
python benchmark-sql.py candidate gaussian256 --queries 1024 --drop-after
python manage-server.py /workspace/hipvs-baseline
python benchmark-sql.py baseline gaussian256 --queries 1024 --drop-after
python manage-server.py /workspace/hipvs-baseline
python benchmark-sql.py baseline2 sift --queries 4096 --drop-after
python manage-server.py /workspace/hipvs-candidate
python benchmark-sql.py candidate sift --queries 4096
```

The manager gracefully stops only the process it owns in its isolated runtime
directory and starts the selected source-built binary with its corresponding
bridge. Raw tables remain loaded. Every timed run creates a fresh index using:

```sql
CREATE VECTOR INDEX idx_items ON items_sift(v)
WITH(distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64);
SELECT id FROM items_sift
ORDER BY l2_distance(v, '[FP32 query vector]') APPROXIMATE LIMIT 10;
```

The timer includes DDL and the first query, and excludes loading and startup.
Top-10 and Top-100 are measured as separate individual SQL statements, not a
bridge batch call. Every result must contain exactly k distinct valid sparse
IDs. JSON includes recall, QPS, P95, sampled GPU VRAM and seekdb RSS. GPU VRAM
is sampled every 50 ms from sysfs, so brief peaks can be missed; formal
resource gates require the organizer's measurements.

Before dropping the candidate SIFT index, verify SQL snapshot reload:

```sh
python check-reload.py record
python manage-server.py /workspace/hipvs-candidate
python check-reload.py check
```

For a CPU comparison on the same loaded SIFT table:

```sh
python benchmark-sql.py cpu sift --lib vsag --queries 128 --drop-after
```

CPU and GPU SQL paths use their existing implementations; CPU VSAG is not the
performance-scoring baseline. Do not compare the 128-query CPU diagnostic's
aggregate query results directly with the 4,096-query GPU results.

## Bridge regression and GPU tracing

From the candidate repository root:

```sh
g++ -std=c++17 -O2 -pthread tools/hipvs_bridge/test_persistence.cpp \
  -Itools/hipvs_bridge -L/workspace/hipvs-candidate \
  -Wl,-rpath,/workspace/hipvs-candidate -lseekdb_cuvs_bridge \
  -o /workspace/hipvs-candidate/test_persistence
/workspace/hipvs-candidate/test_persistence write /workspace/hipvs-candidate/test.cagra
/workspace/hipvs-candidate/test_persistence read /workspace/hipvs-candidate/test.cagra
rocprofv3 --kernel-trace --stats --output-format csv \
  --output-directory /workspace/hipvs-trace -- \
  /workspace/hipvs-candidate/test_persistence write /workspace/hipvs-candidate/profile.cagra
git diff --check
```

The regression test checks small and larger graphs, cross-process snapshot
reload, wrong snapshot dimensions, workspace growth/reuse, optional distances,
invalid arguments, distinct IDs, FP32 distance correctness, and concurrent
searches on the same handle. Profiling is separate from timed benchmarks.

See [REPORT.md](REPORT.md) and the adjacent measured JSON files for actual
results, limitations, and the tests that were run.

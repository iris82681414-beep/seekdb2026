# V5 CAGRA bridge optimization report

Measured on 2026-11-01. All numbers below are local development results,
not official contest scores. The unpublished Hidden250K dataset was not
available and has not been validated.

## Baseline and implementation

The branch descends directly from V5 frozen commit
`634888d9b037153fa143d2c913ba4b25503e1d62`. It does not modify hipVS/cuVS,
SQL syntax, input precision, ID mapping, the snapshot format, or the VSAG CPU
implementation. The fixed hipVS installation is used for GPU graph building
and CAGRA search.

The changes in `tools/hipvs_bridge/seekdb_cuvs_bridge.c` are:

1. Bound the OpenMP build team by the process's available CPUs and cgroup CPU
   quota, while honoring smaller user settings. Restore the calling thread's
   previous setting after building. This environment exposes 128 host CPUs but
   grants only 16 CPU cores; unbounded host-side graph work oversubscribes it.
2. Upload the original FP32 dataset once and retain the device allocation with
   the index. CAGRA can reuse an aligned device view rather than uploading host
   data again when attaching the search dataset. Keep the data alive until the
   CAGRA index has been destroyed, including on error paths.
3. Use 20 NN-Descent iterations, graph degree 128 and intermediate degree 256.
   Compensate with deeper CAGRA traversal: explicit MULTI_CTA and itopk 2048
   (512 for fewer than 2048 rows, at least four times requested k), width 4.
   The fixed C API initializes the search-algorithm enum to SINGLE_CTA; it is
   necessary to select MULTI_CTA explicitly for this larger candidate set.
4. Reuse query/output GPU buffers and search parameters per index. Use its
   resource stream for transfers with one final synchronization before returning
   caller-owned output. Protect the shared workspace with an index-local mutex.
5. Reject invalid shapes, overflow and invalid pointers, and check output-copy
   and synchronization errors.

`build.sh` enables the existing ROCm OpenMP runtime and pthread support.
No new downloaded product dependency is introduced.

Reducing graph iterations alone was rejected during development because it
reduced Gaussian recall. The submitted implementation combines the build
change with deeper GPU search; the results below show the measured recall
effect. An RMM pool experiment was also rejected because it increased peak
memory without improving build time. Neither experiment is a separate runtime
path in the submitted source. The same implementation is used for all inputs
and for timed and untimed execution.

## Environment and method

- Ubuntu 24.04.4, AMD Radeon PRO W7900-family GPU, approximately 48 GiB VRAM.
- ROCm 7.2.4, installed fixed hipVS, GCC 13, CMake from frozen seekdb tools,
  Rust 1.97.1; frozen V5 third-party dependency cache.
- Container CPU quota: 16 cores; host CPU visibility: 128. Container memory
  limit: 59,055,800,320 bytes. These differ from the official 24-core slot.
- Both versions built from V5 source; candidate bridge rebuilt and unchanged
  kernel objects reused for successful reconfiguration/relink. A second clean
  compilation of every unchanged candidate object was not performed.
- Server memory budget 24G, datafile 8G/max 20G, log disk 4G, no external load.
- Tables contain 250,000 preloaded FP32 vectors with sparse external IDs.
  Fresh offline `CREATE VECTOR INDEX` uses the fixed contest SQL parameters.
  Wall time includes DDL and the first successful Top-10 SQL query.
- GPU SIFT uses 4,096 individual SQL queries for each of Top-10 and Top-100.
  Independent Gaussian256 uses 1,024 individual queries for each k. Exact
  ground truth was recomputed against each 250K-row base dataset.
- The primary paired runs are Gaussian candidate then baseline, and SIFT
  baseline then candidate, with server restarts and fresh indexes. Each is one
  local pair, not a statistical confidence interval. An earlier SIFT baseline
  run (49.38 s) is retained separately; the table uses the fresh 44.67 s run.

See [README.md](README.md) for build commands, public data provenance, scripts
and exact execution order. No dataset or prebuilt binary is committed.

## GPU baseline comparison

| Dataset | Version | DDL + first query (s) | Recall@10 | Recall@100 | Top-10 QPS | Top-100 QPS |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Public SIFT250K x 128 | Frozen V5 | 44.673 | 0.999951 | 0.999973 | 136.92 | 130.75 |
| Public SIFT250K x 128 | Candidate | 15.739 | 0.999951 | 0.999968 | 236.30 | 216.14 |
| Independent Gaussian250K x 256 | Frozen V5 | 50.750 | 0.953125 | 0.897969 | 127.51 | 120.95 |
| Independent Gaussian250K x 256 | Candidate | 18.115 | 0.989746 | 0.981582 | 229.70 | 208.46 |

| Dataset | Build speedup | Geometric QPS ratio | Sampled VRAM ratio | Baseline / candidate sampled VRAM (bytes) |
| --- | ---: | ---: | ---: | ---: |
| Public SIFT250K x 128 | 2.838x | 1.689x | 1.088x | 734191616 / 798695424 |
| Independent Gaussian250K x 256 | 2.802x | 1.762x | 1.223x | 862375936 / 1054810112 |

SIFT Recall@100 changes by approximately -0.00000488, well inside 0.005.
Both measured local datasets satisfy the applicable local ID/recall/query
checks. Gaussian results do not establish the formal hidden-data gates.
VRAM is sampled from the GPU sysfs counter every 50 ms; this can miss transient
peaks and is not a replacement for the evaluator's resource accounting.

## CPU comparison and compatibility

The candidate seekdb binary also built and queried a fresh SIFT250K
`lib=vsag` index with the same SQL index parameters. DDL plus first query took
81.047 s, compared with 15.739 s for candidate GPU CAGRA: a 5.149x local build
speedup. This CPU result is diagnostic; official performance scoring compares
against the V5 GPU baseline above.

The CPU check used the first 128 queries per k. Recall@10 was 0.991406 and
Recall@100 0.963203; all returned IDs were valid and distinct. Its shorter
query workload is not used for the GPU QPS ratios. The GPU process retained
runtime allocations from preceding GPU tests, so CPU-run VRAM readings do
not indicate that CPU VSAG queries used GPU search.

## Validation actually executed

- V5 source bridge and seekdb build completed; candidate bridge compilation,
  prescribed CMake configuration and `seekdb` target build/relink completed.
- Expanded C++ regression compiled with GCC and passed in separate `write`
  and `read` processes: 1024-row small graph, 10000-row persisted graph,
  wrong snapshot shape rejection, query buffer growth/reuse, null/invalid
  inputs, optional distance output, valid distinct IDs, FP32 distances and
  four concurrent search callers sharing one handle.
- Candidate SIFT250K index survived a graceful server restart. All 32 checked
  Top-100 query ID sets matched before/after restart, with Recall@100 = 1.0.
  EXPLAIN selected `VECTOR INDEX SCAN`; logs show deserialization of the
  256001713-byte GPU graph snapshot, rather than rebuilding a graph.
- External `rocprofv3 --kernel-trace --stats` on the C++ regression completed.
  It recorded 897 GPU kernel dispatches, including NN-Descent `local_join`
  kernels, CAGRA graph pruning and `multi_cta_search::search_kernel`. This
  demonstrates actual GPU graph building and fixed-library GPU CAGRA search
  in the profiled bridge run. It does not replace the organizer's SQL-path
  profiler gate. Kernel statistics are included in `results/kernel_stats.csv`.
- `git diff --check` passed.

Measured JSON, test output, EXPLAIN and lifecycle evidence are in `results/`.
The baseline source build and candidate build logs are backed up separately
in the development workspace, not required to compile the submission.

## Remaining verification

The official Hidden250K data, full official profiler/CPU-fallback/integrity
checks, 4096-query Gaussian run, cgroup-v1 branch, allocation-failure injection,
and the evaluator's exact peak-VRAM measurements were not executed locally.
There is no claimed official score. One local pair per dataset cannot predict
performance under all thermal/load conditions or the official 24-core quota.
Use the contest evaluator to validate the hard gates and final score.

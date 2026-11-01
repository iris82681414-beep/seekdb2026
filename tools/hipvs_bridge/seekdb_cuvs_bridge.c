/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <omp.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  cuvsCagraSearchParams_t sp;
  float *queries;
  unsigned int *neighbors;
  float *distances;
  size_t query_bytes, neighbor_bytes, distance_bytes;
  pthread_mutex_t search_mutex;
  float *dataset;
  size_t dataset_bytes;
  long n, dim;
} sk_cuvs_index;

/* OpenMP sees the host's CPUs, which can greatly exceed a container's CPU
 * quota. Bound the current build team's size without overriding a smaller
 * user setting or changing OpenMP settings in other seekdb worker threads. */
static int build_thread_limit(void)
{
  int limit = omp_get_max_threads();
  const int available = omp_get_num_procs();
  if (available > 0 && available < limit) limit = available;
  long long quota = -1, period = 0;
  FILE *file = fopen("/sys/fs/cgroup/cpu.max", "r");
  if (file) {
    if (fscanf(file, "%lld %lld", &quota, &period) != 2) quota = -1;
    fclose(file);
  } else {
    const char *roots[] = {"/sys/fs/cgroup/cpu", "/sys/fs/cgroup/cpu,cpuacct"};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
      char path[128];
      snprintf(path, sizeof(path), "%s/cpu.cfs_quota_us", roots[i]);
      file = fopen(path, "r");
      if (!file) continue;
      if (fscanf(file, "%lld", &quota) != 1) quota = -1;
      fclose(file);
      snprintf(path, sizeof(path), "%s/cpu.cfs_period_us", roots[i]);
      file = fopen(path, "r");
      if (file) {
        if (fscanf(file, "%lld", &period) != 1) period = 0;
        fclose(file);
      }
      break;
    }
  }
  if (quota > 0 && period > 0) {
    const long long cores = quota / period + (quota % period != 0);
    if (cores > 0 && cores < limit) limit = (int)cores;
  }
  return limit > 0 ? limit : 1;
}

static cuvsError_t build_graph(sk_cuvs_index *h, DLManagedTensor *dataset)
{
  const int previous = omp_get_max_threads();
  omp_set_num_threads(build_thread_limit());
  const cuvsError_t result = cuvsCagraBuild(h->res, h->ip, dataset, h->idx);
  omp_set_num_threads(previous);
  return result;
}

/* Retain scratch space with the index instead of synchronizing the device
 * allocator for every single-row SQL query. A failed growth keeps the old buffer. */
static int reserve_buffer(sk_cuvs_index *h, void **buffer, size_t *capacity, size_t bytes)
{
  if (*capacity >= bytes) return 0;
  void *replacement = NULL;
  if (cuvsRMMAlloc(h->res, &replacement, bytes) != CUVS_SUCCESS) return 1;
  if (*buffer) cuvsRMMFree(h->res, *buffer, *capacity);
  *buffer = replacement;
  *capacity = bytes;
  return 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  if (!base || n <= 0 || dim <= 0 || (uint64_t)n > UINT32_MAX ||
      (uint64_t)n > SIZE_MAX / sizeof(float) / (uint64_t)dim) return NULL;
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (pthread_mutex_init(&h->search_mutex, NULL) != 0) { free(h); return NULL; }
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) {
    pthread_mutex_destroy(&h->search_mutex); free(h); return NULL;
  }

  DLManagedTensor dset = {0}; long ds[2] = {n, dim};
  cudaStream_t stream;
  dset.dl_tensor.device.device_type = kDLCUDA; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  h->ip->intermediate_graph_degree = 256;
  /* Keep the library's standard NN-Descent budget, and use deeper GPU
   * traversal below to retain recall without 100 full graph-refinement passes. */
  h->ip->nn_descent_niter = 20;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  /* Upload FP32 once. CAGRA can retain this aligned device view for search,
   * instead of transferring the host dataset again when attaching it. */
  h->dataset_bytes = sizeof(float) * n * dim;
  if (cuvsRMMAlloc(h->res, (void**)&h->dataset, h->dataset_bytes) != CUVS_SUCCESS) goto fail_idx;
  if (cuvsStreamGet(h->res, &stream) != CUVS_SUCCESS ||
      cudaMemcpyAsync(h->dataset, base, h->dataset_bytes, cudaMemcpyDefault, stream) != cudaSuccess) goto fail_idx;
  dset.dl_tensor.data = h->dataset;
  if (build_graph(h, &dset) != CUVS_SUCCESS) goto fail_idx;
  if (cuvsStreamSync(h->res) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
  if (h->dataset) cuvsRMMFree(h->res, h->dataset, h->dataset_bytes);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); pthread_mutex_destroy(&h->search_mutex); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !query || !out_ids || nq <= 0 || topk <= 0 || topk > h->n ||
      (uint64_t)nq > SIZE_MAX / sizeof(float) / (uint64_t)h->dim ||
      (uint64_t)nq > SIZE_MAX / sizeof(unsigned) / (uint64_t)topk) return 1;
  pthread_mutex_lock(&h->search_mutex);
  int rc = 0;
  const size_t query_bytes = sizeof(float) * nq * h->dim;
  const size_t neighbor_bytes = sizeof(unsigned) * nq * topk;
  const size_t distance_bytes = sizeof(float) * nq * topk;
  cudaStream_t stream;
  if (reserve_buffer(h, (void**)&h->queries, &h->query_bytes, query_bytes)) { rc=10; goto done; }
  if (reserve_buffer(h, (void**)&h->neighbors, &h->neighbor_bytes, neighbor_bytes)) { rc=11; goto done; }
  if (reserve_buffer(h, (void**)&h->distances, &h->distance_bytes, distance_bytes)) { rc=12; goto done; }
  if (!h->sp && cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { rc=14; goto done; }
  if (cuvsStreamGet(h->res, &stream) != CUVS_SUCCESS) { rc=13; goto done; }
  if (cudaMemcpyAsync(h->queries, query, query_bytes, cudaMemcpyDefault, stream) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt = {0}, nt = {0}, dt = {0}; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->queries; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->neighbors; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->distances; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    {
      size_t required_itopk = (size_t)topk * 4;
      const size_t minimum_itopk = h->n < 2048 ? 512 : 2048;
      if (required_itopk < minimum_itopk) required_itopk = minimum_itopk;
      h->sp->itopk_size = required_itopk;
      h->sp->search_width = 4;
      /* The C API zero-initializes algo to SINGLE_CTA. MULTI_CTA supports the
       * larger candidate set and avoids its high single-query latency. Set it
       * explicitly so batch queries do not switch to a smaller-capacity kernel. */
      h->sp->algo = MULTI_CTA;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpyAsync(out_ids, h->neighbors, neighbor_bytes, cudaMemcpyDefault, stream) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpyAsync(out_dist, h->distances, distance_bytes, cudaMemcpyDefault, stream) != cudaSuccess) { rc=17; goto done; }
  }
done:
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  /* Complete output copies before caller-owned memory can go out of scope. */
  if (cuvsStreamSync(h->res) != CUVS_SUCCESS && rc == 0) {
    rc = 18;
    fprintf(stderr, "[seekdb_cuvs] synchronize failed: %s\n", cuvsGetLastErrorText());
  }
  pthread_mutex_unlock(&h->search_mutex);
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsStreamSync(h->res);
  cuvsCagraIndexDestroy(h->idx);
  if (h->dataset) cuvsRMMFree(h->res, h->dataset, h->dataset_bytes);
  cuvsCagraIndexParamsDestroy(h->ip);
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  if (h->queries) cuvsRMMFree(h->res, h->queries, h->query_bytes);
  if (h->neighbors) cuvsRMMFree(h->res, h->neighbors, h->neighbor_bytes);
  if (h->distances) cuvsRMMFree(h->res, h->distances, h->distance_bytes);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->search_mutex);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (pthread_mutex_init(&index->search_mutex, NULL) != 0) { free(index); return NULL; }
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  pthread_mutex_destroy(&index->search_mutex);
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}

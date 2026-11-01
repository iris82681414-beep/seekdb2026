#include "seekdb_cuvs_bridge.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <atomic>
#include <limits>
#include <set>
#include <thread>
#include <utility>

static bool check_search(void *index, const std::vector<float> &vectors, long count, long dimension,
                         long queries, long topk)
{
  std::vector<unsigned int> ids(queries * topk, std::numeric_limits<unsigned int>::max());
  std::vector<float> distances(queries * topk, std::numeric_limits<float>::quiet_NaN());
  if (seekdb_cuvs_search(index, vectors.data(), queries, topk, ids.data(), distances.data()) != 0) return false;
  for (long query = 0; query < queries; ++query) {
    std::set<unsigned int> distinct;
    for (long rank = 0; rank < topk; ++rank) {
      const unsigned int id = ids[query * topk + rank];
      const float distance = distances[query * topk + rank];
      if (id >= count || !std::isfinite(distance) || !distinct.insert(id).second) return false;
      double exact = 0;
      for (long column = 0; column < dimension; ++column) {
        const double delta = double(vectors[query * dimension + column]) - vectors[id * dimension + column];
        exact += delta * delta;
      }
      if (std::fabs(exact - distance) > 0.001 * (1 + exact)) return false;
    }
  }
  return true;
}

int main(int argc, char **argv)
{
  if (argc != 3) return 2;
  const long count = 10000;
  const long dimension = 32;
  std::vector<float> vectors(count * dimension);
  for (long row = 0; row < count; ++row) {
    for (long column = 0; column < dimension; ++column) {
      vectors[row * dimension + column] = std::sin(float(row * 31 + column * 17));
    }
  }
  if (seekdb_cuvs_build(nullptr, count, dimension) != nullptr ||
      seekdb_cuvs_build(vectors.data(), 0, dimension) != nullptr ||
      seekdb_cuvs_build(vectors.data(), count, -1) != nullptr) return 8;
  if (std::strcmp(argv[1], "write") == 0) {
    void *small = seekdb_cuvs_build(vectors.data(), 1024, dimension);
    if (!small || !check_search(small, vectors, 1024, dimension, 2, 10)) return 12;
    seekdb_cuvs_free(small);
  }
  void *index = nullptr;
  if (std::strcmp(argv[1], "write") == 0) {
    index = seekdb_cuvs_build(vectors.data(), count, dimension);
    if (!index || seekdb_cuvs_serialize(index, argv[2]) != 0) return 3;
  } else {
    void *invalid = seekdb_cuvs_deserialize(argv[2], count - 1, dimension);
    if (invalid) { seekdb_cuvs_free(invalid); return 6; }
    invalid = seekdb_cuvs_deserialize(argv[2], count, dimension + 1);
    if (invalid) { seekdb_cuvs_free(invalid); return 7; }
    index = seekdb_cuvs_deserialize(argv[2], count, dimension);
    if (!index) return 4;
  }
  unsigned int ids[10];
  float distances[10];
  int result = seekdb_cuvs_search(index, vectors.data(), 1, 10, ids, distances);
  // Exercise workspace growth and reuse in both directions, including after reload.
  for (const auto shape : {std::pair<long, long>{1, 10}, {8, 100}, {2, 10}, {1, 100}}) {
    if (!check_search(index, vectors, count, dimension, shape.first, shape.second)) return 9;
  }
  if (seekdb_cuvs_search(index, vectors.data(), 0, 10, ids, nullptr) == 0 ||
      seekdb_cuvs_search(index, vectors.data(), 1, 0, ids, nullptr) == 0 ||
      seekdb_cuvs_search(index, vectors.data(), 1, count + 1, ids, nullptr) == 0 ||
      seekdb_cuvs_search(index, nullptr, 1, 10, ids, nullptr) == 0 ||
      seekdb_cuvs_search(index, vectors.data(), 1, 10, nullptr, nullptr) == 0) return 10;
  if (seekdb_cuvs_search(index, vectors.data(), 1, 10, ids, nullptr) != 0) return 13;
  std::atomic<bool> correct{true};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&] {
      for (int repeat = 0; repeat < 10; ++repeat) {
        if (!check_search(index, vectors, count, dimension, 2, 10)) correct = false;
      }
    });
  }
  for (auto &thread : threads) thread.join();
  if (!correct) return 11;
  seekdb_cuvs_free(index);
  if (result != 0 || ids[0] != 0 || std::fabs(distances[0]) > 0.0001f) return 5;
  std::printf("%s: n=%ld top1=%u distance=%g\n", argv[1], count, ids[0], distances[0]);
  return 0;
}

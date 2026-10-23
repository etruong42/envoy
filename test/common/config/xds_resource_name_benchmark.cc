#include <string>
#include <vector>

#include "source/common/common/macros.h"
#include "source/common/config/xds_symbol_table.h"
#include "source/common/memory/stats.h"

#include "test/common/memory/memory_test_utility.h"

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"

namespace Envoy {
namespace Config {
namespace {

std::vector<std::string> generateSampleXdsClusterNames(size_t count) {
  std::vector<std::string> names;
  names.reserve(count);
  static const char* authorities[] = {
      "traffic-director-global.xds.googleapis.com",
      "traffic-director-p2p.xds.googleapis.com",
      "traffic-director-c2p.xds.googleapis.com",
  };
  static const char* projects[] = {
      "1082309372360", "414017652406", "928374019283", "512398471029", "684729103847",
  };
  static const char* regions[] = {
      "europe-west1",
      "us-central1",
      "asia-east1",
      "us-east4",
  };

  for (size_t i = 0; i < count; ++i) {
    const char* authority = authorities[i % 3];
    const char* project = projects[(i / 3) % 5];
    const char* region = regions[(i / 15) % 4];
    names.push_back(absl::StrFormat("xdstp://%s/envoy.config.cluster.v3.Cluster/%s/default/"
                                    "cloud-internal-17-lb-%s_cloud_17_ilb_%s_%019llu",
                                    authority, project, region, project,
                                    static_cast<unsigned long long>(1000000000000000000ULL + i)));
  }
  return names;
}

// ============================================================================
// BASELINE (std::string) BENCHMARKS
// ============================================================================

void BM_Baseline_String_Creation_10k(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(10000);
  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    std::vector<std::string> cluster_names;
    cluster_names.reserve(raw_names.size());
    for (const auto& name : raw_names) {
      cluster_names.emplace_back(name);
    }
    benchmark::DoNotOptimize(cluster_names);
  }
}
BENCHMARK(BM_Baseline_String_Creation_10k);

void BM_Baseline_String_WatchMap_Insert_10k(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(10000);
  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    absl::flat_hash_map<std::string, std::vector<void*>> watch_interest;
    watch_interest.reserve(raw_names.size());
    for (const auto& name : raw_names) {
      watch_interest[name].push_back(reinterpret_cast<void*>(0x1234));
    }
    benchmark::DoNotOptimize(watch_interest);
  }
}
BENCHMARK(BM_Baseline_String_WatchMap_Insert_10k);

void BM_Baseline_String_WatchMap_Lookup_10k(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(10000);
  absl::flat_hash_map<std::string, std::vector<void*>> watch_interest;
  watch_interest.reserve(raw_names.size());
  for (const auto& name : raw_names) {
    watch_interest[name].push_back(reinterpret_cast<void*>(0x1234));
  }

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    size_t found = 0;
    for (const auto& name : raw_names) {
      auto it = watch_interest.find(name);
      if (it != watch_interest.end()) {
        ++found;
      }
    }
    benchmark::DoNotOptimize(found);
  }
}
BENCHMARK(BM_Baseline_String_WatchMap_Lookup_10k);

void BM_Baseline_String_ClusterManager_Btree_10k(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(10000);
  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    absl::btree_map<std::string, void*> active_clusters;
    for (const auto& name : raw_names) {
      active_clusters.emplace(name, reinterpret_cast<void*>(0xbeef));
    }
    benchmark::DoNotOptimize(active_clusters);
  }
}
BENCHMARK(BM_Baseline_String_ClusterManager_Btree_10k);

void BM_Baseline_String_Routes_50k(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(10000);
  struct MockRouteEntry {
    std::string cluster_name;
    uint32_t priority;
  };

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    std::vector<MockRouteEntry> routes;
    routes.reserve(50000);
    for (size_t i = 0; i < 50000; ++i) {
      routes.push_back({raw_names[i % raw_names.size()], static_cast<uint32_t>(i % 3)});
    }
    benchmark::DoNotOptimize(routes);
  }
}
BENCHMARK(BM_Baseline_String_Routes_50k);

void BM_Baseline_String_Equality(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(2);
  const std::string& name_a = raw_names[0];
  const std::string& name_b = raw_names[1];
  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    bool eq = (name_a == name_b);
    benchmark::DoNotOptimize(eq);
  }
}
BENCHMARK(BM_Baseline_String_Equality);

// ============================================================================
// XdsSymbolTable / XdsResourceName BENCHMARKS
// ============================================================================

void BM_XdsSymbolTable_Creation_10k(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(10000);
  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    auto table = std::make_shared<XdsSymbolTable>();
    std::vector<XdsResourceNameStorage> storages;
    storages.reserve(raw_names.size());
    for (const auto& name : raw_names) {
      storages.emplace_back(table->encode(name));
    }
    benchmark::DoNotOptimize(storages);
  }
}
BENCHMARK(BM_XdsSymbolTable_Creation_10k);

void BM_XdsSymbolTable_WatchMap_Insert_10k(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(10000);
  auto table = std::make_shared<XdsSymbolTable>();
  std::vector<XdsResourceNameStorage> storages;
  storages.reserve(raw_names.size());
  for (const auto& name : raw_names) {
    storages.emplace_back(table->encode(name));
  }

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    absl::flat_hash_map<XdsResourceName, std::vector<void*>> watch_interest;
    watch_interest.reserve(storages.size());
    for (const auto& storage : storages) {
      watch_interest[storage.resourceName()].push_back(reinterpret_cast<void*>(0x1234));
    }
    benchmark::DoNotOptimize(watch_interest);
  }
}
BENCHMARK(BM_XdsSymbolTable_WatchMap_Insert_10k);

void BM_XdsSymbolTable_WatchMap_Lookup_10k(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(10000);
  auto table = std::make_shared<XdsSymbolTable>();
  std::vector<XdsResourceNameStorage> storages;
  storages.reserve(raw_names.size());
  for (const auto& name : raw_names) {
    storages.emplace_back(table->encode(name));
  }

  absl::flat_hash_map<XdsResourceName, std::vector<void*>> watch_interest;
  watch_interest.reserve(storages.size());
  for (const auto& storage : storages) {
    watch_interest[storage.resourceName()].push_back(reinterpret_cast<void*>(0x1234));
  }

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    size_t found = 0;
    for (const auto& storage : storages) {
      auto it = watch_interest.find(storage.resourceName());
      if (it != watch_interest.end()) {
        ++found;
      }
    }
    benchmark::DoNotOptimize(found);
  }
}
BENCHMARK(BM_XdsSymbolTable_WatchMap_Lookup_10k);

void BM_XdsSymbolTable_ClusterManager_Btree_10k(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(10000);
  auto table = std::make_shared<XdsSymbolTable>();
  std::vector<XdsResourceNameStorage> storages;
  storages.reserve(raw_names.size());
  for (const auto& name : raw_names) {
    storages.emplace_back(table->encode(name));
  }

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    absl::btree_map<XdsResourceName, void*> active_clusters;
    for (const auto& storage : storages) {
      active_clusters.emplace(storage.resourceName(), reinterpret_cast<void*>(0xbeef));
    }
    benchmark::DoNotOptimize(active_clusters);
  }
}
BENCHMARK(BM_XdsSymbolTable_ClusterManager_Btree_10k);

void BM_XdsSymbolTable_Routes_50k(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(10000);
  auto table = std::make_shared<XdsSymbolTable>();
  std::vector<XdsResourceNameStorage> storages;
  storages.reserve(raw_names.size());
  for (const auto& name : raw_names) {
    storages.emplace_back(table->encode(name));
  }

  struct MockXdsRouteEntry {
    XdsResourceName cluster_name;
    uint32_t priority;
  };

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    std::vector<MockXdsRouteEntry> routes;
    routes.reserve(50000);
    for (size_t i = 0; i < 50000; ++i) {
      routes.push_back(
          {storages[i % storages.size()].resourceName(), static_cast<uint32_t>(i % 3)});
    }
    benchmark::DoNotOptimize(routes);
  }
}
BENCHMARK(BM_XdsSymbolTable_Routes_50k);

void BM_XdsSymbolTable_Equality(benchmark::State& state) {
  const auto raw_names = generateSampleXdsClusterNames(2);
  auto table = std::make_shared<XdsSymbolTable>();
  auto storage_a = table->encode(raw_names[0]);
  auto storage_b = table->encode(raw_names[1]);
  XdsResourceName name_a = storage_a.resourceName();
  XdsResourceName name_b = storage_b.resourceName();

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    bool eq = (name_a == name_b);
    benchmark::DoNotOptimize(eq);
  }
}
BENCHMARK(BM_XdsSymbolTable_Equality);

} // namespace
} // namespace Config
} // namespace Envoy

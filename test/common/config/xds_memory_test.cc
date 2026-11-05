#include <iostream>
#include <string>
#include <vector>

#include "source/common/config/xds_symbol_table.h"
#include "source/common/memory/stats.h"

#include "test/common/memory/memory_test_utility.h"
#include "test/test_common/utility.h"

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "gtest/gtest.h"

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

struct BaselineRouteEntry {
  std::string cluster_name;
  uint32_t priority;
};

struct XdsSymbolTableRouteEntry {
  XdsResourceName cluster_name;
  uint32_t priority;
};

TEST(XdsMemoryComparisonTest, CompareBaselineVsXdsSymbolTable) {
  constexpr size_t NumClusters = 10000;
  constexpr size_t NumRoutes = 50000;

  auto raw_names = generateSampleXdsClusterNames(NumClusters);

  std::cout << "======================================================================"
            << std::endl;
  std::cout << "XDS MEMORY REDUCTION BENCHMARK: BASELINE vs XdsSymbolTable" << std::endl;
  std::cout << "Workload: " << NumClusters << " clusters, " << NumRoutes << " routes" << std::endl;
  std::cout << "======================================================================"
            << std::endl;

  // -------------------------------------------------------------------------
  // 1. BASELINE MEASUREMENTS (std::string)
  // -------------------------------------------------------------------------
  size_t baseline_mem_before = Memory::Stats::totalCurrentlyAllocated();

  std::vector<std::string> baseline_names;
  baseline_names.reserve(NumClusters);
  for (const auto& name : raw_names) {
    baseline_names.push_back(name);
  }

  absl::flat_hash_map<std::string, std::vector<void*>> baseline_watch_interest;
  baseline_watch_interest.reserve(NumClusters);
  for (const auto& name : baseline_names) {
    baseline_watch_interest[name].push_back(reinterpret_cast<void*>(0x1234));
  }

  absl::btree_map<std::string, void*> baseline_active_clusters;
  for (const auto& name : baseline_names) {
    baseline_active_clusters.emplace(name, reinterpret_cast<void*>(0xbeef));
  }

  std::vector<BaselineRouteEntry> baseline_routes;
  baseline_routes.reserve(NumRoutes);
  for (size_t i = 0; i < NumRoutes; ++i) {
    baseline_routes.push_back({baseline_names[i % NumClusters], static_cast<uint32_t>(i % 3)});
  }

  size_t baseline_mem_after = Memory::Stats::totalCurrentlyAllocated();
  size_t baseline_total_heap =
      (baseline_mem_after > baseline_mem_before) ? (baseline_mem_after - baseline_mem_before) : 0;

  // Clean up baseline to isolate XdsSymbolTable measurement
  baseline_routes.clear();
  baseline_routes.shrink_to_fit();
  baseline_active_clusters.clear();
  baseline_watch_interest.clear();
  baseline_names.clear();
  baseline_names.shrink_to_fit();

  // -------------------------------------------------------------------------
  // 2. XdsSymbolTable MEASUREMENTS
  // -------------------------------------------------------------------------
  size_t symbol_mem_start = Memory::Stats::totalCurrentlyAllocated();

  auto table = XdsSymbolTable::create();
  std::vector<XdsResourceNameStorage> symbol_storages;
  symbol_storages.reserve(NumClusters);
  for (const auto& name : raw_names) {
    symbol_storages.emplace_back(table->encode(name));
  }
  size_t mem_after_storages = Memory::Stats::totalCurrentlyAllocated();
  size_t storages_heap =
      (mem_after_storages > symbol_mem_start) ? (mem_after_storages - symbol_mem_start) : 0;

  absl::flat_hash_map<XdsResourceName, std::vector<void*>> symbol_watch_interest;
  symbol_watch_interest.reserve(NumClusters);
  for (const auto& storage : symbol_storages) {
    symbol_watch_interest[storage.resourceName()].push_back(reinterpret_cast<void*>(0x1234));
  }
  size_t mem_after_sym_watch = Memory::Stats::totalCurrentlyAllocated();
  size_t sym_watch_heap =
      (mem_after_sym_watch > mem_after_storages) ? (mem_after_sym_watch - mem_after_storages) : 0;

  absl::btree_map<XdsResourceName, void*> symbol_active_clusters;
  for (const auto& storage : symbol_storages) {
    symbol_active_clusters.emplace(storage.resourceName(), reinterpret_cast<void*>(0xbeef));
  }
  size_t mem_after_sym_cm = Memory::Stats::totalCurrentlyAllocated();
  size_t sym_cm_heap =
      (mem_after_sym_cm > mem_after_sym_watch) ? (mem_after_sym_cm - mem_after_sym_watch) : 0;

  std::vector<XdsSymbolTableRouteEntry> symbol_routes;
  symbol_routes.reserve(NumRoutes);
  for (size_t i = 0; i < NumRoutes; ++i) {
    symbol_routes.push_back(
        {symbol_storages[i % NumClusters].resourceName(), static_cast<uint32_t>(i % 3)});
  }
  size_t mem_after_sym_routes = Memory::Stats::totalCurrentlyAllocated();
  size_t sym_routes_heap =
      (mem_after_sym_routes > mem_after_sym_cm) ? (mem_after_sym_routes - mem_after_sym_cm) : 0;

  size_t symbol_total_heap =
      (mem_after_sym_routes > symbol_mem_start) ? (mem_after_sym_routes - symbol_mem_start) : 0;

  double reduction_pct = (baseline_total_heap > 0)
                             ? (1.0 - static_cast<double>(symbol_total_heap) /
                                          static_cast<double>(baseline_total_heap)) *
                                   100.0
                             : 0.0;
  double compression_ratio = (symbol_total_heap > 0) ? static_cast<double>(baseline_total_heap) /
                                                           static_cast<double>(symbol_total_heap)
                                                     : 0.0;

  std::cout << "[1] XdsSymbolTable + 10k Name Storages heap: " << storages_heap << " bytes ("
            << (storages_heap / 1024.0 / 1024.0) << " MB)" << std::endl;
  std::cout << "[2] WatchMap with XdsResourceName (10k entries) heap: " << sym_watch_heap
            << " bytes (" << (sym_watch_heap / 1024.0 / 1024.0) << " MB)" << std::endl;
  std::cout << "[3] ClusterManager with XdsResourceName (10k entries) heap: " << sym_cm_heap
            << " bytes (" << (sym_cm_heap / 1024.0 / 1024.0) << " MB)" << std::endl;
  std::cout << "[4] 50k Route Entries with XdsResourceName heap: " << sym_routes_heap << " bytes ("
            << (sym_routes_heap / 1024.0 / 1024.0) << " MB)" << std::endl;
  std::cout << "----------------------------------------------------------------------"
            << std::endl;
  std::cout << "BASELINE HEAP CONSUMED:       " << baseline_total_heap << " bytes ("
            << (baseline_total_heap / 1024.0 / 1024.0) << " MB)" << std::endl;
  std::cout << "XdsSymbolTable HEAP CONSUMED: " << symbol_total_heap << " bytes ("
            << (symbol_total_heap / 1024.0 / 1024.0) << " MB)" << std::endl;
  std::cout << "MEMORY SAVINGS:               " << reduction_pct << "%" << std::endl;
  std::cout << "COMPRESSION FACTOR:           " << compression_ratio << "x" << std::endl;
  std::cout << "UNIQUE SYMBOLS IN TABLE:      " << table->numActiveSymbols()
            << " tokens shared across all 10k cluster names" << std::endl;
  std::cout << "======================================================================"
            << std::endl;

  if (Memory::TestUtil::MemoryTest::mode() != Memory::TestUtil::MemoryTest::Mode::Disabled) {
    EXPECT_GT(reduction_pct, 50.0);
  } else {
    ENVOY_LOG_MISC(info, "Skipping heap memory savings assertion as platform lacks tcmalloc");
  }
}

} // namespace
} // namespace Config
} // namespace Envoy

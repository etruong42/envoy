#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "envoy/config/custom_config_validators.h"
#include "envoy/config/subscription.h"
#include "envoy/grpc/status.h"
#include "envoy/service/discovery/v3/discovery.pb.h"

#include "source/common/common/assert.h"
#include "source/common/common/empty_string.h"
#include "source/common/config/xds_symbol_table.h"
#include "source/common/event/libevent.h"
#include "source/common/memory/stats.h"
#include "source/common/runtime/runtime_features.h"
#include "source/extensions/config_subscription/grpc/delta_subscription_state.h"
#include "source/extensions/config_subscription/grpc/watch_map.h"
#include "source/extensions/config_subscription/grpc/xds_mux/delta_subscription_state.h"
#include "source/extensions/config_subscription/grpc/xds_mux/sotw_subscription_state.h"

#include "test/benchmark/main.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"

namespace Envoy {
namespace Config {
namespace {

constexpr absl::string_view ClusterTypeUrl = "type.googleapis.com/envoy.config.cluster.v3.Cluster";
constexpr absl::string_view EndpointTypeUrl =
    "type.googleapis.com/envoy.config.endpoint.v3.ClusterLoadAssignment";
constexpr absl::string_view CompressFeatureFlag =
    "envoy.reloadable_features.compress_xds_resource_names";

class ScopedCompressFeatureOverride {
public:
  explicit ScopedCompressFeatureOverride(bool enabled)
      : previous_(Runtime::runtimeFeatureEnabled(CompressFeatureFlag)) {
    Runtime::maybeSetRuntimeGuard(CompressFeatureFlag, enabled);
  }
  ~ScopedCompressFeatureOverride() {
    Runtime::maybeSetRuntimeGuard(CompressFeatureFlag, previous_);
  }

private:
  const bool previous_;
};

bool initLibevent() {
  if (!Event::Libevent::Global::initialized()) {
    Event::Libevent::Global::initialize();
  }
  return true;
}

class NoopSubscriptionCallbacks : public SubscriptionCallbacks {
public:
  absl::Status onConfigUpdate(const std::vector<DecodedResourceRef>& resources,
                              const std::string&) override {
    updates_received_ += resources.size();
    return absl::OkStatus();
  }

  absl::Status onConfigUpdate(const std::vector<DecodedResourceRef>& added_resources,
                              const Protobuf::RepeatedPtrField<std::string>& removed_resources,
                              const std::string&) override {
    updates_received_ += added_resources.size() + removed_resources.size();
    return absl::OkStatus();
  }

  void onConfigUpdateFailed(ConfigUpdateFailureReason, const EnvoyException*) override {}

  uint64_t updatesReceived() const { return updates_received_; }

private:
  uint64_t updates_received_{0};
};

class NoopResourceDecoder : public OpaqueResourceDecoder {
public:
  ArenaWrappedProto<Protobuf::Message> decodeResource(const Protobuf::Any&) override {
    return ArenaWrappedProto<Protobuf::Message>(nullptr);
  }

  std::string resourceName(const Protobuf::Message&) override { return EMPTY_STRING; }
};

class NoopConfigValidators : public CustomConfigValidators {
public:
  void executeValidators(absl::string_view, const std::vector<DecodedResourcePtr>&) override {}
  void executeValidators(absl::string_view, const std::vector<DecodedResourcePtr>&,
                         const Protobuf::RepeatedPtrField<std::string>&) override {}
};

class BenchmarkEnv {
public:
  BenchmarkEnv()
      : api_((initLibevent(), Api::createApiForTest(time_system_))),
        dispatcher_(api_->allocateDispatcher("benchmark_thread")),
        shared_decoder_(std::make_shared<NoopResourceDecoder>()) {}

  Event::SimulatedTimeSystemHelper time_system_;
  Api::ApiPtr api_;
  Event::DispatcherPtr dispatcher_;
  NoopSubscriptionCallbacks callbacks_;
  NoopResourceDecoder decoder_;
  OpaqueResourceDecoderSharedPtr shared_decoder_;
  NoopConfigValidators validators_;
};

std::vector<std::string> generateXdstpNames(absl::string_view resource_type, size_t count) {
  static constexpr absl::string_view Authorities[] = {
      "traffic-director-global.xds.googleapis.com",
      "traffic-director-p2p.xds.googleapis.com",
      "traffic-director-c2p.xds.googleapis.com",
  };
  static constexpr absl::string_view Projects[] = {
      "1082309372360", "414017652406", "928374019283", "512398471029", "684729103847",
  };
  static constexpr absl::string_view Regions[] = {
      "europe-west1",
      "us-central1",
      "asia-east1",
      "us-east4",
  };

  std::vector<std::string> names;
  names.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const absl::string_view authority = Authorities[i % 3];
    const absl::string_view project = Projects[(i / 3) % 5];
    const absl::string_view region = Regions[(i / 15) % 4];
    names.push_back(absl::StrFormat("xdstp://%s/%s/%s/default/"
                                    "cloud-internal-17-lb-%s_cloud_17_ilb_%s_%019llu",
                                    authority, resource_type, project, region, project,
                                    static_cast<unsigned long long>(1000000000000000000ULL + i)));
  }
  return names;
}

std::vector<std::string> generateServiceMeshNames(absl::string_view prefix, size_t count) {
  static constexpr absl::string_view Projects[] = {
      "1082309372360", "414017652406", "928374019283", "512398471029", "684729103847",
  };
  static constexpr absl::string_view Regions[] = {
      "europe-west1",
      "us-central1",
      "asia-east1",
      "us-east4",
  };

  std::vector<std::string> names;
  names.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const absl::string_view project = Projects[(i / 3) % 5];
    const absl::string_view region = Regions[(i / 15) % 4];
    names.push_back(absl::StrFormat(
        "%s/projects/%s/locations/%s/namespaces/prod-workloads/services/backend-svc-%08zu:8080",
        prefix, project, region, i));
  }
  return names;
}

envoy::service::discovery::v3::DeltaDiscoveryResponse
buildDeltaResponse(absl::string_view type_url, const std::vector<std::string>& names,
                   size_t start_idx, size_t count, absl::string_view version,
                   absl::string_view nonce) {
  envoy::service::discovery::v3::DeltaDiscoveryResponse response;
  response.set_type_url(type_url);
  response.set_system_version_info(version);
  response.set_nonce(nonce);
  response.mutable_resources()->Reserve(static_cast<int>(count));
  for (size_t i = 0; i < count; ++i) {
    auto* res = response.add_resources();
    res->set_name(names[(start_idx + i) % names.size()]);
    res->set_version(version);
    res->mutable_resource()->set_type_url(type_url);
  }
  return response;
}

// -----------------------------------------------------------------------------
// 1. Full ADS CDS (wildcard) + EDS (per-cluster watches) setup & live heap size
// -----------------------------------------------------------------------------
void bmAdsSubscriptionSetupAndHeap(::benchmark::State& state) {
  const bool compress = state.range(0) != 0;
  const size_t requested_count = static_cast<size_t>(state.range(1));
  const size_t num_clusters = (Envoy::benchmark::skipExpensiveBenchmarks() && requested_count > 100)
                                  ? 100
                                  : requested_count;
  state.SetLabel(compress ? "compressed" : "baseline");

  ScopedCompressFeatureOverride guard(compress);

  BenchmarkEnv env;
  const auto cds_names = generateXdstpNames("envoy.config.cluster.v3.Cluster", num_clusters);
  const auto eds_names =
      generateXdstpNames("envoy.config.endpoint.v3.ClusterLoadAssignment", num_clusters);
  std::vector<absl::flat_hash_set<std::string>> per_cluster_eds_sets;
  per_cluster_eds_sets.reserve(num_clusters);
  for (const auto& name : eds_names) {
    per_cluster_eds_sets.push_back({name});
  }
  const absl::flat_hash_set<std::string> wildcard_set{"*"};

  auto cds_response =
      buildDeltaResponse(ClusterTypeUrl, cds_names, 0, num_clusters, "v1", "nonce-cds-1");
  auto eds_response =
      buildDeltaResponse(EndpointTypeUrl, eds_names, 0, num_clusters, "v1", "nonce-eds-1");

  uint64_t steady_state_heap_bytes = 0;
  size_t active_symbols = 0;

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    const uint64_t mem_before = Memory::Stats::totalCurrentlyAllocated();

    auto symbol_table = compress ? XdsSymbolTable::create() : nullptr;
    const OptRef<XdsSymbolTable> table_ref = makeOptRefFromPtr(symbol_table.get());

    // CDS: 1 wildcard watch + N clusters cached in DeltaSubscriptionState.
    WatchMap cds_watch_map(std::string(ClusterTypeUrl), &env.validators_, {}, table_ref);
    DeltaSubscriptionState cds_delta(std::string(ClusterTypeUrl), cds_watch_map, *env.dispatcher_,
                                     {}, table_ref);
    Watch* cds_watch = cds_watch_map.addWatch(env.callbacks_, env.decoder_);
    {
      AddedRemoved diff = cds_watch_map.updateWatchInterest(cds_watch, wildcard_set);
      cds_delta.updateSubscriptionInterest(diff.added_, diff.removed_);
      auto req = cds_delta.getNextRequestAckless();
      ::benchmark::DoNotOptimize(&req);
    }
    {
      UpdateAck ack = cds_delta.handleResponse(cds_response);
      ASSERT(ack.error_detail_.code() == Grpc::Status::WellKnownGrpcStatus::Ok);
    }

    // EDS: N per-cluster watches (mirroring EdsClusterImpl) + N requested resources in Delta state.
    WatchMap eds_watch_map(std::string(EndpointTypeUrl), &env.validators_, {}, table_ref);
    DeltaSubscriptionState eds_delta(std::string(EndpointTypeUrl), eds_watch_map, *env.dispatcher_,
                                     {}, table_ref);
    for (size_t i = 0; i < num_clusters; ++i) {
      Watch* eds_watch = eds_watch_map.addWatch(env.callbacks_, env.decoder_);
      AddedRemoved diff = eds_watch_map.updateWatchInterest(eds_watch, per_cluster_eds_sets[i]);
      eds_delta.updateSubscriptionInterest(diff.added_, diff.removed_);
    }
    {
      auto req = eds_delta.getNextRequestAckless();
      ::benchmark::DoNotOptimize(&req);
    }
    {
      UpdateAck ack = eds_delta.handleResponse(eds_response);
      ASSERT(ack.error_detail_.code() == Grpc::Status::WellKnownGrpcStatus::Ok);
    }

    ::benchmark::ClobberMemory();
    const uint64_t mem_after = Memory::Stats::totalCurrentlyAllocated();
    steady_state_heap_bytes = (mem_after >= mem_before) ? (mem_after - mem_before) : 0;
    active_symbols = symbol_table != nullptr ? symbol_table->numActiveSymbols() : 0;

    ::benchmark::DoNotOptimize(&cds_watch_map);
    ::benchmark::DoNotOptimize(&cds_delta);
    ::benchmark::DoNotOptimize(&eds_watch_map);
    ::benchmark::DoNotOptimize(&eds_delta);
  }

  state.counters["heap_bytes"] = static_cast<double>(steady_state_heap_bytes);
  state.counters["bytes_per_cluster"] =
      static_cast<double>(steady_state_heap_bytes) / static_cast<double>(num_clusters);
  state.counters["active_symbols"] = static_cast<double>(active_symbols);
}
BENCHMARK(bmAdsSubscriptionSetupAndHeap)
    ->Args({0, 10000})
    ->Args({1, 10000})
    ->Unit(::benchmark::kMillisecond);

// -----------------------------------------------------------------------------
// 2. Unified Mux (XdsMux::SotwSubscriptionState + XdsMux::DeltaSubscriptionState)
// -----------------------------------------------------------------------------
void bmUnifiedMuxSubscriptionHeap(::benchmark::State& state) {
  const bool compress = state.range(0) != 0;
  const size_t requested_count = static_cast<size_t>(state.range(1));
  const size_t num_resources =
      (Envoy::benchmark::skipExpensiveBenchmarks() && requested_count > 100) ? 100
                                                                             : requested_count;
  state.SetLabel(compress ? "compressed" : "baseline");

  ScopedCompressFeatureOverride guard(compress);

  BenchmarkEnv env;
  const auto cds_names = generateXdstpNames("envoy.config.cluster.v3.Cluster", num_resources);
  const auto eds_names =
      generateXdstpNames("envoy.config.endpoint.v3.ClusterLoadAssignment", num_resources);
  std::vector<absl::flat_hash_set<std::string>> per_eds_sets;
  per_eds_sets.reserve(num_resources);
  for (const auto& name : eds_names) {
    per_eds_sets.push_back({name});
  }
  const absl::flat_hash_set<std::string> all_cds_set(cds_names.begin(), cds_names.end());
  auto eds_response =
      buildDeltaResponse(EndpointTypeUrl, eds_names, 0, num_resources, "v1", "nonce-eds-1");

  uint64_t steady_state_heap_bytes = 0;

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    const uint64_t mem_before = Memory::Stats::totalCurrentlyAllocated();

    auto symbol_table = compress ? XdsSymbolTable::create() : nullptr;
    const OptRef<XdsSymbolTable> table_ref = makeOptRefFromPtr(symbol_table.get());

    // Unified SotW subscription tracking N explicit cluster names.
    WatchMap sotw_watch_map(std::string(ClusterTypeUrl), &env.validators_, {}, table_ref);
    XdsMux::SotwSubscriptionState sotw_state(
        std::string(ClusterTypeUrl), sotw_watch_map, *env.dispatcher_, env.shared_decoder_, {}, {},
        "traffic-director-global.xds.googleapis.com", table_ref);
    Watch* sotw_watch = sotw_watch_map.addWatch(env.callbacks_, env.decoder_);
    {
      AddedRemoved diff = sotw_watch_map.updateWatchInterest(sotw_watch, all_cds_set);
      sotw_state.updateSubscriptionInterest(diff.added_, diff.removed_);
      auto req = sotw_state.getNextRequestAckless();
      ::benchmark::DoNotOptimize(&req);
    }

    // Unified Delta subscription tracking N per-cluster EDS watches.
    WatchMap delta_watch_map(std::string(EndpointTypeUrl), &env.validators_, {}, table_ref);
    XdsMux::DeltaSubscriptionState delta_state(std::string(EndpointTypeUrl), delta_watch_map,
                                               *env.dispatcher_, {}, table_ref);
    for (size_t i = 0; i < num_resources; ++i) {
      Watch* watch = delta_watch_map.addWatch(env.callbacks_, env.decoder_);
      AddedRemoved diff = delta_watch_map.updateWatchInterest(watch, per_eds_sets[i]);
      delta_state.updateSubscriptionInterest(diff.added_, diff.removed_);
    }
    {
      auto req = delta_state.getNextRequestAckless();
      ::benchmark::DoNotOptimize(&req);
    }
    {
      UpdateAck ack = delta_state.handleResponse(eds_response);
      ASSERT(ack.error_detail_.code() == Grpc::Status::WellKnownGrpcStatus::Ok);
    }

    ::benchmark::ClobberMemory();
    const uint64_t mem_after = Memory::Stats::totalCurrentlyAllocated();
    steady_state_heap_bytes = (mem_after >= mem_before) ? (mem_after - mem_before) : 0;

    ::benchmark::DoNotOptimize(&sotw_watch_map);
    ::benchmark::DoNotOptimize(&sotw_state);
    ::benchmark::DoNotOptimize(&delta_watch_map);
    ::benchmark::DoNotOptimize(&delta_state);
  }

  state.counters["heap_bytes"] = static_cast<double>(steady_state_heap_bytes);
  state.counters["bytes_per_pair"] =
      static_cast<double>(steady_state_heap_bytes) / static_cast<double>(num_resources);
}
BENCHMARK(bmUnifiedMuxSubscriptionHeap)
    ->Args({0, 10000})
    ->Args({1, 10000})
    ->Unit(::benchmark::kMillisecond);

// -----------------------------------------------------------------------------
// 3. Steady-state incremental Delta update dispatch (100 updates / 10k watched)
// -----------------------------------------------------------------------------
void runDeltaIncrementalUpdateBenchmark(::benchmark::State& state, bool use_xdstp) {
  const bool compress = state.range(0) != 0;
  const size_t num_resources = Envoy::benchmark::skipExpensiveBenchmarks() ? 200 : 10000;
  constexpr size_t BatchSize = 100;
  state.SetLabel(compress ? "compressed" : "baseline");

  ScopedCompressFeatureOverride guard(compress);

  BenchmarkEnv env;
  const auto names =
      use_xdstp
          ? generateXdstpNames("envoy.config.endpoint.v3.ClusterLoadAssignment", num_resources)
          : generateServiceMeshNames("eds", num_resources);

  auto symbol_table = compress ? XdsSymbolTable::create() : nullptr;
  const OptRef<XdsSymbolTable> table_ref = makeOptRefFromPtr(symbol_table.get());

  WatchMap watch_map(std::string(EndpointTypeUrl), &env.validators_, {}, table_ref);
  DeltaSubscriptionState delta_state(std::string(EndpointTypeUrl), watch_map, *env.dispatcher_, {},
                                     table_ref);
  for (const auto& name : names) {
    Watch* watch = watch_map.addWatch(env.callbacks_, env.decoder_);
    AddedRemoved diff = watch_map.updateWatchInterest(watch, {name});
    delta_state.updateSubscriptionInterest(diff.added_, diff.removed_);
  }
  {
    auto req = delta_state.getNextRequestAckless();
    ::benchmark::DoNotOptimize(&req);
  }
  auto initial_response =
      buildDeltaResponse(EndpointTypeUrl, names, 0, num_resources, "v1", "nonce-init");
  {
    UpdateAck ack = delta_state.handleResponse(initial_response);
    ASSERT(ack.error_detail_.code() == Grpc::Status::WellKnownGrpcStatus::Ok);
  }

  // Pre-build two alternating incremental batches with distinct versions so updates are never
  // treated as heartbeats.
  auto batch_v2 = buildDeltaResponse(EndpointTypeUrl, names, 0, BatchSize, "v2", "nonce-2");
  auto batch_v3 = buildDeltaResponse(EndpointTypeUrl, names, BatchSize, BatchSize, "v3", "nonce-3");
  // Prime batch_v3 range to v1 (already v1), and alternate versions across iterations.
  uint64_t iter = 0;
  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    auto& msg = (iter % 2 == 0) ? batch_v2 : batch_v3;
    const std::string ver = absl::StrCat("v", iter + 2);
    for (auto& res : *msg.mutable_resources()) {
      res.set_version(ver);
    }
    UpdateAck ack = delta_state.handleResponse(msg);
    ::benchmark::DoNotOptimize(&ack);
    ++iter;
  }
  const uint64_t total_updates = env.callbacks_.updatesReceived();
  ::benchmark::DoNotOptimize(total_updates);
}

void bmDeltaIncrementalUpdateMeshNames(::benchmark::State& state) {
  runDeltaIncrementalUpdateBenchmark(state, /*use_xdstp=*/false);
}
BENCHMARK(bmDeltaIncrementalUpdateMeshNames)->Arg(0)->Arg(1)->Unit(::benchmark::kMicrosecond);

void bmDeltaIncrementalUpdateXdstpNames(::benchmark::State& state) {
  runDeltaIncrementalUpdateBenchmark(state, /*use_xdstp=*/true);
}
BENCHMARK(bmDeltaIncrementalUpdateXdstpNames)->Arg(0)->Arg(1)->Unit(::benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// 4. Watch churn: dynamically adding and removing 100 watches on a 10k WatchMap
// -----------------------------------------------------------------------------
void bmWatchChurn(::benchmark::State& state) {
  const bool compress = state.range(0) != 0;
  const size_t base_count = Envoy::benchmark::skipExpensiveBenchmarks() ? 200 : 10000;
  constexpr size_t ChurnBatch = 100;
  state.SetLabel(compress ? "compressed" : "baseline");

  ScopedCompressFeatureOverride guard(compress);

  BenchmarkEnv env;
  const auto all_names =
      generateXdstpNames("envoy.config.endpoint.v3.ClusterLoadAssignment", base_count + ChurnBatch);
  std::vector<absl::flat_hash_set<std::string>> churn_sets;
  churn_sets.reserve(ChurnBatch);
  for (size_t i = 0; i < ChurnBatch; ++i) {
    churn_sets.push_back({all_names[base_count + i]});
  }
  const absl::flat_hash_set<std::string> empty_set;

  auto symbol_table = compress ? XdsSymbolTable::create() : nullptr;
  const OptRef<XdsSymbolTable> table_ref = makeOptRefFromPtr(symbol_table.get());

  WatchMap watch_map(std::string(EndpointTypeUrl), &env.validators_, {}, table_ref);
  DeltaSubscriptionState delta_state(std::string(EndpointTypeUrl), watch_map, *env.dispatcher_, {},
                                     table_ref);
  for (size_t i = 0; i < base_count; ++i) {
    Watch* watch = watch_map.addWatch(env.callbacks_, env.decoder_);
    AddedRemoved diff = watch_map.updateWatchInterest(watch, {all_names[i]});
    delta_state.updateSubscriptionInterest(diff.added_, diff.removed_);
  }
  {
    auto req = delta_state.getNextRequestAckless();
    ::benchmark::DoNotOptimize(&req);
  }

  std::vector<Watch*> transient_watches(ChurnBatch, nullptr);
  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    for (size_t i = 0; i < ChurnBatch; ++i) {
      transient_watches[i] = watch_map.addWatch(env.callbacks_, env.decoder_);
      AddedRemoved add_diff = watch_map.updateWatchInterest(transient_watches[i], churn_sets[i]);
      delta_state.updateSubscriptionInterest(add_diff.added_, add_diff.removed_);
    }
    for (size_t i = 0; i < ChurnBatch; ++i) {
      AddedRemoved rem_diff = watch_map.updateWatchInterest(transient_watches[i], empty_set);
      delta_state.updateSubscriptionInterest(rem_diff.added_, rem_diff.removed_);
      watch_map.removeWatch(transient_watches[i]);
    }
    auto req = delta_state.getNextRequestAckless();
    ::benchmark::DoNotOptimize(&req);
  }

  state.counters["active_symbols"] =
      static_cast<double>(symbol_table != nullptr ? symbol_table->numActiveSymbols() : 0);
  state.counters["allocated_symbols"] =
      static_cast<double>(symbol_table != nullptr ? symbol_table->numSymbols() : 0);
}
BENCHMARK(bmWatchChurn)->Arg(0)->Arg(1)->Unit(::benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// 5. Stream reconnect: materializing initial_resource_versions for 10k resources
// -----------------------------------------------------------------------------
void bmDeltaStreamReconnectInitialVersions(::benchmark::State& state) {
  const bool compress = state.range(0) != 0;
  const size_t num_resources = Envoy::benchmark::skipExpensiveBenchmarks() ? 200 : 10000;
  state.SetLabel(compress ? "compressed" : "baseline");

  ScopedCompressFeatureOverride guard(compress);

  BenchmarkEnv env;
  const auto names =
      generateXdstpNames("envoy.config.endpoint.v3.ClusterLoadAssignment", num_resources);

  auto symbol_table = compress ? XdsSymbolTable::create() : nullptr;
  const OptRef<XdsSymbolTable> table_ref = makeOptRefFromPtr(symbol_table.get());

  WatchMap watch_map(std::string(EndpointTypeUrl), &env.validators_, {}, table_ref);
  DeltaSubscriptionState delta_state(std::string(EndpointTypeUrl), watch_map, *env.dispatcher_, {},
                                     table_ref);
  for (const auto& name : names) {
    Watch* watch = watch_map.addWatch(env.callbacks_, env.decoder_);
    AddedRemoved diff = watch_map.updateWatchInterest(watch, {name});
    delta_state.updateSubscriptionInterest(diff.added_, diff.removed_);
  }
  {
    auto req = delta_state.getNextRequestAckless();
    ::benchmark::DoNotOptimize(&req);
  }
  auto initial_response =
      buildDeltaResponse(EndpointTypeUrl, names, 0, num_resources, "v1", "nonce-init");
  {
    UpdateAck ack = delta_state.handleResponse(initial_response);
    ASSERT(ack.error_detail_.code() == Grpc::Status::WellKnownGrpcStatus::Ok);
  }

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    delta_state.markStreamFresh(/*should_send_initial_resource_versions=*/true);
    auto reconnect_req = delta_state.getNextRequestAckless();
    ::benchmark::DoNotOptimize(&reconnect_req);
  }
}
BENCHMARK(bmDeltaStreamReconnectInitialVersions)->Arg(0)->Arg(1)->Unit(::benchmark::kMillisecond);

} // namespace
} // namespace Config
} // namespace Envoy

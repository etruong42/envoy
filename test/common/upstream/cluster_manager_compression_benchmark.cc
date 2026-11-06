#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "envoy/common/optref.h"
#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/config/core/v3/base.pb.h"
#include "envoy/config/core/v3/config_source.pb.h"
#include "envoy/config/core/v3/protocol.pb.h"
#include "envoy/config/subscription.h"
#include "envoy/config/typed_metadata.h"
#include "envoy/config/xds_manager.h"
#include "envoy/event/dispatcher.h"
#include "envoy/http/codec.h"
#include "envoy/stats/scope.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/thread_local/thread_local_object.h"
#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/load_balancer.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"
#include "source/common/common/empty_string.h"
#include "source/common/config/null_grpc_mux_impl.h"
#include "source/common/config/xds_symbol_table.h"
#include "source/common/memory/stats.h"
#include "source/common/runtime/runtime_features.h"
#include "source/common/upstream/upstream_impl.h"

#include "test/benchmark/main.h"
#include "test/common/upstream/test_cluster_manager.h"
#include "test/mocks/upstream/cluster_info.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"

namespace Envoy {
namespace Upstream {
namespace {

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

std::vector<std::string> generateXdstpClusterNames(size_t count, size_t start_offset = 0) {
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
    const size_t idx = start_offset + i;
    const absl::string_view authority = Authorities[idx % 3];
    const absl::string_view project = Projects[(idx / 3) % 5];
    const absl::string_view region = Regions[(idx / 15) % 4];
    names.push_back(absl::StrFormat("xdstp://%s/envoy.config.cluster.v3.Cluster/%s/default/"
                                    "cloud-internal-17-lb-%s_cloud_17_ilb_%s_%019llu",
                                    authority, project, region, project,
                                    static_cast<unsigned long long>(1000000000000000000ULL + idx)));
  }
  return names;
}

// Non-mock XdsManager that avoids gmock overhead on hot-path xdsSymbolTable() and adsMux() calls.
class BenchmarkXdsManager : public Config::XdsManager {
public:
  BenchmarkXdsManager()
      : symbol_table_(Config::XdsSymbolTable::create()),
        ads_mux_(std::make_shared<Config::NullGrpcMuxImpl>()) {}

  absl::Status initialize(const envoy::config::bootstrap::v3::Bootstrap&,
                          ClusterManager*) override {
    return absl::OkStatus();
  }
  absl::Status initializeAdsConnections(const envoy::config::bootstrap::v3::Bootstrap&) override {
    return absl::OkStatus();
  }
  void startXdstpAdsMuxes() override {}
  absl::StatusOr<Config::SubscriptionPtr> subscribeToSingletonResource(
      absl::string_view, OptRef<const envoy::config::core::v3::ConfigSource>, absl::string_view,
      Stats::Scope&, Config::SubscriptionCallbacks&, Config::OpaqueResourceDecoderSharedPtr,
      const Config::SubscriptionOptions&) override {
    return absl::UnimplementedError("not implemented");
  }
  absl::StatusOr<std::unique_ptr<Config::SingletonSubscription>> subscribeToSingletonResource(
      absl::string_view, OptRef<const envoy::config::core::v3::ConfigSource>, absl::string_view,
      Stats::Scope&, Config::SingletonSubscriptionCallbacks&,
      Config::OpaqueResourceDecoderSharedPtr, const Config::SubscriptionOptions&) override {
    return absl::UnimplementedError("not implemented");
  }
  Config::ScopedResume pause(const std::string&) override {
    return std::make_unique<Cleanup>([] {});
  }
  Config::ScopedResume pause(const std::vector<std::string>&) override {
    return std::make_unique<Cleanup>([] {});
  }
  void shutdown() override {}
  absl::Status setAdsConfigSource(const envoy::config::core::v3::ApiConfigSource&) override {
    return absl::OkStatus();
  }
  Config::GrpcMuxSharedPtr adsMux() override { return ads_mux_; }
  Config::SubscriptionFactory& subscriptionFactory() override { return subscription_factory_; }
  Config::XdsSymbolTable& xdsSymbolTable() override { return *symbol_table_; }

private:
  Config::XdsSymbolTableSharedPtr symbol_table_;
  Config::GrpcMuxSharedPtr ads_mux_;
  testing::NiceMock<Config::MockSubscriptionFactory> subscription_factory_;
};

// Non-mock multi-worker ThreadLocal::Instance and Slot that fan out ThreadLocalClusterManagerImpl
// creation and updates across N simulated worker threads on the benchmark thread.
class MultiWorkerThreadLocal : public ThreadLocal::Instance {
public:
  class MultiWorkerSlot : public ThreadLocal::Slot {
  public:
    MultiWorkerSlot(MultiWorkerThreadLocal& parent, size_t num_workers)
        : parent_(parent), workers_(num_workers) {}

    ~MultiWorkerSlot() override {
      if (!parent_.shutdown_) {
        workers_.clear();
      }
    }

    bool currentThreadRegistered() override { return true; }

    ThreadLocal::ThreadLocalObjectSharedPtr get() override {
      return workers_[parent_.active_worker_];
    }

    void set(InitializeCb cb) override {
      for (auto& worker : workers_) {
        worker = cb(parent_.dispatcher_);
      }
    }

    void runOnAllThreads(const UpdateCb& update_cb) override {
      for (auto& worker : workers_) {
        update_cb(worker);
      }
    }

    void runOnAllThreads(const UpdateCb& update_cb,
                         const std::function<void()>& complete_cb) override {
      for (auto& worker : workers_) {
        update_cb(worker);
      }
      complete_cb();
    }

    bool isShutdown() const override { return parent_.shutdown_; }

    void clearWorkers() { workers_.clear(); }

  private:
    MultiWorkerThreadLocal& parent_;
    std::vector<ThreadLocal::ThreadLocalObjectSharedPtr> workers_;
  };

  explicit MultiWorkerThreadLocal(Event::Dispatcher& dispatcher, size_t num_workers = 1)
      : dispatcher_(dispatcher), num_workers_(num_workers) {}

  ~MultiWorkerThreadLocal() override { shutdownThread(); }

  ThreadLocal::SlotSharedPtr allocateSlot() override {
    auto slot = std::make_shared<MultiWorkerSlot>(*this, num_workers_);
    slots_.push_back(slot);
    return slot;
  }

  void registerThread(Event::Dispatcher&, bool) override {}
  void shutdownGlobalThreading() override { shutdown_ = true; }
  void shutdownThread() override {
    shutdown_ = true;
    for (auto& slot : slots_) {
      if (auto locked = slot.lock(); locked != nullptr) {
        locked->clearWorkers();
      }
    }
    slots_.clear();
  }
  Event::Dispatcher& dispatcher() override { return dispatcher_; }
  bool isShutdown() const override { return shutdown_; }

  void setNumWorkers(size_t num_workers) { num_workers_ = num_workers; }
  void setActiveWorker(size_t worker_idx) { active_worker_ = worker_idx; }

private:
  Event::Dispatcher& dispatcher_;
  size_t num_workers_;
  size_t active_worker_{0};
  bool shutdown_{false};
  std::vector<std::weak_ptr<MultiWorkerSlot>> slots_;
};

class NoopLoadBalancerFactory : public LoadBalancerFactory {
public:
  LoadBalancerPtr create(LoadBalancerParams) override { return nullptr; }
  bool recreateOnHostChangeDeprecated() const override { return false; }
};

class NoopThreadAwareLoadBalancer : public ThreadAwareLoadBalancer {
public:
  explicit NoopThreadAwareLoadBalancer(LoadBalancerFactorySharedPtr factory)
      : factory_(std::move(factory)) {}

  LoadBalancerFactorySharedPtr factory() override { return factory_; }
  absl::Status initialize() override { return absl::OkStatus(); }

private:
  LoadBalancerFactorySharedPtr factory_;
};

class NoopTypedLoadBalancerFactory : public TypedLoadBalancerFactory {
public:
  NoopTypedLoadBalancerFactory() : lb_factory_(std::make_shared<NoopLoadBalancerFactory>()) {}

  ThreadAwareLoadBalancerPtr create(OptRef<const LoadBalancerConfig>, const ClusterInfo&,
                                    const PrioritySet&, Runtime::Loader&, Random::RandomGenerator&,
                                    TimeSource&) override {
    return std::make_unique<NoopThreadAwareLoadBalancer>(lb_factory_);
  }

  ProtobufTypes::MessagePtr createEmptyConfigProto() override { return nullptr; }

  absl::StatusOr<LoadBalancerConfigPtr> loadConfig(Server::Configuration::ServerFactoryContext&,
                                                   const Protobuf::Message&) override {
    return nullptr;
  }

  std::string name() const override { return "envoy.load_balancing_policies.round_robin"; }
  std::string category() const override { return "envoy.load_balancing_policies"; }

private:
  LoadBalancerFactorySharedPtr lb_factory_;
};

struct SharedClusterResources {
  mutable testing::NiceMock<MockClusterInfo> template_info_;
  mutable NoopTypedLoadBalancerFactory typed_lb_factory_;
  envoy::config::cluster::v3::Cluster::CommonLbConfig common_lb_config_;
  envoy::config::core::v3::Metadata empty_metadata_;
  std::vector<Http::Protocol> default_protocols_{Http::Protocol::Http11};
};

// Lightweight ClusterInfo implementation that allocates only the cluster name and optional
// compressed XdsResourceNameStorage (matching ClusterInfoImpl) while sharing static stats/configs.
class BenchmarkClusterInfo : public ClusterInfo {
public:
  BenchmarkClusterInfo(const SharedClusterResources& shared, absl::string_view name,
                       Config::XdsSymbolTable& symbol_table, bool added_via_api)
      : shared_(shared), name_(name), added_via_api_(added_via_api) {
    if (Runtime::runtimeFeatureEnabled(CompressFeatureFlag)) {
      xds_resource_name_storage_.emplace(symbol_table.encode(name_));
      xds_resource_name_ = xds_resource_name_storage_->resourceName();
    }
  }

  bool addedViaApi() const override { return added_via_api_; }
  std::chrono::milliseconds connectTimeout() const override {
    return std::chrono::milliseconds(250);
  }
  const std::optional<std::chrono::milliseconds> idleTimeout() const override {
    return std::nullopt;
  }
  const std::optional<std::chrono::milliseconds> tcpPoolIdleTimeout() const override {
    return std::nullopt;
  }
  const std::optional<std::chrono::milliseconds> maxConnectionDuration() const override {
    return std::nullopt;
  }
  float perUpstreamPreconnectRatio() const override { return 1.0f; }
  float peekaheadRatio() const override { return 1.0f; }
  bool shouldPreconnect(const Host&) const override { return false; }
  uint32_t perConnectionBufferLimitBytes() const override { return 0; }
  std::chrono::milliseconds perConnectionBufferHighWatermarkTimeout() const override {
    return std::chrono::milliseconds(0);
  }
  uint64_t features() const override { return 0; }
  const HttpProtocolOptionsConfig& httpProtocolOptions() const override {
    return shared_.template_info_.httpProtocolOptions();
  }
  ProtocolOptionsConfigConstSharedPtr extensionProtocolOptions(const std::string&) const override {
    return nullptr;
  }
  OptRef<const LoadBalancerConfig> loadBalancerConfig() const override { return std::nullopt; }
  TypedLoadBalancerFactory& loadBalancerFactory() const override {
    return shared_.typed_lb_factory_;
  }
  const envoy::config::cluster::v3::Cluster::CommonLbConfig& lbConfig() const override {
    return shared_.common_lb_config_;
  }
  std::optional<bool> processHttpForOutlierDetection(Http::ResponseHeaderMap&) const override {
    return std::nullopt;
  }
  envoy::config::cluster::v3::Cluster::DiscoveryType type() const override {
    return envoy::config::cluster::v3::Cluster::STATIC;
  }
  OptRef<const envoy::config::cluster::v3::Cluster::CustomClusterType>
  clusterType() const override {
    return std::nullopt;
  }
  OptRef<const Http::ClientCodecFactory> upstreamHttpClientCodecFactory() const override {
    return std::nullopt;
  }
  OptRef<const envoy::config::core::v3::TypedExtensionConfig> upstreamConfig() const override {
    return std::nullopt;
  }
  bool maintenanceMode() const override { return false; }
  uint32_t maxRequestsPerConnection() const override { return 0; }
  uint32_t maxResponseHeadersCount() const override { return Http::DEFAULT_MAX_HEADERS_COUNT; }
  std::optional<uint16_t> maxResponseHeadersKb() const override { return std::nullopt; }
  const std::string& name() const override { return name_; }
  OptRef<const Config::XdsResourceName> xdsResourceName() const override {
    if (xds_resource_name_storage_.has_value()) {
      return xds_resource_name_;
    }
    return std::nullopt;
  }
  const std::string& observabilityName() const override { return name_; }
  ResourceManager& resourceManager(ResourcePriority) const override {
    return *shared_.template_info_.resource_manager_;
  }
  TransportSocketMatcher& transportSocketMatcher() const override {
    return *shared_.template_info_.transport_socket_matcher_;
  }
  ClusterConfigUpdateStats& configUpdateStats() const override {
    return const_cast<ClusterConfigUpdateStats&>(shared_.template_info_.config_update_stats_);
  }
  ClusterLbStats& lbStats() const override {
    return const_cast<ClusterLbStats&>(shared_.template_info_.lb_stats_);
  }
  ClusterEndpointStats& endpointStats() const override {
    return const_cast<ClusterEndpointStats&>(shared_.template_info_.endpoint_stats_);
  }
  DeferredCreationCompatibleClusterTrafficStats& trafficStats() const override {
    return shared_.template_info_.traffic_stats_;
  }
  Stats::Scope& statsScope() const override {
    return *shared_.template_info_.stats_store_.rootScope();
  }
  ClusterLoadReportStats& loadReportStats() const override {
    return const_cast<ClusterLoadReportStats&>(shared_.template_info_.load_report_stats_);
  }
  ClusterRequestResponseSizeStatsOptRef requestResponseSizeStats() const override {
    return std::nullopt;
  }
  ClusterTimeoutBudgetStatsOptRef timeoutBudgetStats() const override { return std::nullopt; }
  bool perEndpointStatsEnabled() const override { return false; }
  UpstreamLocalAddressSelectorConstSharedPtr getUpstreamLocalAddressSelector() const override {
    return shared_.template_info_.upstream_local_address_selector_;
  }
  const envoy::config::core::v3::Metadata& metadata() const override {
    return shared_.empty_metadata_;
  }
  const Envoy::Config::TypedMetadata& typedMetadata() const override {
    return shared_.template_info_.typedMetadata();
  }
  bool drainConnectionsOnHostRemoval() const override { return false; }
  bool connectionPoolPerDownstreamConnection() const override { return false; }
  bool warmHosts() const override { return false; }
  bool waitForWarmOnInit() const override { return true; }
  bool setLocalInterfaceNameOnUpstreamConnections() const override { return false; }
  const std::string& edsServiceName() const override { return EMPTY_STRING; }
  void createNetworkFilterChain(Network::Connection&) const override {}
  std::vector<Http::Protocol> upstreamHttpProtocol(std::optional<Http::Protocol>) const override {
    return shared_.default_protocols_;
  }
  bool createFilterChain(Http::FilterChainFactoryCallbacks&) const override { return false; }
  bool createUpgradeFilterChain(absl::string_view, const Http::FilterChainFactory::UpgradeMap*,
                                Http::FilterChainFactoryCallbacks&) const override {
    return false;
  }
  Http::Http1::CodecStats& http1CodecStats() const override {
    return shared_.template_info_.http1CodecStats();
  }
  Http::Http2::CodecStats& http2CodecStats() const override {
    return shared_.template_info_.http2CodecStats();
  }
  Http::Http3::CodecStats& http3CodecStats() const override {
    return shared_.template_info_.http3CodecStats();
  }
  Http::ClientHeaderValidatorPtr makeHeaderValidator(Http::Protocol) const override {
    return nullptr;
  }
  OptRef<const envoy::config::cluster::v3::UpstreamConnectionOptions::HappyEyeballsConfig>
  happyEyeballsConfig() const override {
    return std::nullopt;
  }
  OptRef<const std::vector<std::string>> lrsReportMetricNames() const override {
    return std::nullopt;
  }
  OptRef<const ClusterInfo::PendingRqQueuePolicy> pendingRqQueuePolicy() const override {
    return std::nullopt;
  }

private:
  const SharedClusterResources& shared_;
  const std::string name_;
  std::optional<Config::XdsResourceNameStorage> xds_resource_name_storage_;
  Config::XdsResourceName xds_resource_name_;
  const bool added_via_api_;
};

class BenchmarkCluster : public Cluster {
public:
  explicit BenchmarkCluster(ClusterInfoConstSharedPtr info) : info_(std::move(info)) {
    priority_set_.getOrCreateHostSet(0);
  }

  HealthChecker* healthChecker() override { return nullptr; }
  ClusterInfoConstSharedPtr info() const override { return info_; }
  Outlier::Detector* outlierDetector() override { return nullptr; }
  const Outlier::Detector* outlierDetector() const override { return nullptr; }
  void initialize(std::function<absl::Status()> callback) override { THROW_IF_NOT_OK(callback()); }
  InitializePhase initializePhase() const override { return InitializePhase::Primary; }
  PrioritySet& prioritySet() override { return priority_set_; }
  const PrioritySet& prioritySet() const override { return priority_set_; }
  UnitFloat dropOverload() const override { return UnitFloat(0); }
  const std::string& dropCategory() const override { return EMPTY_STRING; }
  void setDropOverload(UnitFloat) override {}
  void setDropCategory(absl::string_view) override {}

private:
  ClusterInfoConstSharedPtr info_;
  PrioritySetImpl priority_set_;
};

class BenchmarkClusterManagerEnv {
public:
  explicit BenchmarkClusterManagerEnv(size_t num_workers, bool deferred_cluster_creation = false)
      : multi_worker_tls_(factory_.dispatcher_, num_workers) {
    ON_CALL(factory_.server_context_, xdsManager()).WillByDefault(testing::ReturnRef(xds_manager_));
    ON_CALL(factory_.server_context_, threadLocal())
        .WillByDefault(testing::ReturnRef(multi_worker_tls_));

    ON_CALL(factory_, clusterFromProto_(testing::_, testing::_, testing::_))
        .WillByDefault(testing::Invoke(
            [this](const envoy::config::cluster::v3::Cluster& cluster,
                   Outlier::EventLoggerSharedPtr,
                   bool added_via_api) -> std::pair<ClusterSharedPtr, ThreadAwareLoadBalancer*> {
              auto info = std::make_shared<BenchmarkClusterInfo>(
                  shared_resources_, cluster.name(), xds_manager_.xdsSymbolTable(), added_via_api);
              return std::make_pair(std::make_shared<BenchmarkCluster>(std::move(info)), nullptr);
            }));

    bootstrap_.mutable_cluster_manager()->set_enable_deferred_cluster_creation(
        deferred_cluster_creation);
  }

  ~BenchmarkClusterManagerEnv() {
    if (cluster_manager_ != nullptr) {
      cluster_manager_->shutdown();
      cluster_manager_.reset();
    }
    multi_worker_tls_.shutdownThread();
  }

  void initClusterManager() {
    cluster_manager_ = TestClusterManagerImpl::createTestClusterManager(bootstrap_, factory_,
                                                                        factory_.server_context_);
    ON_CALL(factory_.server_context_, clusterManager())
        .WillByDefault(testing::ReturnRef(*cluster_manager_));
    THROW_IF_NOT_OK(cluster_manager_->initialize(bootstrap_));
    cluster_manager_->setPrimaryClustersInitializedCb(
        [this]() { THROW_IF_NOT_OK(cluster_manager_->initializeSecondaryClusters(bootstrap_)); });
  }

  Config::XdsSymbolTable& symbolTable() { return xds_manager_.xdsSymbolTable(); }

  SharedClusterResources shared_resources_;
  BenchmarkXdsManager xds_manager_;
  testing::NiceMock<TestClusterManagerFactory> factory_;
  MultiWorkerThreadLocal multi_worker_tls_;
  envoy::config::bootstrap::v3::Bootstrap bootstrap_;
  std::unique_ptr<TestClusterManagerImpl> cluster_manager_;
};

std::vector<envoy::config::cluster::v3::Cluster>
buildClusterProtos(const std::vector<std::string>& names) {
  std::vector<envoy::config::cluster::v3::Cluster> protos;
  protos.reserve(names.size());
  for (const auto& name : names) {
    envoy::config::cluster::v3::Cluster cluster;
    cluster.set_name(name);
    cluster.set_type(envoy::config::cluster::v3::Cluster::STATIC);
    cluster.mutable_connect_timeout()->set_seconds(1);
    protos.push_back(std::move(cluster));
  }
  return protos;
}

// -----------------------------------------------------------------------------
// 1. Multi-worker ClusterManager steady-state TCMalloc heap footprint & fanout
//    Arg(0): compress (0 = baseline, 1 = compressed)
//    Arg(1): deferred_cluster_creation (0 = active, 1 = deferred)
//    Arg(2): num_workers (1 vs 32 worker threads)
// -----------------------------------------------------------------------------
void bmMultiWorkerClusterMapMemory(::benchmark::State& state) {
  const bool compress = state.range(0) != 0;
  const bool deferred = state.range(1) != 0;
  const size_t num_workers = static_cast<size_t>(state.range(2));
  const size_t num_clusters = Envoy::benchmark::skipExpensiveBenchmarks() ? 50 : 1000;
  state.SetLabel(absl::StrFormat("%s/%s/%zu_workers", compress ? "compressed" : "baseline",
                                 deferred ? "deferred" : "active", num_workers));

  ScopedCompressFeatureOverride guard(compress);

  const auto names = generateXdstpClusterNames(num_clusters);
  const auto cluster_protos = buildClusterProtos(names);

  uint64_t steady_state_heap_bytes = 0;
  size_t active_symbols = 0;
  size_t allocated_symbols = 0;

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    BenchmarkClusterManagerEnv env(num_workers, deferred);
    env.initClusterManager();

    const uint64_t mem_before = Memory::Stats::totalCurrentlyAllocated();
    for (size_t i = 0; i < num_clusters; ++i) {
      auto status_or_added = env.cluster_manager_->addOrUpdateCluster(cluster_protos[i], "v1");
      ASSERT(status_or_added.ok() && *status_or_added);
    }
    const uint64_t mem_after = Memory::Stats::totalCurrentlyAllocated();
    steady_state_heap_bytes = (mem_after >= mem_before) ? (mem_after - mem_before) : 0;
    active_symbols = env.symbolTable().numActiveSymbols();
    allocated_symbols = env.symbolTable().numSymbols();
  }

  state.counters["heap_delta_bytes"] = static_cast<double>(steady_state_heap_bytes);
  state.counters["bytes_per_cluster"] =
      static_cast<double>(steady_state_heap_bytes) / static_cast<double>(num_clusters);
  state.counters["active_symbols"] = static_cast<double>(compress ? active_symbols : 0);
  state.counters["allocated_symbols"] = static_cast<double>(compress ? allocated_symbols : 0);
}
BENCHMARK(bmMultiWorkerClusterMapMemory)
    ->Args({0, 0, 1})
    ->Args({1, 0, 1})
    ->Args({0, 0, 32})
    ->Args({1, 0, 32})
    ->Args({0, 1, 32})
    ->Args({1, 1, 32})
    ->Unit(::benchmark::kMillisecond);

// -----------------------------------------------------------------------------
// 2. Per-request worker-thread cluster lookup (getThreadLocalCluster vs
//    getThreadLocalClusterByResourceName) across 2,000 active clusters
//    Arg(0):
//      0 = baseline getThreadLocalCluster(string_view)
//      1 = compressed getThreadLocalCluster(string_view) via tryEncode
//      2 = compressed getThreadLocalClusterByResourceName(XdsResourceName)
// -----------------------------------------------------------------------------
void bmGetThreadLocalClusterHit(::benchmark::State& state) {
  const int64_t mode = state.range(0);
  const bool compress = (mode != 0);
  const size_t num_clusters = Envoy::benchmark::skipExpensiveBenchmarks() ? 100 : 2000;
  if (mode == 0) {
    state.SetLabel("baseline_string");
  } else if (mode == 1) {
    state.SetLabel("compressed_string_try_encode");
  } else {
    state.SetLabel("compressed_by_resource_name");
  }

  ScopedCompressFeatureOverride guard(compress);

  BenchmarkClusterManagerEnv env(/*num_workers=*/1, /*deferred_cluster_creation=*/false);
  env.initClusterManager();

  const auto names = generateXdstpClusterNames(num_clusters);
  const auto cluster_protos = buildClusterProtos(names);
  for (size_t i = 0; i < num_clusters; ++i) {
    auto status_or_added = env.cluster_manager_->addOrUpdateCluster(cluster_protos[i], "v1");
    ASSERT(status_or_added.ok() && *status_or_added);
  }

  std::vector<Config::XdsResourceName> resource_names;
  if (mode == 2) {
    resource_names.reserve(num_clusters);
    for (const auto& name : names) {
      auto cluster_ref = env.cluster_manager_->getActiveCluster(name);
      ASSERT(cluster_ref.has_value());
      ASSERT(cluster_ref->info()->xdsResourceName().has_value());
      resource_names.push_back(*cluster_ref->info()->xdsResourceName());
    }
  }

  size_t idx = 0;
  if (mode == 2) {
    for (auto _ : state) {
      UNREFERENCED_PARAMETER(_);
      auto* tl_cluster =
          env.cluster_manager_->getThreadLocalClusterByResourceName(resource_names[idx]);
      ::benchmark::DoNotOptimize(tl_cluster);
      idx = (idx + 1 == num_clusters) ? 0 : (idx + 1);
    }
  } else {
    for (auto _ : state) {
      UNREFERENCED_PARAMETER(_);
      auto* tl_cluster = env.cluster_manager_->getThreadLocalCluster(names[idx]);
      ::benchmark::DoNotOptimize(tl_cluster);
      idx = (idx + 1 == num_clusters) ? 0 : (idx + 1);
    }
  }
}
BENCHMARK(bmGetThreadLocalClusterHit)->Arg(0)->Arg(1)->Arg(2)->Unit(::benchmark::kNanosecond);

// -----------------------------------------------------------------------------
// 3. Per-request worker-thread lookup for untrusted/unknown cluster names
//    (e.g. cluster_header / Lua) verifying fast-fail and zero symbol growth
//    Arg(0):
//      0 = baseline miss (untrusted xdstp:// name)
//      1 = compressed miss (untrusted header name failing on first token)
//      2 = compressed miss (unknown xdstp:// leaf token failing on last token)
// -----------------------------------------------------------------------------
void bmGetThreadLocalClusterMissUntrustedName(::benchmark::State& state) {
  const int64_t mode = state.range(0);
  const bool compress = (mode != 0);
  const size_t num_clusters = Envoy::benchmark::skipExpensiveBenchmarks() ? 100 : 2000;
  if (mode == 0) {
    state.SetLabel("baseline_miss");
  } else if (mode == 1) {
    state.SetLabel("compressed_miss_untrusted_prefix");
  } else {
    state.SetLabel("compressed_miss_unknown_leaf");
  }

  ScopedCompressFeatureOverride guard(compress);

  BenchmarkClusterManagerEnv env(/*num_workers=*/1, /*deferred_cluster_creation=*/false);
  env.initClusterManager();

  const auto names = generateXdstpClusterNames(num_clusters);
  const auto cluster_protos = buildClusterProtos(names);
  for (size_t i = 0; i < num_clusters; ++i) {
    auto status_or_added = env.cluster_manager_->addOrUpdateCluster(cluster_protos[i], "v1");
    ASSERT(status_or_added.ok() && *status_or_added);
  }

  std::vector<std::string> unknown_names;
  if (mode == 1) {
    unknown_names.reserve(256);
    for (size_t i = 0; i < 256; ++i) {
      unknown_names.push_back(
          absl::StrFormat("untrusted_header_cluster_%04zu.example.internal", i));
    }
  } else {
    unknown_names = generateXdstpClusterNames(256, /*start_offset=*/1000000);
  }
  const size_t symbols_before = env.symbolTable().numSymbols();

  size_t idx = 0;
  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    auto* tl_cluster = env.cluster_manager_->getThreadLocalCluster(unknown_names[idx]);
    ::benchmark::DoNotOptimize(tl_cluster);
    idx = (idx + 1) & 255;
  }

  ASSERT(env.symbolTable().numSymbols() == symbols_before);
  state.counters["symbol_growth"] =
      static_cast<double>(env.symbolTable().numSymbols() - symbols_before);
}
BENCHMARK(bmGetThreadLocalClusterMissUntrustedName)
    ->Arg(0)
    ->Arg(1)
    ->Arg(2)
    ->Unit(::benchmark::kNanosecond);

// -----------------------------------------------------------------------------
// 4. Multi-worker dynamic CDS cluster churn (adding and removing 50 clusters
//    across 16 workers on a 1,000-cluster ClusterManager)
//    Arg(0): compress (0 = baseline, 1 = compressed)
// -----------------------------------------------------------------------------
void bmMultiWorkerDynamicClusterChurn(::benchmark::State& state) {
  const bool compress = state.range(0) != 0;
  const size_t base_count = Envoy::benchmark::skipExpensiveBenchmarks() ? 50 : 1000;
  const size_t churn_batch = Envoy::benchmark::skipExpensiveBenchmarks() ? 10 : 50;
  constexpr size_t NumWorkers = 16;
  state.SetLabel(compress ? "compressed" : "baseline");

  ScopedCompressFeatureOverride guard(compress);

  BenchmarkClusterManagerEnv env(NumWorkers, /*deferred_cluster_creation=*/false);
  env.initClusterManager();

  const auto all_names = generateXdstpClusterNames(base_count + churn_batch);
  const auto all_protos = buildClusterProtos(all_names);
  for (size_t i = 0; i < base_count; ++i) {
    auto status_or_added = env.cluster_manager_->addOrUpdateCluster(all_protos[i], "v1");
    ASSERT(status_or_added.ok() && *status_or_added);
  }

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    for (size_t i = 0; i < churn_batch; ++i) {
      auto status_or_added =
          env.cluster_manager_->addOrUpdateCluster(all_protos[base_count + i], "v1");
      ::benchmark::DoNotOptimize(status_or_added);
    }
    for (size_t i = 0; i < churn_batch; ++i) {
      bool removed = env.cluster_manager_->removeCluster(all_names[base_count + i]);
      ::benchmark::DoNotOptimize(removed);
    }
  }

  state.counters["active_symbols"] =
      static_cast<double>(compress ? env.symbolTable().numActiveSymbols() : 0);
  state.counters["allocated_symbols"] =
      static_cast<double>(compress ? env.symbolTable().numSymbols() : 0);
}
BENCHMARK(bmMultiWorkerDynamicClusterChurn)->Arg(0)->Arg(1)->Unit(::benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// 5. Deferred cluster creation across 16 workers + inline worker inflation
//    Arg(0):
//      0 = baseline (inflate via getThreadLocalCluster(string_view))
//      1 = compressed (inflate via getThreadLocalClusterByResourceName)
// -----------------------------------------------------------------------------
void bmDeferredClusterInlineInflationAndChurn(::benchmark::State& state) {
  const bool compress = state.range(0) != 0;
  const size_t batch_size = Envoy::benchmark::skipExpensiveBenchmarks() ? 10 : 50;
  constexpr size_t NumWorkers = 16;
  state.SetLabel(compress ? "compressed_by_resource_name" : "baseline_string");

  ScopedCompressFeatureOverride guard(compress);

  BenchmarkClusterManagerEnv env(NumWorkers, /*deferred_cluster_creation=*/true);
  env.initClusterManager();

  const auto names = generateXdstpClusterNames(batch_size);
  const auto protos = buildClusterProtos(names);

  std::vector<Config::XdsResourceNameStorage> pinned_storages;
  std::vector<Config::XdsResourceName> resource_names;
  if (compress) {
    pinned_storages.reserve(batch_size);
    resource_names.reserve(batch_size);
    for (const auto& name : names) {
      pinned_storages.push_back(env.symbolTable().encode(name));
      resource_names.push_back(pinned_storages.back().resourceName());
    }
  }

  for (auto _ : state) {
    UNREFERENCED_PARAMETER(_);
    for (size_t i = 0; i < batch_size; ++i) {
      auto status_or_added = env.cluster_manager_->addOrUpdateCluster(protos[i], "v1");
      ::benchmark::DoNotOptimize(status_or_added);
    }
    // Inflate clusters on demand across all workers.
    for (size_t w = 0; w < NumWorkers; ++w) {
      env.multi_worker_tls_.setActiveWorker(w);
      if (compress) {
        for (size_t i = 0; i < batch_size; ++i) {
          auto* tl_cluster =
              env.cluster_manager_->getThreadLocalClusterByResourceName(resource_names[i]);
          ::benchmark::DoNotOptimize(tl_cluster);
        }
      } else {
        for (size_t i = 0; i < batch_size; ++i) {
          auto* tl_cluster = env.cluster_manager_->getThreadLocalCluster(names[i]);
          ::benchmark::DoNotOptimize(tl_cluster);
        }
      }
    }
    for (size_t i = 0; i < batch_size; ++i) {
      bool removed = env.cluster_manager_->removeCluster(names[i]);
      ::benchmark::DoNotOptimize(removed);
    }
  }
}
BENCHMARK(bmDeferredClusterInlineInflationAndChurn)
    ->Arg(0)
    ->Arg(1)
    ->Unit(::benchmark::kMicrosecond);

} // namespace
} // namespace Upstream
} // namespace Envoy

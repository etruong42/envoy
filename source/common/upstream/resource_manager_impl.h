#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "envoy/config/core/v3/base.pb.h"
#include "envoy/common/resource.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/runtime/runtime.h"
#include "envoy/upstream/resource_manager.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"
#include "source/common/common/basic_resource_impl.h"

#include "absl/container/inlined_vector.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Upstream {

struct ManagedResourceImpl : public BasicResourceLimitImpl {
  ManagedResourceImpl(uint64_t max, Runtime::Loader& runtime, const std::string& runtime_key,
                      Stats::Gauge& open_gauge, Stats::Gauge& remaining)
      : BasicResourceLimitImpl(max, runtime, runtime_key), open_gauge_(open_gauge),
        remaining_(remaining) {
    remaining_.set(max);
  }

  // BasicResourceLimitImpl
  void inc() override {
    BasicResourceLimitImpl::inc();
    updateRemaining();
    open_gauge_.set(BasicResourceLimitImpl::canCreate() ? 0 : 1);
    if (on_inc_cb_) {
      on_inc_cb_();
    }
  }
  void decBy(uint64_t amount) override {
    BasicResourceLimitImpl::decBy(amount);
    updateRemaining();
    open_gauge_.set(BasicResourceLimitImpl::canCreate() ? 0 : 1);
  }

  // Optional callback function used for counting new requests in retry budget.
  std::function<void()> on_inc_cb_;

  /**
   * We set the gauge instead of incrementing and decrementing because,
   * though atomics are used, it is possible for the current resource count
   * to be greater than the supplied max.
   */
  void updateRemaining() {
    /**
     * We cannot use std::max here because max() and current_ are
     * unsigned and subtracting them may overflow.
     */
    const uint64_t current_copy = current_;
    remaining_.set(max() > current_copy ? max() - current_copy : 0);
  }

  /**
   * A gauge to notify the live circuit breaker state. The gauge is set to 0
   * to notify that the circuit breaker is not yet triggered.
   */
  Stats::Gauge& open_gauge_;

  /**
   * The number of resources remaining before the circuit breaker opens.
   */
  Stats::Gauge& remaining_;
};

/**
 * Implementation of ResourceManager.
 * NOTE: This implementation makes some assumptions which favor simplicity over correctness.
 * 1) Primarily, it assumes that traffic will be mostly balanced over all the worker threads since
 *    no attempt is made to balance resources between them. It is possible that starvation can
 *    occur during high contention.
 * 2) Though atomics are used, it is possible for resources to temporarily go above the supplied
 *    maximums. This should not effect overall behavior.
 */
class ResourceManagerImpl : public ResourceManager {
public:
  // Cluster circuit-breaker constructor: borrows `cluster_name` (which must outlive this
  // `ResourceManagerImpl`, as `ClusterInfoImpl::name_` does) and formats runtime keys onto a stack
  // buffer on demand, avoiding 8 heap `std::string` allocations per priority manager.
  ResourceManagerImpl(Runtime::Loader& runtime, absl::string_view cluster_name,
                      envoy::config::core::v3::RoutingPriority priority, uint64_t max_connections,
                      uint64_t max_pending_requests, uint64_t max_requests, uint64_t max_retries,
                      uint64_t max_connection_pools, uint64_t max_connections_per_host,
                      ClusterCircuitBreakersStats cb_stats, std::optional<double> budget_percent,
                      std::optional<uint64_t> budget_interval,
                      std::optional<uint32_t> min_retry_concurrency, Event::Dispatcher& dispatcher)
      : runtime_(runtime), key_prefix_(cluster_name),
        priority_infix_(priorityInfix(priority)),
        connections_(max_connections, *this, "max_connections", cb_stats.cx_open_,
                     cb_stats.remaining_cx_),
        pending_requests_(max_pending_requests, *this, "max_pending_requests",
                          cb_stats.rq_pending_open_, cb_stats.remaining_pending_),
        requests_(max_requests, *this, "max_requests", cb_stats.rq_open_, cb_stats.remaining_rq_),
        connection_pools_(max_connection_pools, *this, "max_connection_pools",
                          cb_stats.cx_pool_open_, cb_stats.remaining_cx_pools_),
        max_connections_per_host_(max_connections_per_host),
        retries_(budget_percent, budget_interval, min_retry_concurrency, max_retries, *this,
                 cb_stats.rq_retry_open_, cb_stats.remaining_retries_, requests_, pending_requests_,
                 dispatcher) {
    // Count active requests when retry budget is configured.
    // Pending requests are not counted to avoid counting them twice.
    if (budget_percent.has_value() || min_retry_concurrency.has_value()) {
      requests_.on_inc_cb_ = [this]() { retries_.writeRequest(); };
    }
  }

  ResourceManagerImpl(Runtime::Loader& runtime, absl::string_view runtime_key,
                      uint64_t max_connections, uint64_t max_pending_requests,
                      uint64_t max_requests, uint64_t max_retries, uint64_t max_connection_pools,
                      uint64_t max_connections_per_host, ClusterCircuitBreakersStats cb_stats,
                      std::optional<double> budget_percent, std::optional<uint64_t> budget_interval,
                      std::optional<uint32_t> min_retry_concurrency, Event::Dispatcher& dispatcher)
      : runtime_(runtime), owned_key_prefix_(copyKeyPrefix(runtime_key)),
        key_prefix_(owned_key_prefix_ != nullptr
                        ? absl::string_view(owned_key_prefix_.get(), runtime_key.size())
                        : absl::string_view()),
        connections_(max_connections, *this, "max_connections", cb_stats.cx_open_,
                     cb_stats.remaining_cx_),
        pending_requests_(max_pending_requests, *this, "max_pending_requests",
                          cb_stats.rq_pending_open_, cb_stats.remaining_pending_),
        requests_(max_requests, *this, "max_requests", cb_stats.rq_open_, cb_stats.remaining_rq_),
        connection_pools_(max_connection_pools, *this, "max_connection_pools",
                          cb_stats.cx_pool_open_, cb_stats.remaining_cx_pools_),
        max_connections_per_host_(max_connections_per_host),
        retries_(budget_percent, budget_interval, min_retry_concurrency, max_retries, *this,
                 cb_stats.rq_retry_open_, cb_stats.remaining_retries_, requests_, pending_requests_,
                 dispatcher) {
    // Count active requests when retry budget is configured.
    // Pending requests are not counted to avoid counting them twice.
    if (budget_percent.has_value() || min_retry_concurrency.has_value()) {
      requests_.on_inc_cb_ = [this]() { retries_.writeRequest(); };
    }
  }

  // Upstream::ResourceManager
  ResourceLimit& connections() override { return connections_; }
  ResourceLimit& pendingRequests() override { return pending_requests_; }
  ResourceLimit& requests() override { return requests_; }
  ResourceLimit& retries() override { return retries_; }
  ResourceLimit& connectionPools() override { return connection_pools_; }
  uint64_t maxConnectionsPerHost() override { return max_connections_per_host_; }

private:
  using RuntimeKeyBuffer = absl::InlinedVector<char, 256>;

  static absl::string_view priorityInfix(envoy::config::core::v3::RoutingPriority priority) {
    switch (priority) {
      PANIC_ON_PROTO_ENUM_SENTINEL_VALUES;
    case envoy::config::core::v3::DEFAULT:
      return ".default.";
    case envoy::config::core::v3::HIGH:
      return ".high.";
    }
    PANIC_DUE_TO_CORRUPT_ENUM;
  }

  static std::unique_ptr<char[]> copyKeyPrefix(absl::string_view runtime_key) {
    if (runtime_key.empty()) {
      return nullptr;
    }
    auto copy = std::make_unique<char[]>(runtime_key.size());
    std::copy_n(runtime_key.data(), runtime_key.size(), copy.get());
    return copy;
  }

  absl::string_view buildRuntimeKey(absl::string_view suffix, RuntimeKeyBuffer& buf) const {
    static constexpr absl::string_view kCircuitBreakersPrefix = "circuit_breakers.";
    const size_t total_size =
        (priority_infix_.empty() ? 0 : kCircuitBreakersPrefix.size()) + key_prefix_.size() +
        priority_infix_.size() + suffix.size();
    if (total_size > buf.capacity()) {
      buf.reserve(total_size);
    }
    if (!priority_infix_.empty()) {
      buf.insert(buf.end(), kCircuitBreakersPrefix.begin(), kCircuitBreakersPrefix.end());
    }
    buf.insert(buf.end(), key_prefix_.begin(), key_prefix_.end());
    buf.insert(buf.end(), priority_infix_.begin(), priority_infix_.end());
    buf.insert(buf.end(), suffix.begin(), suffix.end());
    return absl::string_view(buf.data(), buf.size());
  }

  uint64_t getRuntimeInteger(absl::string_view suffix, uint64_t default_value) const {
    RuntimeKeyBuffer buf;
    return runtime_.snapshot().getInteger(buildRuntimeKey(suffix, buf), default_value);
  }

  double getRuntimeDouble(absl::string_view suffix, double default_value) const {
    RuntimeKeyBuffer buf;
    return runtime_.snapshot().getDouble(buildRuntimeKey(suffix, buf), default_value);
  }

  bool hasRuntimeKey(absl::string_view suffix) const {
    RuntimeKeyBuffer buf;
    return runtime_.snapshot().get(buildRuntimeKey(suffix, buf)).has_value();
  }

  struct ClusterResourceImpl final : public ResourceLimit {
    ClusterResourceImpl(uint64_t max, const ResourceManagerImpl& parent,
                        absl::string_view runtime_key_suffix, Stats::Gauge& open_gauge,
                        Stats::Gauge& remaining)
        : max_(max), parent_(parent), runtime_key_suffix_(runtime_key_suffix),
          open_gauge_(open_gauge), remaining_(remaining) {
      remaining_.set(max);
    }

    // Envoy::ResourceLimit
    bool canCreate() override { return current_.load() < max(); }
    void inc() override {
      ++current_;
      updateRemaining();
      open_gauge_.set(canCreate() ? 0 : 1);
      if (on_inc_cb_) {
        on_inc_cb_();
      }
    }
    void dec() override { decBy(1); }
    void decBy(uint64_t amount) override {
      ASSERT(current_ >= amount);
      current_ -= amount;
      updateRemaining();
      open_gauge_.set(canCreate() ? 0 : 1);
    }
    uint64_t max() override { return parent_.getRuntimeInteger(runtime_key_suffix_, max_); }
    uint64_t count() const override { return current_.load(); }

    void updateRemaining() {
      const uint64_t current_copy = current_;
      remaining_.set(max() > current_copy ? max() - current_copy : 0);
    }

    std::atomic<uint64_t> current_{0};
    const uint64_t max_;
    const ResourceManagerImpl& parent_;
    const absl::string_view runtime_key_suffix_;
    Stats::Gauge& open_gauge_;
    Stats::Gauge& remaining_;
    std::function<void()> on_inc_cb_;
  };

  class RetryBudgetImpl : public ResourceLimit {
  public:
    RetryBudgetImpl(std::optional<double> budget_percent, std::optional<uint64_t> budget_interval,
                    std::optional<uint32_t> min_retry_concurrency, uint64_t max_retries,
                    const ResourceManagerImpl& parent, Stats::Gauge& open_gauge,
                    Stats::Gauge& remaining, const ResourceLimit& requests,
                    const ResourceLimit& pending_requests, Event::Dispatcher& dispatcher)
        : parent_(parent),
          max_retry_resource_(max_retries, parent, "max_retries", open_gauge, remaining),
          budget_percent_(budget_percent), budget_interval_(budget_interval),
          min_retry_concurrency_(min_retry_concurrency), requests_(requests),
          pending_requests_(pending_requests), remaining_(remaining) {
      if (budget_interval.has_value() && budget_interval.value() > 0) {
        interval_state_ = std::make_unique<IntervalState>();
        interval_state_->expiration_interval = budget_interval.value() / NumSlots;

        const auto expiration_interval_ms =
            std::chrono::milliseconds(interval_state_->expiration_interval);

        // Every budget_interval / 10 seconds, call expireRequests to decrement outdated requests
        // from req_in_interval_, store the current value of req_to_expire_ in expireAmounts, and
        // reset req_to_expire_ to 0.
        interval_state_->main_timer = dispatcher.createTimer([this, expiration_interval_ms]() {
          expireRequests();
          interval_state_->main_timer->enableTimer(expiration_interval_ms);
        });
        interval_state_->main_timer->enableTimer(expiration_interval_ms);
      }
    }

    // Envoy::ResourceLimit
    bool canCreate() override {
      if (!useRetryBudget()) {
        return max_retry_resource_.canCreate();
      }
      clearRemainingGauge();
      return count() < max();
    }
    void inc() override {
      max_retry_resource_.inc();
      clearRemainingGauge();
    }
    void dec() override {
      max_retry_resource_.dec();
      clearRemainingGauge();
    }
    void decBy(uint64_t amount) override {
      max_retry_resource_.decBy(amount);
      clearRemainingGauge();
    }
    void writeRequest() {
      if (interval_state_ == nullptr) {
        return;
      }
      interval_state_->req_in_interval.fetch_add(1, std::memory_order_relaxed);
      interval_state_->req_to_expire.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t max() override {
      if (!useRetryBudget()) {
        return max_retry_resource_.max();
      }

      const double budget_percent = parent_.getRuntimeDouble(
          "retry_budget.budget_percent", budget_percent_ ? *budget_percent_ : 20.0);
      const uint32_t min_retry_concurrency = parent_.getRuntimeInteger(
          "retry_budget.min_retry_concurrency",
          min_retry_concurrency_ ? *min_retry_concurrency_ : 3);

      clearRemainingGauge();

      // Use the in-flight request count when budget_interval is not present.
      uint64_t requests_for_budget = requests_.count() + pending_requests_.count();

      if (interval_state_ != nullptr) {
        const uint64_t configured_interval = parent_.getRuntimeInteger(
            "retry_budget.budget_interval", budget_interval_ ? *budget_interval_ : 0);
        if (configured_interval > 0) {
          // Retry budget implementation across a fixed window inspired by tower.rs:
          // https://github.com/tower-rs/tower/blob/master/tower/src/retry/budget/tps_budget.rs
          requests_for_budget = interval_state_->req_in_interval.load(std::memory_order_seq_cst);
        }
      }
      // We enforce that the retry concurrency is never allowed to go below the
      // min_retry_concurrency, even if the configured percent of the current active requests yields
      // a value that is smaller.
      return std::max<uint64_t>(budget_percent / 100.0 * requests_for_budget,
                                min_retry_concurrency);
    }
    uint64_t count() const override { return max_retry_resource_.count(); }

  private:
    bool useRetryBudget() const {
      return parent_.hasRuntimeKey("retry_budget.budget_percent") ||
             parent_.hasRuntimeKey("retry_budget.min_retry_concurrency") || budget_percent_ ||
             min_retry_concurrency_;
    }

    // If the retry budget is in use, the stats tracking remaining retries do not make sense since
    // they would dependent on other resources that can change without a call to this object.
    // Therefore, the gauge should just be reset to 0.
    void clearRemainingGauge() {
      if (useRetryBudget()) {
        remaining_.set(0);
      }
    }

    void expireRequests() {
      ASSERT(interval_state_ != nullptr);
      const uint64_t to_expire =
          interval_state_->req_to_expire.exchange(0, std::memory_order_seq_cst);
      // Add slot to the deque, even if to_expire is 0.
      interval_state_->expire_amounts.push_back(to_expire);

      // If the deque has as many elements as the number of slots, expire the oldest slot.
      if (interval_state_->expire_amounts.size() >= NumSlots) {
        const uint64_t expired = interval_state_->expire_amounts.front();
        interval_state_->expire_amounts.pop_front();
        if (expired > 0) {
          interval_state_->req_in_interval.fetch_sub(expired, std::memory_order_seq_cst);
        }
      }
    }

    const ResourceManagerImpl& parent_;
    // The max_retry resource is nested within the budget to maintain state if the retry budget is
    // toggled.
    ClusterResourceImpl max_retry_resource_;
    const std::optional<double> budget_percent_;
    const std::optional<uint64_t> budget_interval_;
    const std::optional<uint32_t> min_retry_concurrency_;
    const ResourceLimit& requests_;
    const ResourceLimit& pending_requests_;
    Stats::Gauge& remaining_;

    static constexpr uint64_t NumSlots = 10;

    // Allocated when budget_interval is configured.
    struct IntervalState {
      uint64_t expiration_interval{0};
      std::atomic<uint64_t> req_in_interval{0};
      std::atomic<uint64_t> req_to_expire{0};
      Event::TimerPtr main_timer;
      std::deque<uint64_t> expire_amounts;
    };
    std::unique_ptr<IntervalState> interval_state_;
  };

  Runtime::Loader& runtime_;
  const std::unique_ptr<char[]> owned_key_prefix_;
  const absl::string_view key_prefix_;
  const absl::string_view priority_infix_;
  ClusterResourceImpl connections_;
  ClusterResourceImpl pending_requests_;
  ClusterResourceImpl requests_;
  ClusterResourceImpl connection_pools_;
  uint64_t max_connections_per_host_;
  RetryBudgetImpl retries_;
};

using ResourceManagerImplPtr = std::unique_ptr<ResourceManagerImpl>;

} // namespace Upstream
} // namespace Envoy

#include "source/common/init/manager_impl.h"

#include <functional>

#include "source/common/common/assert.h"
#include "source/common/init/watcher_impl.h"

namespace Envoy {
namespace Init {

ManagerImpl::ManagerImpl(absl::string_view name) : name_(fmt::format("init manager {}", name)) {}

Manager::State ManagerImpl::state() const { return state_; }

WatcherImpl& ManagerImpl::ensureWatcher() {
  if (!watcher_.has_value()) {
    watcher_.emplace(name_, [this](absl::string_view target_name) { onTargetReady(target_name); });
  }
  return *watcher_;
}

void ManagerImpl::add(const Target& target) {
  ++count_;
  TargetHandlePtr target_handle(target.createHandle(name_));
  ++target_names_count_[target.name()];
  switch (state_) {
  case State::Uninitialized:
    // If the manager isn't initialized yet, save the target handle to be initialized later.
    ENVOY_LOG(debug, "added {} to {}", target.name(), name_);
    target_handles_.push_back(std::move(target_handle));
    return;
  case State::Initializing:
    // If the manager is already initializing, initialize the new target immediately. Note that
    // it's important in this case that count_ was incremented above before calling the target,
    // because if the target calls the init manager back immediately, count_ will be decremented
    // here (see ensureWatcher() above).
    target_handle->initialize(ensureWatcher());
    return;
  case State::Initialized:
    // If the manager has already completed initialization, consider this a programming error.
    ASSERT(false, fmt::format("attempted to add {} to initialized {}", target.name(), name_));
  }
}

void ManagerImpl::initialize(const Watcher& watcher) {
  // If the manager is already initializing or initialized, consider this a programming error.
  ASSERT(state_ == State::Uninitialized, fmt::format("attempted to initialize {} twice", name_));

  // Create a handle to notify when initialization is complete.
  watcher_handle_ = watcher.createHandle(name_);

  if (count_ == 0) {
    // If we have no targets, initialization trivially completes. This can happen, and is fine.
    ENVOY_LOG(debug, "{} contains no targets", name_);
    ready();
  } else {
    // If we have some targets, start initialization...
    ENVOY_LOG(debug, "{} initializing", name_);
    state_ = State::Initializing;

    // Move target_handles_ to a local list so handles are released once invoked and iteration
    // remains safe even if the final target completes synchronously and destroys this manager.
    auto target_handles = std::move(target_handles_);
    WatcherImpl& target_watcher = ensureWatcher();

    // Attempt to initialize each target. If a target is unavailable, treat it as though it
    // completed immediately.
    for (const auto& target_handle : target_handles) {
      if (!target_handle->initialize(target_watcher)) {
        onTargetReady(target_handle->name());
      }
    }
  }
}

void ManagerImpl::updateWatcher(const Watcher& watcher) {
  ASSERT(state_ != State::Initialized, "attempted to update watcher on initialized manager");
  watcher_handle_ = watcher.createHandle(name_);
};

void ManagerImpl::dumpUnreadyTargets(envoy::admin::v3::UnreadyTargetsDumps& unready_targets_dumps) {
  auto& message = *unready_targets_dumps.mutable_unready_targets_dumps()->Add();
  message.set_name(name_);
  for (const auto& [target_name, count] : target_names_count_) {
    UNREFERENCED_PARAMETER(count);
    message.add_target_names(target_name);
  }
}

void ManagerImpl::onTargetReady(absl::string_view target_name) {
  // If there are no remaining targets and one mysteriously calls us back, this manager is haunted.
  ASSERT(count_ != 0,
         fmt::format("{} called back by target after initialization complete", target_name));

  // Decrease target_name count by 1.
  ASSERT(target_names_count_.contains(target_name));
  if (--target_names_count_[target_name] == 0) {
    target_names_count_.erase(target_name);
  }

  // If there are no uninitialized targets remaining when called back by a target, that means it was
  // the last. Signal `ready` to the handle we saved in `initialize`.
  if (--count_ == 0) {
    ready();
  }
}

void ManagerImpl::clearAfterInitialization() {
  ASSERT(state_ == State::Initialized);
  name_ = std::string();
}

void ManagerImpl::ready() {
  state_ = State::Initialized;
  target_names_count_ = {};
  watcher_.reset();
  // Move watcher_handle_ to a local before invoking ready() in case the callback destroys `this`,
  // and to immediately reclaim the WatcherHandleImpl and its manager-name string copy.
  auto local_watcher_handle = std::move(watcher_handle_);
  local_watcher_handle->ready();
}

} // namespace Init
} // namespace Envoy

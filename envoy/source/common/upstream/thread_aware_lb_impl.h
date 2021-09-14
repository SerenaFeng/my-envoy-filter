#pragma once

#include "envoy/common/callback.h"
#include "envoy/config/cluster/v3/cluster.pb.h"

#include "source/common/common/logger.h"
#include "source/common/config/metadata.h"
#include "source/common/config/well_known_names.h"
#include "source/common/upstream/load_balancer_impl.h"

#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"

namespace Envoy {
namespace Upstream {

using NormalizedHostWeightVector = std::vector<std::pair<HostConstSharedPtr, double>>;
using NormalizedHostWeightMap = std::map<HostConstSharedPtr, double>;

class ThreadAwareLoadBalancerBase : public LoadBalancerBase, public ThreadAwareLoadBalancer {
public:
  /**
   * Base class for a hashing load balancer implemented for use in a thread aware load balancer.
   * TODO(mattklein123): Currently only RingHash and Maglev use the thread aware load balancer.
   *                     The hash is pre-computed prior to getting to the real load balancer for
   *                     use in priority selection. In the future we likely we will want to pass
   *                     through the full load balancer context in case a future implementation
   *                     wants to use it.
   */
  class HashingLoadBalancer {
  public:
    virtual ~HashingLoadBalancer() = default;
    virtual HostConstSharedPtr chooseHost(uint64_t hash, uint32_t attempt) const PURE;
    const absl::string_view hashKey(HostConstSharedPtr host, bool use_hostname) {
      const ProtobufWkt::Value& val = Config::Metadata::metadataValue(
          host->metadata().get(), Config::MetadataFilters::get().ENVOY_LB,
          Config::MetadataEnvoyLbKeys::get().HASH_KEY);
      if (val.kind_case() != val.kStringValue && val.kind_case() != val.KIND_NOT_SET) {
        FANCY_LOG(debug, "hash_key must be string type, got: {}", val.kind_case());
      }
      absl::string_view hash_key = val.string_value();
      if (hash_key.empty()) {
        hash_key = use_hostname ? host->hostname() : host->address()->asString();
      }
      return hash_key;
    }
  };
  using HashingLoadBalancerSharedPtr = std::shared_ptr<HashingLoadBalancer>;

  /**
   * Class for consistent hashing load balancer (CH-LB) with bounded loads.
   * It is common to both RingHash and Maglev load balancers, because the logic of selecting the
   * next host when one is overloaded is independent of the CH-LB type.
   */
  class BoundedLoadHashingLoadBalancer : public HashingLoadBalancer {
  public:
    BoundedLoadHashingLoadBalancer(HashingLoadBalancerSharedPtr hashing_lb_ptr,
                                   NormalizedHostWeightVector normalized_host_weights,
                                   uint32_t hash_balance_factor)
        : normalized_host_weights_map_(initNormalizedHostWeightMap(normalized_host_weights)),
          hashing_lb_ptr_(std::move(hashing_lb_ptr)),
          normalized_host_weights_(std::move(normalized_host_weights)),
          hash_balance_factor_(hash_balance_factor) {
      ASSERT(hashing_lb_ptr_ != nullptr);
      ASSERT(hash_balance_factor > 0);
    }
    HostConstSharedPtr chooseHost(uint64_t hash, uint32_t attempt) const override;

  protected:
    virtual double hostOverloadFactor(const Host& host, double weight) const;
    const NormalizedHostWeightMap normalized_host_weights_map_;

  private:
    const NormalizedHostWeightMap
    initNormalizedHostWeightMap(const NormalizedHostWeightVector& normalized_host_weights) {
      NormalizedHostWeightMap normalized_host_weights_map;
      for (auto const& item : normalized_host_weights) {
        normalized_host_weights_map[item.first] = item.second;
      }
      return normalized_host_weights_map;
    }
    const HashingLoadBalancerSharedPtr hashing_lb_ptr_;
    const NormalizedHostWeightVector normalized_host_weights_;
    const uint32_t hash_balance_factor_;
  };
  // Upstream::ThreadAwareLoadBalancer
  LoadBalancerFactorySharedPtr factory() override { return factory_; }
  void initialize() override;

  // Upstream::LoadBalancer
  HostConstSharedPtr chooseHost(LoadBalancerContext*) override { NOT_IMPLEMENTED_GCOVR_EXCL_LINE; }
  // Preconnect not implemented for hash based load balancing
  HostConstSharedPtr peekAnotherHost(LoadBalancerContext*) override { return nullptr; }

protected:
  ThreadAwareLoadBalancerBase(
      const PrioritySet& priority_set, ClusterStats& stats, Runtime::Loader& runtime,
      Random::RandomGenerator& random,
      const envoy::config::cluster::v3::Cluster::CommonLbConfig& common_config)
      : LoadBalancerBase(priority_set, stats, runtime, random, common_config),
        factory_(new LoadBalancerFactoryImpl(stats, random, *this)) {}

private:
  struct PerPriorityState {
    std::shared_ptr<HashingLoadBalancer> current_lb_;
    bool global_panic_{};
  };
  using PerPriorityStatePtr = std::unique_ptr<PerPriorityState>;

  struct LoadBalancerImpl : public LoadBalancer {
    LoadBalancerImpl(ClusterStats& stats, Random::RandomGenerator& random,
                     HostMapConstSharedPtr host_map)
        : stats_(stats), random_(random), cross_priority_host_map_(std::move(host_map)) {}

    // Upstream::LoadBalancer
    HostConstSharedPtr chooseHost(LoadBalancerContext* context) override;
    // Preconnect not implemented for hash based load balancing
    HostConstSharedPtr peekAnotherHost(LoadBalancerContext*) override { return nullptr; }

    ClusterStats& stats_;
    Random::RandomGenerator& random_;
    std::shared_ptr<std::vector<PerPriorityStatePtr>> per_priority_state_;
    std::shared_ptr<HealthyLoad> healthy_per_priority_load_;
    std::shared_ptr<DegradedLoad> degraded_per_priority_load_;

    // Cross priority host map.
    HostMapConstSharedPtr cross_priority_host_map_;
  };

  struct LoadBalancerFactoryImpl : public LoadBalancerFactory {
    LoadBalancerFactoryImpl(ClusterStats& stats, Random::RandomGenerator& random,
                            ThreadAwareLoadBalancerBase& thread_aware_lb)
        : thread_aware_lb_(thread_aware_lb), stats_(stats), random_(random) {}

    // Upstream::LoadBalancerFactory
    LoadBalancerPtr create() override;

    ThreadAwareLoadBalancerBase& thread_aware_lb_;

    ClusterStats& stats_;
    Random::RandomGenerator& random_;
    absl::Mutex mutex_;
    std::shared_ptr<std::vector<PerPriorityStatePtr>> per_priority_state_ ABSL_GUARDED_BY(mutex_);
    // This is split out of PerPriorityState so LoadBalancerBase::ChoosePriority can be reused.
    std::shared_ptr<HealthyLoad> healthy_per_priority_load_ ABSL_GUARDED_BY(mutex_);
    std::shared_ptr<DegradedLoad> degraded_per_priority_load_ ABSL_GUARDED_BY(mutex_);
  };

  virtual HashingLoadBalancerSharedPtr
  createLoadBalancer(const NormalizedHostWeightVector& normalized_host_weights,
                     double min_normalized_weight, double max_normalized_weight) PURE;
  void refresh();

  void threadSafeSetCrossPriorityHostMap(HostMapConstSharedPtr host_map) {
    absl::MutexLock ml(&cross_priority_host_map_mutex_);
    cross_priority_host_map_ = std::move(host_map);
  }
  HostMapConstSharedPtr threadSafeGetCrossPriorityHostMap() {
    absl::MutexLock ml(&cross_priority_host_map_mutex_);
    return cross_priority_host_map_;
  }

  std::shared_ptr<LoadBalancerFactoryImpl> factory_;
  Common::CallbackHandlePtr priority_update_cb_;

  // Whenever the membership changes, the cross_priority_host_map_ will be updated automatically.
  // And all workers will create a new worker local load balancer and copy the
  // cross_priority_host_map_.
  //
  // This leads to the possibility of simultaneous reading and writing of cross_priority_host_map_
  // in different threads. For this reason, an additional mutex is necessary to guard
  // cross_priority_host_map_.
  absl::Mutex cross_priority_host_map_mutex_;
  // Cross priority host map for fast cross priority host searching. When the priority update
  // callback is executed, the host map will also be updated.
  HostMapConstSharedPtr cross_priority_host_map_ ABSL_GUARDED_BY(cross_priority_host_map_mutex_);
};

} // namespace Upstream
} // namespace Envoy
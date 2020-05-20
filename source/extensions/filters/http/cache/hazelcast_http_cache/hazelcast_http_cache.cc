#include "extensions/filters/http/cache/hazelcast_http_cache/hazelcast_http_cache.h"

#include "extensions/filters/http/cache/hazelcast_http_cache/hazelcast_context.h"
#include "extensions/filters/http/cache/hazelcast_http_cache/util.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace HazelcastHttpCache {

using hazelcast::client::ClientConfig;
using hazelcast::client::exception::HazelcastClientOfflineException;
using hazelcast::client::exception::OperationTimeoutException;
using hazelcast::client::serialization::DataSerializableFactory;

HazelcastHttpCache::HazelcastHttpCache(HazelcastHttpCacheConfig config)
    : unified_(config.unified()),
      body_partition_size_(ConfigUtil::validPartitionSize(config.body_partition_size())),
      max_body_size_(ConfigUtil::validMaxBodySize(config.max_body_size(), config.unified())),
      cache_config_(config) {}

void HazelcastHttpCache::onMissingBody(uint64_t key, int32_t version, uint64_t body_size) {
  try {
    if (!tryLock(key)) {
      // Let lock owner context to recover it.
      return;
    }
    auto header = getHeader(key);
    if (header && header->version() != version) {
      // The missed body does not belong to the looked up header. Probably eviction and then
      // insertion for the header has happened in the meantime. Since new insertion will
      // override the existing bodies, ignore the cleanup and let orphan bodies (belong to
      // evicted header, not overridden) be evicted by TTL as well.
      unlock(key);
      return;
    }
    int body_count = body_size / body_partition_size_;
    while (body_count >= 0) {
      accessor_->removeBodyAsync(orderedMapKey(key, body_count--));
    }
    accessor_->removeHeader(mapKey(key));
    unlock(key);
  } catch (HazelcastClientOfflineException& e) {
    // see DividedInsertContext::insertHeader for left over locks on a connection failure.
    ENVOY_LOG(warn, "Hazelcast Connection is offline!");
  } catch (std::exception& e) {
    ENVOY_LOG(warn, "Clean up for missing body has failed: {}", e.what());
  }
}

void HazelcastHttpCache::onVersionMismatch(uint64_t key, int32_t version, uint64_t body_size) {
  onMissingBody(key, version, body_size);
}

void HazelcastHttpCache::start() {
  if (accessor_ && accessor_->isRunning()) {
    ENVOY_LOG(warn, "Client is already connected. Cluster name: {}", accessor_->clusterName());
    return;
  }

  ClientConfig client_config = ConfigUtil::getClientConfig(cache_config_);
  client_config.getSerializationConfig().addDataSerializableFactory(
      HazelcastCacheEntrySerializableFactory::FACTORY_ID,
      boost::shared_ptr<DataSerializableFactory>(new HazelcastCacheEntrySerializableFactory()));

  if (!accessor_) {
    accessor_ = std::make_unique<HazelcastClusterAccessor>(
        std::move(client_config), cache_config_.app_prefix(), body_partition_size_);
    ENVOY_LOG(debug, "New HazelcastClusterAccessor created.");
  }

  try {
    accessor_->connect();
  } catch (...) {
    accessor_.reset();
    throw EnvoyException("Hazelcast Client could not connect to any cluster.");
  }

  ENVOY_LOG(info, "HazelcastHttpCache has been started with profile: {}. Max body size: {}.",
            unified_ ? "UNIFIED"
                     : "DIVIDED, partition size: " + std::to_string(body_partition_size_),
            max_body_size_);

  HazelcastClusterAccessor& cluster_accessor = static_cast<HazelcastClusterAccessor&>(*accessor_);
  ENVOY_LOG(info,
            "Cache statistics can be observed on Hazelcast Management Center"
            " from the map named {}.",
            unified_ ? cluster_accessor.responseMapName() : cluster_accessor.headerMapName());
}

void HazelcastHttpCache::shutdown(bool destroy) {
  if (!accessor_) {
    ENVOY_LOG(warn, "Cache is already offline.");
    return;
  }
  if (accessor_->isRunning()) {
    ENVOY_LOG(info, "Shutting down Hazelcast connection...");
    accessor_->disconnect();
    ENVOY_LOG(info, "Cache is offline now.");
  } else {
    ENVOY_LOG(warn, "Hazelcast client is already disconnected.");
  }
  if (destroy) {
    accessor_.reset();
  }
}

HazelcastHttpCache::~HazelcastHttpCache() { shutdown(true); }

LookupContextPtr HazelcastHttpCache::makeLookupContext(LookupRequest&& request) {
  if (unified_) {
    return std::make_unique<UnifiedLookupContext>(*this, std::move(request));
  } else {
    return std::make_unique<DividedLookupContext>(*this, std::move(request));
  }
}

InsertContextPtr HazelcastHttpCache::makeInsertContext(LookupContextPtr&& lookup_context) {
  ASSERT(lookup_context != nullptr);
  if (unified_) {
    return std::make_unique<UnifiedInsertContext>(*lookup_context, *this);
  } else {
    return std::make_unique<DividedInsertContext>(*lookup_context, *this);
  }
}

// TODO(enozcan): Implement when it's ready on the filter side.
//  Depending on the filter's implementation, the cached entry's
//  variant_key_ must be updated as well. Also, if vary headers
//  change then the hash key of the response will change and
//  updating only header map will not be enough in this case.
void HazelcastHttpCache::updateHeaders(LookupContextPtr&& lookup_context,
                                       Http::ResponseHeaderMapPtr&& response_headers) {
  ASSERT(lookup_context);
  ASSERT(response_headers);
  NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
}

constexpr absl::string_view HazelcastCacheName = "envoy.extensions.http.cache.hazelcast";

// Cluster wide cache statistics should be observed on Hazelcast Management Center.
// They are not stored locally.
CacheInfo HazelcastHttpCache::cacheInfo() const {
  CacheInfo cache_info;
  cache_info.name_ = HazelcastCacheName;
  cache_info.supports_range_requests_ = true;
  return cache_info;
}

std::string HazelcastHttpCacheFactory::name() const { return std::string(HazelcastCacheName); }

ProtobufTypes::MessagePtr HazelcastHttpCacheFactory::createEmptyConfigProto() {
  return std::make_unique<HazelcastHttpCacheConfig>();
}

HttpCache& HazelcastHttpCacheFactory::getCache(
    const envoy::extensions::filters::http::cache::v3alpha::CacheConfig& config) {
  if (!cache_) {
    HazelcastHttpCacheConfig hz_cache_config;
    MessageUtil::unpackTo(config.typed_config(), hz_cache_config);
    cache_ = std::make_unique<HazelcastHttpCache>(hz_cache_config);
    cache_->start();
  }
  return *cache_;
}

HttpCache& HazelcastHttpCacheFactory::getOfflineCache(
    const envoy::extensions::filters::http::cache::v3alpha::CacheConfig& config) {
  if (!cache_) {
    HazelcastHttpCacheConfig hz_cache_config;
    MessageUtil::unpackTo(config.typed_config(), hz_cache_config);
    cache_ = std::make_unique<HazelcastHttpCache>(hz_cache_config);
  }
  return *cache_;
}

static Registry::RegisterFactory<HazelcastHttpCacheFactory, HttpCacheFactory> register_;

} // namespace HazelcastHttpCache
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

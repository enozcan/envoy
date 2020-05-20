#pragma once

#include "common/http/header_map_impl.h"

#include "source/extensions/filters/http/cache/key.pb.h"

#include "hazelcast/client/EntryView.h"
#include "hazelcast/client/serialization/ObjectDataOutput.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace HazelcastHttpCache {

using hazelcast::client::serialization::DataSerializableFactory;
using hazelcast::client::serialization::IdentifiedDataSerializable;
using hazelcast::client::serialization::ObjectDataInput;
using hazelcast::client::serialization::ObjectDataOutput;

static const int HAZELCAST_BODY_TYPE_ID = 100;
static const int HAZELCAST_HEADER_TYPE_ID = 101;
static const int HAZELCAST_RESPONSE_TYPE_ID = 102;
static const int HAZELCAST_ENTRY_SERIALIZER_FACTORY_ID = 1000;

/**
 * Response header wrapper for cache entries.
 *
 * @note In DIVIDED cache mode, response headers and corresponding bodies will be
 * stored in different distributed maps. This option is in favor of the efficiency
 * of range HTTP requests. For each header entry, there will be body entries (if any)
 * with a relevant key.
 */
class HazelcastHeaderEntry : public IdentifiedDataSerializable {
public:
  HazelcastHeaderEntry();
  HazelcastHeaderEntry(Http::ResponseHeaderMapPtr&& header_map, Key&& key, uint64_t body_size,
                       int32_t version);
  HazelcastHeaderEntry(const HazelcastHeaderEntry& other);
  HazelcastHeaderEntry(HazelcastHeaderEntry&& other) noexcept;

  // hazelcast::client::serialization::IdentifiedDataSerializable
  void writeData(ObjectDataOutput& writer) const override;
  void readData(ObjectDataInput& reader) override;
  int getClassId() const override { return HAZELCAST_HEADER_TYPE_ID; }
  int getFactoryId() const override { return HAZELCAST_ENTRY_SERIALIZER_FACTORY_ID; }

  // Only required fields of a header entry for unified mode are de/serialized
  // in unifiedData methods.
  void writeUnifiedData(ObjectDataOutput& writer) const;
  void readUnifiedData(ObjectDataInput& reader);

  Http::ResponseHeaderMapPtr& headerMap() { return header_map_; }
  const Http::ResponseHeaderMapPtr& headerMap() const { return header_map_; }
  const Key& variantKey() const { return variant_key_; }
  uint64_t bodySize() const { return body_size_; }
  int32_t version() const { return version_; }

  void variantKey(Key&& key) { variant_key_ = std::move(key); }
  void version(int32_t version) { version_ = version; }

  bool operator==(const HazelcastHeaderEntry& other) const;

private:
  Http::ResponseHeaderMapPtr header_map_;

  /** The key generated by the cache filter and modified with vary headers later on. */
  Key variant_key_;

  /** Total body size of the response with these headers. */
  uint64_t body_size_;

  /** Marker to link bodies to header. Bodies in DIVIDED mode will have the same
   * version and hence it is ensured a body partition belongs to the correct header.
   * Used to handle malformed responses. */
  int32_t version_;
};

/**
 * Response body wrapper for cache entries.
 *
 * @note In DIVIDED cache mode, response headers and corresponding bodies will be stored in
 * different distributed maps. For a response HeaderEntry with 64-bit hash key <H>, bodies
 * will be stored with keys <H>"#0", <H>"#1", <H>"#2".. and so on in a continuous manner.
 * Body partition size is fixed and configurable via cache config. On a range request, only
 * necessary partitions according to the request will be fetched from distributed map,
 * not the whole response.
 */
class HazelcastBodyEntry : public IdentifiedDataSerializable {
public:
  HazelcastBodyEntry();
  HazelcastBodyEntry(std::vector<hazelcast::byte>&& buffer, int32_t version);
  HazelcastBodyEntry(const HazelcastBodyEntry& other);
  HazelcastBodyEntry(HazelcastBodyEntry&& other) noexcept;

  // hazelcast::client::serialization::IdentifiedDataSerializable
  void writeData(ObjectDataOutput& writer) const override;
  void readData(ObjectDataInput& reader) override;
  int getClassId() const override { return HAZELCAST_BODY_TYPE_ID; }
  int getFactoryId() const override { return HAZELCAST_ENTRY_SERIALIZER_FACTORY_ID; }

  // Only required fields of a header entry for unified mode are de/serialized
  // in unifiedData methods.
  void writeUnifiedData(ObjectDataOutput& writer) const;
  void readUnifiedData(ObjectDataInput& reader);

  size_t length() const { return body_buffer_.size(); }
  hazelcast::byte* begin() { return body_buffer_.data(); }
  int32_t version() const { return version_; }
  const std::vector<hazelcast::byte>& buffer() const { return body_buffer_; }

  void version(int32_t version) { version_ = version; }

  bool operator==(const HazelcastBodyEntry& other) const;

private:
  /** Derived from header */
  int32_t version_;

  std::vector<hazelcast::byte> body_buffer_;
};

/**
 * Response wrapper for cache entries.
 *
 * @note In UNIFIED cache mode, unlike DIVIDED, there is only one cache entry containing
 * the response as a whole. Even if a range request arrives, all the body is fetched from
 * the cache. This option is in favor of the efficiency of http responses with small body
 * sizes. Hence it prevents extra calls for bodies after fetching header.
 */
class HazelcastResponseEntry : public IdentifiedDataSerializable {
public:
  HazelcastResponseEntry();
  HazelcastResponseEntry(HazelcastHeaderEntry&& header, HazelcastBodyEntry&& body);

  // hazelcast::client::serialization::IdentifiedDataSerializable
  void writeData(ObjectDataOutput& writer) const override;
  void readData(ObjectDataInput& reader) override;
  int getClassId() const override { return HAZELCAST_RESPONSE_TYPE_ID; }
  int getFactoryId() const override { return HAZELCAST_ENTRY_SERIALIZER_FACTORY_ID; }

  HazelcastHeaderEntry& header() { return response_header_; }
  HazelcastBodyEntry& body() { return response_body_; }
  const HazelcastHeaderEntry& header() const { return response_header_; }
  const HazelcastBodyEntry& body() const { return response_body_; }

  bool operator==(const HazelcastResponseEntry& other) const;

private:
  HazelcastHeaderEntry response_header_;
  HazelcastBodyEntry response_body_;
};

// To make cache compatible with Hazelcast Cpp Client, boost pointers are
// used internally instead of std.
using HazelcastHeaderPtr = boost::shared_ptr<HazelcastHeaderEntry>;
using HazelcastBodyPtr = boost::shared_ptr<HazelcastBodyEntry>;
using HazelcastResponsePtr = boost::shared_ptr<HazelcastResponseEntry>;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

class HazelcastCacheEntrySerializableFactory : public DataSerializableFactory {

public:
  static const int FACTORY_ID = HAZELCAST_ENTRY_SERIALIZER_FACTORY_ID;

  std::auto_ptr<IdentifiedDataSerializable> create(int32_t class_id) override {
    switch (class_id) {
    case HAZELCAST_BODY_TYPE_ID:
      return std::auto_ptr<IdentifiedDataSerializable>(new HazelcastBodyEntry());
    case HAZELCAST_HEADER_TYPE_ID:
      return std::auto_ptr<IdentifiedDataSerializable>(new HazelcastHeaderEntry());
    case HAZELCAST_RESPONSE_TYPE_ID:
      return std::auto_ptr<IdentifiedDataSerializable>(new HazelcastResponseEntry());
    default:
      return std::auto_ptr<IdentifiedDataSerializable>();
    }
  }
};

#pragma GCC diagnostic pop

} // namespace HazelcastHttpCache
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

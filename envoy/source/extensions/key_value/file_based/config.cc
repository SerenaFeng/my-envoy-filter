#include "source/extensions/key_value/file_based/config.h"

#include "envoy/registry/registry.h"

namespace Envoy {
namespace Extensions {
namespace KeyValue {

FileBasedKeyValueStore::FileBasedKeyValueStore(Event::Dispatcher& dispatcher,
                                               std::chrono::seconds flush_interval,
                                               Filesystem::Instance& file_system,
                                               const std::string& filename)
    : KeyValueStoreBase(dispatcher, flush_interval), file_system_(file_system),
      filename_(filename) {
  if (!file_system_.fileExists(filename_)) {
    ENVOY_LOG(info, "File for key value store does not yet exist: {}", filename);
    return;
  }
  const std::string contents = file_system_.fileReadToEnd(filename_);
  if (!parseContents(contents, store_)) {
    ENVOY_LOG(warn, "Failed to parse key value store file {}", filename);
  }
}

void FileBasedKeyValueStore::flush() {
  static constexpr Filesystem::FlagSet DefaultFlags{1 << Filesystem::File::Operation::Write |
                                                    1 << Filesystem::File::Operation::Create};
  Filesystem::FilePathAndType file_info{Filesystem::DestinationType::File, filename_};
  auto file = file_system_.createFile(file_info);
  if (!file || !file->open(DefaultFlags).return_value_) {
    ENVOY_LOG(error, "Failed to flush cache to file {}", filename_);
    return;
  }
  for (const auto& it : store_) {
    file->write(absl::StrCat(it.first.length(), "\n"));
    file->write(it.first);
    file->write(absl::StrCat(it.second.length(), "\n"));
    file->write(it.second);
  }
  file->close();
}

KeyValueStorePtr FileBasedKeyValueStoreFactory::createStore(
    const Protobuf::Message& config, ProtobufMessage::ValidationVisitor& validation_visitor,
    Event::Dispatcher& dispatcher, Filesystem::Instance& file_system) {
  const auto& typed_config = MessageUtil::downcastAndValidate<
      const envoy::extensions::common::key_value::v3::KeyValueStoreConfig&>(config,
                                                                            validation_visitor);
  const auto file_config = MessageUtil::anyConvertAndValidate<
      envoy::extensions::key_value::file_based::v3::FileBasedKeyValueStoreConfig>(
      typed_config.config().typed_config(), validation_visitor);
  auto seconds =
      std::chrono::seconds(DurationUtil::durationToSeconds(file_config.flush_interval()));
  return std::make_unique<FileBasedKeyValueStore>(dispatcher, seconds, file_system,
                                                  file_config.filename());
}

REGISTER_FACTORY(FileBasedKeyValueStoreFactory, KeyValueStoreFactory);

} // namespace KeyValue
} // namespace Extensions
} // namespace Envoy

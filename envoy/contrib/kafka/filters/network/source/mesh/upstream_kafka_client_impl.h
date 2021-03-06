#pragma once

#include <atomic>

#include "envoy/event/dispatcher.h"

#include "contrib/kafka/filters/network/source/mesh/upstream_kafka_client.h"
#include "librdkafka/rdkafkacpp.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Kafka {
namespace Mesh {

/**
 * Helper class responsible for creating librdkafka entities, so we can have mocks in tests.
 */
class LibRdKafkaUtils {
public:
  virtual ~LibRdKafkaUtils() = default;

  virtual RdKafka::Conf::ConfResult setConfProperty(RdKafka::Conf& conf, const std::string& name,
                                                    const std::string& value,
                                                    std::string& errstr) const PURE;

  virtual RdKafka::Conf::ConfResult setConfDeliveryCallback(RdKafka::Conf& conf,
                                                            RdKafka::DeliveryReportCb* dr_cb,
                                                            std::string& errstr) const PURE;

  virtual std::unique_ptr<RdKafka::Producer> createProducer(RdKafka::Conf* conf,
                                                            std::string& errstr) const PURE;
};

using RawKafkaProducerConfig = std::map<std::string, std::string>;

/**
 * Combines the librdkafka producer and its dedicated monitoring thread.
 * Producer is used to schedule messages to be sent to Kafka.
 * Independently running monitoring thread picks up delivery confirmations from producer and uses
 * Dispatcher to notify itself about delivery in worker thread.
 */
class RichKafkaProducer : public KafkaProducer,
                          public RdKafka::DeliveryReportCb,
                          private Logger::Loggable<Logger::Id::kafka> {
public:
  // Main constructor.
  RichKafkaProducer(Event::Dispatcher& dispatcher, Thread::ThreadFactory& thread_factory,
                    const RawKafkaProducerConfig& configuration);

  // Visible for testing (allows injection of LibRdKafkaUtils).
  RichKafkaProducer(Event::Dispatcher& dispatcher, Thread::ThreadFactory& thread_factory,
                    const RawKafkaProducerConfig& configuration, const LibRdKafkaUtils& utils);

  // More complex than usual.
  // Marks that monitoring thread should finish and waits for it to join.
  ~RichKafkaProducer() override;

  // KafkaProducer
  void markFinished() override;

  // KafkaProducer
  void send(const ProduceFinishCbSharedPtr origin, const std::string& topic,
            const int32_t partition, const absl::string_view key,
            const absl::string_view value) override;

  // This method gets executed by monitoring thread.
  // Does not finish until this object gets 'markFinished' invoked or gets destroyed.
  // Executed in dedicated monitoring thread.
  void checkDeliveryReports();

  // RdKafka::DeliveryReportCb
  void dr_cb(RdKafka::Message& message) override;

  // Processes the delivery confirmation.
  // Executed in Envoy worker thread.
  void processDelivery(const DeliveryMemento& memento);

  std::list<ProduceFinishCbSharedPtr>& getUnfinishedRequestsForTest();

private:
  Event::Dispatcher& dispatcher_;

  std::list<ProduceFinishCbSharedPtr> unfinished_produce_requests_;

  // Real Kafka producer (thread-safe).
  // Invoked by Envoy handler thread (to produce), and internal monitoring thread
  // (to poll for delivery events).
  std::unique_ptr<RdKafka::Producer> producer_;

  // Flag controlling monitoring threads's execution.
  std::atomic<bool> poller_thread_active_;

  // Monitoring thread that's responsible for continuously polling for new Kafka producer events.
  Thread::ThreadPtr poller_thread_;
};

using RichKafkaProducerPtr = std::unique_ptr<RichKafkaProducer>;

} // namespace Mesh
} // namespace Kafka
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy

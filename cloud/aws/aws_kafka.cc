//  Copyright (c) 2016-present, Rockset, Inc.  All rights reserved.
//
// This file defines an AWS-Kinesis environment for rocksdb.
// A log file maps to a stream in Kinesis.
//
#include "cloud/aws/aws_kafka.h"

#ifdef USE_KAFKA

#include <fstream>
#include <iostream>

#include "cloud/aws/aws_env.h"
#include "cloud/aws/aws_file.h"
#include "rocksdb/status.h"
#include "util/coding.h"
#include "util/stderr_logger.h"
#include "util/string_util.h"

namespace rocksdb {

const std::chrono::microseconds KafkaWritableFile::kFlushTimeout =
    std::chrono::seconds(10);

KafkaWritableFile::KafkaWritableFile(
    AwsEnv* env, const std::string& fname, const EnvOptions& options,
    std::shared_ptr<RdKafka::Producer> producer,
    std::shared_ptr<RdKafka::Topic> topic)
    : CloudLogWritableFile(env, fname, options),
      producer_(producer),
      topic_(topic),
      current_offset_(0) {
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[kafka] WritableFile opened file %s", fname_.c_str());
}

KafkaWritableFile::~KafkaWritableFile() {}

Status KafkaWritableFile::ProduceRaw(const std::string& operation_name,
                                     const Slice& message) {
  if (!status_.ok()){
      return status_;
  }

  RdKafka::ErrorCode resp;
  resp = producer_->produce(
      topic_.get(), RdKafka::Topic::PARTITION_UA /* UnAssigned */,
      RdKafka::Producer::RK_MSG_COPY /* Copy payload */, (void*)message.data(),
      message.size(), &fname_ /* Partitioning key */, nullptr);

  if (resp == RdKafka::ERR_NO_ERROR) {
    Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
        "[kafka] WritableFile %s file %s %ld", fname_.c_str(),
        operation_name.c_str(), message.size());
    return Status::OK();
  } else if (resp == RdKafka::ERR__QUEUE_FULL) {
    const std::string formatted_err = RdKafka::err2str(resp);
    Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
        "[kafka] WritableFile src %s %s error %s", fname_.c_str(),
        operation_name.c_str(), formatted_err.c_str());

    return Status::Busy(topic_->name().c_str(), RdKafka::err2str(resp).c_str());
  } else {
    const std::string formatted_err = RdKafka::err2str(resp);
    Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
        "[kafka] WritableFile src %s %s error %s", fname_.c_str(),
        operation_name.c_str(), formatted_err.c_str());

    return Status::IOError(topic_->name().c_str(),
                           RdKafka::err2str(resp).c_str());
  }
  current_offset_ += message.size();

  return Status::OK();
}

Status KafkaWritableFile::Append(const Slice& data) {
  std::string serialized_data;
  CloudLogController::SerializeLogRecordAppend(fname_, data, current_offset_,
                                               &serialized_data);

  return ProduceRaw("Append", serialized_data);
}

Status KafkaWritableFile::Close() {
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[kafka] S3WritableFile closing %s", fname_.c_str());

  std::string serialized_data;
  CloudLogController::SerializeLogRecordClosed(fname_, current_offset_,
                                               &serialized_data);

  return ProduceRaw("Close", serialized_data);
}

bool KafkaWritableFile::IsSyncThreadSafe() const {
  return true;
}

Status KafkaWritableFile::Sync() {
  return Flush();
}

Status KafkaWritableFile::Flush() {
  std::chrono::microseconds start(env_->NowMicros());

  bool done = false;
  bool timeout = false;
  while (status_.ok() && !(done = (producer_->outq_len() == 0)) &&
         !(timeout = (std::chrono::microseconds(env_->NowMicros()) - start >
                      kFlushTimeout))) {
    Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
        "[kafka] WritableFile src %s "
        "Waiting on flush: Output queue length: %d",
        fname_.c_str(), producer_->outq_len());

    producer_->poll(500);
  }

  if (done) {
    Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
        "[kafka] WritableFile src %s Flushed", fname_.c_str());
  } else if (timeout) {
    Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
        "[kafka] WritableFile src %s Flushing timed out after %lldus",
        fname_.c_str(), kFlushTimeout);
    status_ = Status::TimedOut();
  } else {
    Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
        "[kafka] WritableFile src %s Flush interrupted", fname_.c_str());
  }

  return status_;
}

Status KafkaWritableFile::LogDelete() {
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_, "[kafka] LogDelete %s",
      fname_.c_str());

  std::string serialized_data;
  CloudLogController::SerializeLogRecordDelete(fname_, &serialized_data);

  return ProduceRaw("Delete", serialized_data);
}

KafkaController::KafkaController(AwsEnv* env, std::shared_ptr<Logger> info_log,
                                 std::unique_ptr<RdKafka::Producer> producer,
                                 std::unique_ptr<RdKafka::Consumer> consumer)
    : CloudLogController(env, info_log),
      producer_(std::move(producer)),
      consumer_(std::move(consumer)) {
  const std::string topic_name = GetStreamName(env_->GetSrcBucketPrefix());

  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[%s] KafkaController opening stream %s using cachedir '%s'",
      GetTypeName().c_str(), topic_name.c_str(), cache_dir_.c_str());

  std::string pt_errstr, ct_errstr;

  // Initialize stream name.
  RdKafka::Topic* producer_topic =
      RdKafka::Topic::create(producer_.get(), topic_name, NULL, pt_errstr);

  RdKafka::Topic* consumer_topic =
      RdKafka::Topic::create(consumer_.get(), topic_name, NULL, ct_errstr);

  RdKafka::Queue* consuming_queue = RdKafka::Queue::create(consumer_.get());

  assert(producer_topic != nullptr);
  assert(consumer_topic != nullptr);
  assert(consuming_queue != nullptr);

  consuming_queue_.reset(consuming_queue);

  producer_topic_.reset(producer_topic);
  consumer_topic_.reset(consumer_topic);
}

KafkaController::~KafkaController() {
  for (size_t i = 0; i < partitions_.size(); i++) {
    consumer_->stop(consumer_topic_.get(), partitions_[i]->partition());
  }

  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[%s] KafkaController closed.", GetTypeName().c_str());
}

Status KafkaController::create(AwsEnv* env, std::shared_ptr<Logger> info_log,
                               const CloudEnvOptions& cloud_env_options,
                               KafkaController** output) {
  Status st = Status::OK();
  std::string conf_errstr, producer_errstr, consumer_errstr;
  const auto& kconf =
      cloud_env_options.kafka_log_options.client_config_params;

  std::unique_ptr<RdKafka::Conf> conf(
      RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));

  if (kconf.empty()) {
    st = Status::InvalidArgument("No configs specified to kafka client");

    Log(InfoLogLevel::ERROR_LEVEL, info_log,
        "[aws] NewAwsEnv Kafka conf error: %s", st.ToString().c_str());
    return st;
  }

  for (auto const& item : kconf) {
    if (conf->set(item.first,
                  item.second,
                  conf_errstr) != RdKafka::Conf::CONF_OK) {
      st = Status::InvalidArgument("Failed adding specified conf to Kafka conf",
                                   conf_errstr.c_str());

      Log(InfoLogLevel::ERROR_LEVEL, info_log,
          "[aws] NewAwsEnv Kafka conf set error: %s", st.ToString().c_str());
      return st;
    }
  }

  {
    std::unique_ptr<RdKafka::Producer> producer(
        RdKafka::Producer::create(conf.get(), producer_errstr));
    std::unique_ptr<RdKafka::Consumer> consumer(
        RdKafka::Consumer::create(conf.get(), consumer_errstr));

    if (!producer) {
      st = Status::InvalidArgument("Failed creating Kafka producer",
                                   producer_errstr.c_str());

      Log(InfoLogLevel::ERROR_LEVEL, info_log,
          "[aws] NewAwsEnv Kafka producer error: %s", st.ToString().c_str());
    } else if (!consumer) {
      st = Status::InvalidArgument("Failed creating Kafka consumer",
                                   consumer_errstr.c_str());

      Log(InfoLogLevel::ERROR_LEVEL, info_log,
          "[aws] NewAwsEnv Kafka consumer error: %s", st.ToString().c_str());
    } else {
      *output = new KafkaController(env, info_log, std::move(producer),
                                    std::move(consumer));

      if (*output == nullptr) {
        st = Status::IOError("Error in creating Kafka controller");
      }
    }
  }

  return st;
}

Status KafkaController::TailStream() {
  InitializePartitions();

  if (!status_.ok()) {
    return status_;
  }

  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_, "[%s] TailStream topic %s %s",
      GetTypeName().c_str(), consumer_topic_->name().c_str(),
      status_.ToString().c_str());

  Status lastErrorStatus;
  int retryAttempt = 0;
  while (env_->IsRunning()) {
    if (retryAttempt > 10) {
      status_ = lastErrorStatus;
      break;
    }

    std::unique_ptr<RdKafka::Message> message(
        consumer_->consume(consuming_queue_.get(), 1000));

    switch (message->err()) {
      case RdKafka::ERR_NO_ERROR: {
        /* Real message */
        Slice sl(static_cast<const char*>(message->payload()),
                 static_cast<size_t>(message->len()));

        // Apply the payload to local filesystem
        status_ = Apply(sl);
        if (!status_.ok()) {
          Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
              "[%s] error processing message size %ld "
              "extracted from stream %s %s",
              GetTypeName().c_str(), message->len(),
              consumer_topic_->name().c_str(), status_.ToString().c_str());
        } else {
          Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
              "[%s] successfully processed message size %ld "
              "extracted from stream %s %s",
              GetTypeName().c_str(), message->len(),
              consumer_topic_->name().c_str(), status_.ToString().c_str());
        }

        // Remember last read offset from topic (currently unused).
        partitions_[message->partition()]->set_offset(message->offset());
        break;
      }
      case RdKafka::ERR__PARTITION_EOF: {
        // There are no new messages.
        consumer_->poll(50);
        break;
      }
      default: {
        lastErrorStatus =
            Status::IOError(consumer_topic_->name().c_str(),
                            RdKafka::err2str(message->err()).c_str());

        Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
            "[%s] error reading %s %s", GetTypeName().c_str(),
            consumer_topic_->name().c_str(),
            RdKafka::err2str(message->err()).c_str());

        ++retryAttempt;
        break;
      }
    }
  }
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[%s] TailStream topic %s finished: %s", GetTypeName().c_str(),
      consumer_topic_->name().c_str(), status_.ToString().c_str());

  return status_;
}

Status KafkaController::InitializePartitions() {
  if (!status_.ok()) {
    return status_;
  }

  RdKafka::Metadata* result;
  RdKafka::ErrorCode err =
      consumer_->metadata(false, consumer_topic_.get(), &result, 5000);

  std::unique_ptr<RdKafka::Metadata> metadata(result);

  if (err != RdKafka::ERR_NO_ERROR) {
    status_ = Status::IOError(consumer_topic_->name().c_str(),
                              RdKafka::err2str(err).c_str());

    Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
        "[%s] S3ReadableFile file %s Unable to find shards %s",
        GetTypeName().c_str(), consumer_topic_->name().c_str(),
        status_.ToString().c_str());

    return status_;
  }

  assert(metadata->topics()->size() == 1);

  const RdKafka::TopicMetadata* topic_metadata = metadata->topics()->at(0);
  if (topic_metadata->partitions()->size() == 0) {
    // Topic's currently empty. As soon as writing starts, there'll be a
    // partition.
    partitions_.push_back(std::shared_ptr<RdKafka::TopicPartition>(
        RdKafka::TopicPartition::create(topic_metadata->topic(), 0)));
    partitions_.back()->set_offset(0);
  } else {
    assert(topic_metadata->partitions()->size() == 1);

    for (auto partition_metadata : *(topic_metadata->partitions())) {
      partitions_.push_back(std::shared_ptr<RdKafka::TopicPartition>(
          RdKafka::TopicPartition::create(topic_metadata->topic(),
                                          partition_metadata->id())));
      partitions_.back()->set_offset(0);
    }
  }

  for (size_t i = 0; i < partitions_.size(); i++) {
    if (partitions_[i]->offset() > 0) {
      continue;
    }

    consumer_->start(consumer_topic_.get(), partitions_[i]->partition(),
                     partitions_[i]->offset(), consuming_queue_.get());
  }

  return status_;
}

CloudLogWritableFile* KafkaController::CreateWritableFile(
    const std::string& fname, const EnvOptions& options) {
  return dynamic_cast<CloudLogWritableFile*>(
      new KafkaWritableFile(env_, fname, options, producer_, producer_topic_));
}

}  // namespace rocksdb

#endif /* USE_KAFKA */

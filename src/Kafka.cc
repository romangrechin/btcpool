/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */
#include "Kafka.h"

#include "Common.h"
#include <glog/logging.h>

static
void kafkaLogger(const rd_kafka_t *rk, int level,
                 const char *fac, const char *buf) {
  LOG(INFO) << "RDKAFKA-" << level << "-" << fac << ": "
  << (rk ? rd_kafka_name(rk) : NULL) << buf;
}

static
void print_partition_list(const rd_kafka_topic_partition_list_t *partitions) {
  int i;
  for (i = 0 ; i < partitions->cnt ; i++) {
    LOG(ERROR) << i << " " << partitions->elems[i].topic<< " ["
    << partitions->elems[i].partition << "] offset " << partitions->elems[i].offset;
  }
}

static
void rebalance_cb(rd_kafka_t *rk,
                  rd_kafka_resp_err_t err,
                  rd_kafka_topic_partition_list_t *partitions,
                  void *opaque) {
  LOG(ERROR) << "consumer group rebalanced: ";
  switch (err)
  {
    case RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS:
      LOG(ERROR) << "assigned:";
      print_partition_list(partitions);
      rd_kafka_assign(rk, partitions);
      break;

    case RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS:
      LOG(ERROR) << "revoked:";
      print_partition_list(partitions);
      rd_kafka_assign(rk, NULL);
      break;

    default:
      fprintf(stderr, "failed: %s\n", rd_kafka_err2str(err));
      rd_kafka_assign(rk, NULL);
      break;
  }
}


///////////////////////////////// KafkaConsumer ////////////////////////////////
KafkaConsumer::KafkaConsumer(const char *brokers, const char *topic,
                             int partition):
brokers_(brokers), topicStr_(topic),
partition_(partition), conf_(rd_kafka_conf_new()),
consumer_(nullptr),
topic_(nullptr)
{
  rd_kafka_conf_set_log_cb(conf_, kafkaLogger);  // set logger
  LOG(INFO) << "consumer librdkafka version: " << rd_kafka_version_str();
}

KafkaConsumer::~KafkaConsumer() {
  if (topic_ == nullptr) {
    return;
  }

  /* Stop consuming */
  rd_kafka_consume_stop(topic_, partition_);
  while (rd_kafka_outq_len(consumer_) > 0) {
    rd_kafka_poll(consumer_, 10);
  }
  rd_kafka_topic_destroy(topic_);  // Destroy topic
  rd_kafka_destroy(consumer_);     // Destroy the handle
}


//
// offset:
//     RD_KAFKA_OFFSET_BEGINNING
//     RD_KAFKA_OFFSET_END
//     RD_KAFKA_OFFSET_STORED
//     RD_KAFKA_OFFSET_TAIL(CNT)
//
bool KafkaConsumer::setup(int64_t offset) {
  char errstr[1024];
  //
  // rdkafka options:
  //
  // message.max.bytes:
  //         Maximum transmit message size. 20000000 = 20,000,000
  //
  // TODO: increase 'message.max.bytes' in the feature
  //
  const vector<string> conKeys = {"message.max.bytes", "compression.codec",
    "queued.max.messages.kbytes","fetch.message.max.bytes","fetch.wait.max.ms"
  };
  const vector<string> conVals = {"20000000", "snappy",
    "20000000","20000000","5"};
  assert(conKeys.size() == conVals.size());

  for (size_t i = 0; i < conKeys.size(); i++) {
    if (rd_kafka_conf_set(conf_,
                          conKeys[i].c_str(), conVals[i].c_str(),
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      LOG(ERROR) << "kafka set conf failure: " << errstr
      << ", key: " << conKeys[i] << ", val: " << conVals[i];
      return false;
    }
  }

  rd_kafka_topic_conf_t *topicConf = rd_kafka_topic_conf_new();

  /* create consumer_ */
  if (!(consumer_ = rd_kafka_new(RD_KAFKA_CONSUMER, conf_,
                                 errstr, sizeof(errstr)))) {
    LOG(ERROR) << "kafka create consumer failure: " << errstr;
    return false;
  }

#ifndef NDEBUG
  rd_kafka_set_log_level(consumer_, 7 /* LOG_DEBUG */);
#else
  rd_kafka_set_log_level(consumer_, 0);
#endif

  /* Add brokers */
  LOG(INFO) << "add brokers: " << brokers_;
  if (rd_kafka_brokers_add(consumer_, brokers_.c_str()) == 0) {
    LOG(ERROR) << "kafka add brokers failure";
    return false;
  }

  /* Create topic */
  LOG(INFO) << "create topic handle: " << topicStr_;
  topic_ = rd_kafka_topic_new(consumer_, topicStr_.c_str(), topicConf);
  topicConf = NULL; /* Now owned by topic */

  /* Start consuming */
  if (rd_kafka_consume_start(topic_, partition_, offset) == -1) {
    LOG(ERROR) << "failed to start consuming: " << rd_kafka_err2str(rd_kafka_errno2err(errno));
    return false;
  }

  return true;
}

bool KafkaConsumer::checkAlive() {
  if (consumer_ == nullptr) {
    return false;
  }

  // check kafka meta, maybe there is better solution to check brokers
  rd_kafka_resp_err_t err;
  const struct rd_kafka_metadata *metadata;
  /* Fetch metadata */
  err = rd_kafka_metadata(consumer_, topic_ ? 0 : 1,
                          topic_, &metadata, 3000/* timeout_ms */);
  if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
    LOG(FATAL) << "Failed to acquire metadata: " << rd_kafka_err2str(err);
    return false;
  }
  rd_kafka_metadata_destroy(metadata);  // no need to print out meta data

  return true;
}

//
// don't forget to call rd_kafka_message_destroy() after consumer()
//
rd_kafka_message_t *KafkaConsumer::consumer(int timeout_ms) {
  return rd_kafka_consume(topic_, partition_, timeout_ms);
}



//////////////////////////// KafkaHighLevelConsumer ////////////////////////////
KafkaHighLevelConsumer::KafkaHighLevelConsumer(const char *brokers, const char *topic,
                                               int partition, const string &groupStr):
brokers_(brokers), topicStr_(topic),
groupStr_(groupStr), partition_(partition),
conf_(rd_kafka_conf_new()), consumer_(nullptr), topics_(nullptr)
{
  rd_kafka_conf_set_log_cb(conf_, kafkaLogger);  // set logger
  LOG(INFO) << "consumer librdkafka version: " << rd_kafka_version_str();
}

KafkaHighLevelConsumer::~KafkaHighLevelConsumer() {
  rd_kafka_resp_err_t err;
  if (topics_ == nullptr) {
    return;
  }

  /* Stop consuming */
  err = rd_kafka_consumer_close(consumer_);
  if (err)
    LOG(ERROR) << "failed to close consumer: " << rd_kafka_err2str(err);
  else
    LOG(INFO) << "consumer closed";

  rd_kafka_topic_partition_list_destroy(topics_);
  rd_kafka_destroy(consumer_);

  /* Let background threads clean up and terminate cleanly. */
  int run = 5;
  while (run-- > 0 && rd_kafka_wait_destroyed(1000) == -1)
    LOG(INFO) << "waiting for librdkafka to decommission";

  if (run <= 0)
    rd_kafka_dump(stdout, consumer_);
}

bool KafkaHighLevelConsumer::setup() {
  char errstr[1024];
  rd_kafka_resp_err_t err;
  //
  // rdkafka options:
  //
  // message.max.bytes:
  //         Maximum transmit message size. 20000000 = 20,000,000
  //
  // TODO: increase 'message.max.bytes' in the feature
  //
  const vector<string> conKeys = {"message.max.bytes", "compression.codec",
    "queued.max.messages.kbytes","fetch.message.max.bytes","fetch.wait.max.ms",
    "group.id" /* Consumer groups require a group id */
  };
  const vector<string> conVals = {"20000000", "snappy",
    "20000000","20000000","5", groupStr_.c_str()};
  assert(conKeys.size() == conVals.size());

  for (size_t i = 0; i < conKeys.size(); i++) {
    if (rd_kafka_conf_set(conf_,
                          conKeys[i].c_str(), conVals[i].c_str(),
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      LOG(ERROR) << "kafka set conf failure: " << errstr
      << ", key: " << conKeys[i] << ", val: " << conVals[i];
      return false;
    }
  }

  /* topic conf */
  rd_kafka_topic_conf_t *topicConf = rd_kafka_topic_conf_new();

  /* Consumer groups always use broker based offset storage */
  // offset.store.method
  if (rd_kafka_topic_conf_set(topicConf, "offset.store.method", "broker",
                              errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    LOG(ERROR) << "kafka set 'offset.store.method' failure: " << errstr;
    return false;
  }
  // auto.offset.reset
  if (rd_kafka_topic_conf_set(topicConf, "auto.offset.reset", "smallest",
                              errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    LOG(ERROR) << "kafka set 'auto.offset.reset' failure: " << errstr;
    return false;
  }

  /* Set default topic config for pattern-matched topics. */
  rd_kafka_conf_set_default_topic_conf(conf_, topicConf);

  /* Callback called on partition assignment changes */
  //
  // NOTE: right now, we use only 1 patition, I think it's not going to happen
  //
  rd_kafka_conf_set_rebalance_cb(conf_, rebalance_cb);

  /* create consumer_ */
  if (!(consumer_ = rd_kafka_new(RD_KAFKA_CONSUMER, conf_,
                                 errstr, sizeof(errstr)))) {
    LOG(ERROR) << "kafka create consumer failure: " << errstr;
    return false;
  }

#ifndef NDEBUG
  rd_kafka_set_log_level(consumer_, 7 /* LOG_DEBUG */);
#else
  rd_kafka_set_log_level(consumer_, 0);
#endif

  /* Add brokers */
  LOG(INFO) << "add brokers: " << brokers_;
  if (rd_kafka_brokers_add(consumer_, brokers_.c_str()) == 0) {
    LOG(ERROR) << "kafka add brokers failure";
    return false;
  }

  /* Redirect rd_kafka_poll() to consumer_poll() */
  rd_kafka_poll_set_consumer(consumer_);

  /* Create a new list/vector Topic+Partition container */
  int size = 1;  // only 1 container
  topics_ = rd_kafka_topic_partition_list_new(size);
  rd_kafka_topic_partition_list_add(topics_, topicStr_.c_str(), partition_);

  if ((err = rd_kafka_assign(consumer_, topics_))) {
    LOG(ERROR) << "failed to assign partitions: " << rd_kafka_err2str(err);
    return false;
  }

  return true;
}

//
// don't forget to call rd_kafka_message_destroy() after consumer()
//
rd_kafka_message_t *KafkaHighLevelConsumer::consumer(int timeout_ms) {
  return rd_kafka_consumer_poll(consumer_, timeout_ms);
}



///////////////////////////////// KafkaProducer ////////////////////////////////
KafkaProducer::KafkaProducer(const char *brokers, const char *topic, int partition):
brokers_(brokers), topicStr_(topic), partition_(partition), conf_(rd_kafka_conf_new()),
producer_(nullptr), topic_(nullptr)
{
  rd_kafka_conf_set_log_cb(conf_, kafkaLogger);  // set logger
  LOG(INFO) << "producer librdkafka version: " << rd_kafka_version_str();
}

KafkaProducer::~KafkaProducer() {
  /* Poll to handle delivery reports */
  rd_kafka_poll(producer_, 0);

  /* Wait for messages to be delivered */
  while (rd_kafka_outq_len(producer_) > 0) {
    rd_kafka_poll(producer_, 100);
  }
  rd_kafka_topic_destroy(topic_);  // Destroy topic
  rd_kafka_destroy(producer_);     // Destroy the handle
}


bool KafkaProducer::setup() {
  char errstr[1024];
  //
  // rdkafka options:
  //
  // queue.buffering.max.ms:
  //         set to 1 (0 is an illegal value here), deliver msg as soon as possible.
  // queue.buffering.max.messages:
  //         submit share will be a lot
  // message.max.bytes:
  //         Maximum transmit message size. 20000000 = 20,000,000
  //
  // TODO: increase 'message.max.bytes' in the feature
  //
  const vector<string> conKeys = {"message.max.bytes", "compression.codec",
    "queue.buffering.max.ms", "queue.buffering.max.messages",
    "batch.num.messages"};
  const vector<string> conVals = {"20000000", "snappy", "5000", "20000", "50000"};
  assert(conKeys.size() == conVals.size());

  for (size_t i = 0; i < conKeys.size(); i++) {
    if (rd_kafka_conf_set(conf_,
                          conKeys[i].c_str(), conVals[i].c_str(),
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      LOG(ERROR) << "kafka set conf failure: " << errstr
      << ", key: " << conKeys[i] << ", val: " << conVals[i];
      return false;
    }
  }

  /* create producer */
  if (!(producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, conf_,
                                 errstr, sizeof(errstr)))) {
    LOG(ERROR) << "kafka create producer failure: " << errstr;
    return false;
  }

#ifndef NDEBUG
  rd_kafka_set_log_level(producer_, 7 /* LOG_DEBUG */);
#else
  rd_kafka_set_log_level(producer_, 0);
#endif

  /* Add brokers */
  LOG(INFO) << "add brokers: " << brokers_;
  if (rd_kafka_brokers_add(producer_, brokers_.c_str()) == 0) {
    LOG(ERROR) << "kafka add brokers failure";
    return false;
  }

  /* Create topic */
  LOG(INFO) << "create topic handle: " << topicStr_;
  rd_kafka_topic_conf_t *topicConf = rd_kafka_topic_conf_new();
  topic_ = rd_kafka_topic_new(producer_, topicStr_.c_str(), topicConf);
  topicConf = NULL; /* Now owned by topic */

  return true;
}

bool KafkaProducer::checkAlive() {
  if (producer_ == nullptr) {
    return false;
  }

  // check kafka meta, maybe there is better solution to check brokers
  rd_kafka_resp_err_t err;
  const struct rd_kafka_metadata *metadata;
  /* Fetch metadata */
  err = rd_kafka_metadata(producer_, topic_ ? 0 : 1,
                          topic_, &metadata, 3000/* timeout_ms */);
  if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
    LOG(FATAL) << "Failed to acquire metadata: " << rd_kafka_err2str(err);
    return false;
  }
  rd_kafka_metadata_destroy(metadata);  // no need to print out meta data

  return true;
}


void KafkaProducer::produce(const void *payload, size_t len) {
  // rd_kafka_produce() is non-blocking
  // Returns 0 on success or -1 on error
  int res = rd_kafka_produce(topic_, partition_, RD_KAFKA_MSG_F_COPY,
                             (void *)payload, len,
                             NULL, 0,  /* Optional key and its length */
                             /* Message opaque, provided in delivery report
                              * callback as msg_opaque. */
                             NULL);
  if (res == -1) {
    LOG(ERROR) << "produce to topic [ " << rd_kafka_topic_name(topic_)
    << "]: " << rd_kafka_err2str(rd_kafka_errno2err(errno));
  }
}

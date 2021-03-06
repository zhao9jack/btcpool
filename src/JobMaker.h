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
#ifndef JOB_MAKER_H_
#define JOB_MAKER_H_

#include "Common.h"
#include "Kafka.h"
#include "Stratum.h"

#include "bitcoin/uint256.h"
#include "bitcoin/base58.h"

#include <map>
#include <deque>

class JobMaker {
  atomic<bool> running_;
  mutex lock_;

  string kafkaBrokers_;
  KafkaConsumer kafkaConsumer_;
  KafkaProducer kafkaProducer_;

  uint32_t currBestHeight_;
  uint32_t lastJobSendTime_;
  bool isLastJobEmptyBlock_;
  bool isLastJobNewHeight_;
  uint32_t stratumJobInterval_;


  string poolCoinbaseInfo_;
  CBitcoinAddress poolPayoutAddr_;

  uint32_t kGbtLifeTime_;
  std::map<uint64_t/* timestamp + height */, string> rawgbtMap_;  // sorted gbt by timestamp

  deque<uint256> lastestGbtHash_;
  uint32_t blockVersion_;

  void consumeRawGbtMsg(rd_kafka_message_t *rkmessage, bool needToSend);
  void addRawgbt(const char *str, size_t len);

  void clearTimeoutGbt();
  void findNewBestHeight(std::map<uint64_t/* height + ts */, const char *> *gbtByHeight);
  bool isReachTimeout();
  void sendStratumJob(const char *gbt);

  void checkAndSendStratumJob();

public:
  JobMaker(const string &kafkaBrokers, uint32_t stratumJobInterval,
           const string &payoutAddr, uint32_t gbtLifeTime);
  ~JobMaker();

  bool init();
  void stop();
  void run();
};

#endif

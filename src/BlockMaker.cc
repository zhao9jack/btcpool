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
#include "BlockMaker.h"

#include <boost/thread.hpp>

#include "utilities_js.hpp"


////////////////////////////////// BlockMaker //////////////////////////////////
BlockMaker::BlockMaker(const char *kafkaBrokers, const MysqlConnectInfo &poolDB):
running_(true),
kMaxRawGbtNum_(100),    /* if 5 seconds a rawgbt, will hold 100*5/60 = 8 mins rawgbt */
kMaxStratumJobNum_(120), /* if 30 seconds a stratum job, will hold 60 mins stratum job */
kafkaConsumerRawGbt_     (kafkaBrokers, KAFKA_TOPIC_RAWGBT,       0/* patition */),
kafkaConsumerStratumJob_ (kafkaBrokers, KAFKA_TOPIC_STRATUM_JOB,  0/* patition */),
kafkaConsumerSovledShare_(kafkaBrokers, KAFKA_TOPIC_SOLVED_SHARE, 0/* patition */),
poolDB_(poolDB)
{
}

BlockMaker::~BlockMaker() {
  if (threadConsumeRawGbt_.joinable())
    threadConsumeRawGbt_.join();

  if (threadConsumeStratumJob_.joinable())
    threadConsumeStratumJob_.join();
}

void BlockMaker::stop() {
  if (!running_)
    return;

  running_ = false;
  LOG(INFO) << "stop block maker";
}

void BlockMaker::addBitcoind(const string &rpcAddress, const string &rpcUserpass) {
  bitcoindRpcUri_.push_back(make_pair(rpcAddress, rpcUserpass));
}

bool BlockMaker::init() {
  if (!checkBitcoinds())
    return false;

  //
  // Raw Gbt
  //
  // we need to consume the latest N messages
  if (kafkaConsumerRawGbt_.setup(RD_KAFKA_OFFSET_TAIL(kMaxRawGbtNum_)) == false) {
    LOG(INFO) << "setup kafkaConsumerRawGbt_ fail";
    return false;
  }
  if (!kafkaConsumerRawGbt_.checkAlive()) {
    LOG(ERROR) << "kafka brokers is not alive: kafkaConsumerRawGbt_";
    return false;
  }

  //
  // Stratum Job
  //
  // we need to consume the latest 2 messages, just in case
  if (kafkaConsumerStratumJob_.setup(RD_KAFKA_OFFSET_TAIL(kMaxStratumJobNum_)) == false) {
    LOG(INFO) << "setup kafkaConsumerSovledShare_ fail";
    return false;
  }
  if (!kafkaConsumerStratumJob_.checkAlive()) {
    LOG(ERROR) << "kafka brokers is not alive: kafkaConsumerSovledShare_";
    return false;
  }

  //
  // Sloved Share
  //
  // we need to consume the latest 2 messages, just in case
  if (kafkaConsumerSovledShare_.setup(RD_KAFKA_OFFSET_TAIL(2)) == false) {
    LOG(INFO) << "setup kafkaConsumerSovledShare_ fail";
    return false;
  }
  if (!kafkaConsumerSovledShare_.checkAlive()) {
    LOG(ERROR) << "kafka brokers is not alive: kafkaConsumerSovledShare_";
    return false;
  }

  return true;
}

void BlockMaker::consumeRawGbt(rd_kafka_message_t *rkmessage) {
  // check error
  if (rkmessage->err) {
    if (rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
      // Reached the end of the topic+partition queue on the broker.
      // Not really an error.
      //      LOG(INFO) << "consumer reached end of " << rd_kafka_topic_name(rkmessage->rkt)
      //      << "[" << rkmessage->partition << "] "
      //      << " message queue at offset " << rkmessage->offset;
      // acturlly
      return;
    }

    LOG(ERROR) << "consume error for topic " << rd_kafka_topic_name(rkmessage->rkt)
    << "[" << rkmessage->partition << "] offset " << rkmessage->offset
    << ": " << rd_kafka_message_errstr(rkmessage);

    if (rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION ||
        rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC) {
      LOG(FATAL) << "consume fatal";
      stop();
    }
    return;
  }

  LOG(INFO) << "received rawgbt message, len: " << rkmessage->len;
  addRawgbt((const char *)rkmessage->payload, rkmessage->len);
}

void BlockMaker::addRawgbt(const char *str, size_t len) {
  JsonNode r;
  if (!JsonNode::parse(str, str + len, r)) {
    LOG(ERROR) << "parse rawgbt message to json fail";
    return;
  }
  if (r["created_at_ts"].type()         != Utilities::JS::type::Int ||
      r["block_template_base64"].type() != Utilities::JS::type::Str ||
      r["gbthash"].type()               != Utilities::JS::type::Str) {
    LOG(ERROR) << "invalid rawgbt: missing fields";
    return;
  }

  const uint256 gbtHash = uint256(r["gbthash"].str());
  if (rawGbtMap_.find(gbtHash) != rawGbtMap_.end()) {
    LOG(ERROR) << "already exist raw gbt, ingore: " << gbtHash.ToString();
    return;
  }

  const string gbt = DecodeBase64(r["block_template_base64"].str());
  assert(gbt.length() > 64);  // valid gbt string's len at least 64 bytes

  JsonNode nodeGbt;
  if (!JsonNode::parse(gbt.c_str(), gbt.c_str() + gbt.length(), nodeGbt)) {
    LOG(ERROR) << "parse gbt message to json fail";
    return;
  }
  JsonNode jgbt = nodeGbt["result"];

  // transaction without coinbase_tx
  shared_ptr<vector<CTransaction>> vtxs = std::make_shared<std::vector<CTransaction>>();
  for (JsonNode & node : jgbt["transactions"].array()) {
    CTransaction tx;
    DecodeHexTx(tx, node["data"].str());
    vtxs->push_back(tx);
  }

  LOG(INFO) << "insert rawgbt: " << gbtHash.ToString() << ", txs: " << vtxs->size();
  insertRawGbt(gbtHash, vtxs);
}

void BlockMaker::insertRawGbt(const uint256 &gbtHash,
                              shared_ptr<vector<CTransaction>> vtxs) {
  ScopeLock ls(rawGbtLock_);

  // insert rawgbt
  rawGbtMap_[gbtHash] = vtxs;
  rawGbtQ_.push_back(gbtHash);

  // remove rawgbt if need
  while (rawGbtQ_.size() > kMaxRawGbtNum_) {
    const uint256 h = *rawGbtQ_.begin();

    rawGbtMap_.erase(h);   // delete from map
    rawGbtQ_.pop_front();  // delete from Q
  }
}

void BlockMaker::consumeSovledShare(rd_kafka_message_t *rkmessage) {
  // check error
  if (rkmessage->err) {
    if (rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
      // Reached the end of the topic+partition queue on the broker.
      // Not really an error.
      //      LOG(INFO) << "consumer reached end of " << rd_kafka_topic_name(rkmessage->rkt)
      //      << "[" << rkmessage->partition << "] "
      //      << " message queue at offset " << rkmessage->offset;
      // acturlly
      return;
    }

    LOG(ERROR) << "consume error for topic " << rd_kafka_topic_name(rkmessage->rkt)
    << "[" << rkmessage->partition << "] offset " << rkmessage->offset
    << ": " << rd_kafka_message_errstr(rkmessage);

    if (rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION ||
        rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC) {
      LOG(FATAL) << "consume fatal";
      stop();
    }
    return;
  }

  LOG(INFO) << "received SolvedShare message, len: " << rkmessage->len;

  //
  // solved share message:  FoundBlock + coinbase_Tx
  //
  FoundBlock foundBlock;
  CBlockHeader blkHeader;
  vector<char> coinbaseTxBin;

  {
    if (rkmessage->len <= sizeof(FoundBlock)) {
      LOG(ERROR) << "invalid SolvedShare length: " << rkmessage->len;
      return;
    }
    coinbaseTxBin.resize(rkmessage->len - sizeof(FoundBlock));

    // foundBlock
    memcpy((uint8_t *)&foundBlock, (const uint8_t *)rkmessage->payload, sizeof(FoundBlock));

    // coinbase tx
    memcpy((uint8_t *)coinbaseTxBin.data(),
           (const uint8_t *)rkmessage->payload + sizeof(FoundBlock),
           coinbaseTxBin.size());
    // copy header
    memcpy((uint8_t *)&blkHeader, foundBlock.header80_, sizeof(CBlockHeader));
  }

  // get gbtHash and rawgbt (vtxs)
  uint256 gbtHash;
  shared_ptr<vector<CTransaction>> vtxs;
  {
    ScopeLock sl(jobIdMapLock_);
    if (jobId2GbtHash_.find(foundBlock.jobId_) != jobId2GbtHash_.end()) {
      gbtHash = jobId2GbtHash_[foundBlock.jobId_];
    }
  }
  {
    ScopeLock ls(rawGbtLock_);
    if (rawGbtMap_.find(gbtHash) == rawGbtMap_.end()) {
      LOG(ERROR) << "can't find this gbthash in rawGbtMap_: " << gbtHash.ToString();
      return;
    }
    vtxs = rawGbtMap_[gbtHash];
  }
  assert(vtxs.get() != nullptr);

  //
  // build new block
  //
  CBlock newblk(blkHeader);

  // put coinbase tx
  {
    CSerializeData data(coinbaseTxBin);
    newblk.vtx.push_back(CTransaction());
    CDataStream c(data, SER_NETWORK, BITCOIN_PROTOCOL_VERSION);
    c >> newblk.vtx[newblk.vtx.size() - 1];
  }

  // put other txs
  if (vtxs->size()) {
    newblk.vtx.insert(newblk.vtx.end(), vtxs->begin(), vtxs->end());
  }

  // submit to bitcoind
  LOG(INFO) << "submit block: " << newblk.GetHash().ToString();
  const string blockHex = EncodeHexBlock(newblk);
  submitBlock(blockHex);  // using thread

  // save to DB, using thread
  saveBlockToDB(foundBlock, blkHeader,
                newblk.vtx[0].GetValueOut(),  // coinbase value
                blockHex.length()/2);
}

void BlockMaker::saveBlockToDB(const FoundBlock &foundBlock,
                               const CBlockHeader &header,
                               const uint64_t coinbaseValue,
                               const int32_t blksize) {
  boost::thread t(boost::bind(&BlockMaker::_saveBlockToDBThread, this,
                              foundBlock, header, coinbaseValue, blksize));
}

void BlockMaker::_saveBlockToDBThread(const FoundBlock &foundBlock,
                                      const CBlockHeader &header,
                                      const uint64_t coinbaseValue,
                                      const int32_t blksize) {
  const string nowStr = date("%F %T");
  string sql;
  sql = Strings::Format("INSERT INTO `found_blocks` "
                        " (`puid`, `worker_id`, `worker_full_name`, `job_id`"
                        "  ,`height`, `hash`, `rewards`, `size`, `prev_hash`"
                        "  ,`bits`, `version`, `created_at`)"
                        " VALUES (%d,%" PRId64",\"%s\", %" PRIu64",%d,\"%s\""
                        "  ,%" PRId64",%d,\"%s\",%u,%d,\"%s\"); ",
                        foundBlock.userId_, foundBlock.workerId_,
                        // filter again, just in case
                        filterWorkerName(foundBlock.workerFullName_).c_str(),
                        foundBlock.jobId_, foundBlock.height_,
                        header.GetHash().ToString().c_str(),
                        coinbaseValue, blksize,
                        header.hashPrevBlock.ToString().c_str(),
                        header.nBits, header.nVersion, nowStr.c_str());

  // try connect to DB
  MySQLConnection db(poolDB_);
  for (size_t i = 0; i < 3; i++) {
    if (db.ping())
      break;
    else
      sleep(3);
  }

  if (db.execute(sql) == false) {
    LOG(ERROR) << "insert found block failure: " << sql;
  }
}

bool BlockMaker::checkBitcoinds() {
  const string request = "{\"jsonrpc\":\"1.0\",\"id\":\"1\",\"method\":\"getinfo\",\"params\":[]}";

  if (bitcoindRpcUri_.size() == 0)
    return false;

  for (const auto &itr : bitcoindRpcUri_) {
    string response;
    bool res = bitcoindRpcCall(itr.first.c_str(), itr.second.c_str(),
                               request.c_str(), response);
    if (res == false) {
      return false;
    }
    LOG(INFO) << "response: " << response;
    JsonNode r;
    if (!JsonNode::parse(response.c_str(), response.c_str() + response.length(), r)) {
      LOG(ERROR) << "json parse failure: " << response;
      return false;
    }
    JsonNode result = r["result"];
    if (result.type() == Utilities::JS::type::Null ||
        result["connections"].int32() == 0) {
      LOG(ERROR) << "bitcoind is NOT works fine, getinfo: " << response;
      return false;
    }
  }

  return true;
}

void BlockMaker::submitBlock(const string &blockHex) {
  for (const auto &itr : bitcoindRpcUri_) {
    // use thread to submit
    boost::thread t(boost::bind(&BlockMaker::_submitBlockThread, this,
                                itr.first, itr.second, blockHex));
  }
}

void BlockMaker::_submitBlockThread(const string &rpcAddress,
                                    const string &rpcUserpass,
                                    const string &blockHex) {
  string request = "{\"jsonrpc\":\"1.0\",\"id\":\"1\",\"method\":\"submitblock\",\"params\":[\"";
  request += blockHex + "\"]}";

  LOG(INFO) << "submit block to: " << rpcAddress;
  // try N times
  for (size_t i = 0; i < 3; i++) {
    string response;
    bool res = bitcoindRpcCall(rpcAddress.c_str(), rpcUserpass.c_str(),
                               request.c_str(), response);

    // success
    if (res == true) {
      LOG(INFO) << "rpc call success, submit block response: " << response;
      break;
    }

    // failure
    LOG(ERROR) << "rpc call fail: " << response;
  }
}

void BlockMaker::consumeStratumJob(rd_kafka_message_t *rkmessage) {
  // check error
  if (rkmessage->err) {
    if (rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
      // Reached the end of the topic+partition queue on the broker.
      // Not really an error.
      //      LOG(INFO) << "consumer reached end of " << rd_kafka_topic_name(rkmessage->rkt)
      //      << "[" << rkmessage->partition << "] "
      //      << " message queue at offset " << rkmessage->offset;
      // acturlly
      return;
    }

    LOG(ERROR) << "consume error for topic " << rd_kafka_topic_name(rkmessage->rkt)
    << "[" << rkmessage->partition << "] offset " << rkmessage->offset
    << ": " << rd_kafka_message_errstr(rkmessage);

    if (rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION ||
        rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC) {
      LOG(FATAL) << "consume fatal";
      stop();
    }
    return;
  }

  LOG(INFO) << "received StratumJob message, len: " << rkmessage->len;

  StratumJob *sjob = new StratumJob();
  bool res = sjob->unserializeFromJson((const char *)rkmessage->payload,
                                       rkmessage->len);
  if (res == false) {
    LOG(ERROR) << "unserialize stratum job fail";
    delete sjob;
    return;
  }

  const uint256 gbtHash(sjob->gbtHash_);
  {
    ScopeLock sl(jobIdMapLock_);
    jobId2GbtHash_[sjob->jobId_] = gbtHash;

    // Maps (and sets) are sorted, so the first element is the smallest,
    // and the last element is the largest.
    while (jobId2GbtHash_.size() > kMaxStratumJobNum_) {
      jobId2GbtHash_.erase(jobId2GbtHash_.begin());
    }
  }

  LOG(INFO) << "StratumJob, jobId: " << sjob->jobId_ << ", gbtHash: " << gbtHash.ToString();
  delete sjob;
}

void BlockMaker::runThreadConsumeRawGbt() {
  const int32_t timeoutMs = 1000;

  while (running_) {
    rd_kafka_message_t *rkmessage;
    rkmessage = kafkaConsumerRawGbt_.consumer(timeoutMs);
    if (rkmessage == nullptr) /* timeout */
      continue;

    consumeRawGbt(rkmessage);

    /* Return message to rdkafka */
    rd_kafka_message_destroy(rkmessage);
  }
}

void BlockMaker::runThreadConsumeStratumJob() {
  const int32_t timeoutMs = 1000;

  while (running_) {
    rd_kafka_message_t *rkmessage;
    rkmessage = kafkaConsumerStratumJob_.consumer(timeoutMs);
    if (rkmessage == nullptr) /* timeout */
      continue;

    consumeStratumJob(rkmessage);

    /* Return message to rdkafka */
    rd_kafka_message_destroy(rkmessage);
  }
}

void BlockMaker::runThreadConsumeSovledShare() {
  const int32_t timeoutMs = 1000;

  while (running_) {
    rd_kafka_message_t *rkmessage;
    rkmessage = kafkaConsumerSovledShare_.consumer(timeoutMs);
    if (rkmessage == nullptr) /* timeout */
      continue;

    consumeSovledShare(rkmessage);

    /* Return message to rdkafka */
    rd_kafka_message_destroy(rkmessage);
  }
}

void BlockMaker::run() {
  // setup threads
  threadConsumeRawGbt_      = thread(&BlockMaker::runThreadConsumeRawGbt,     this);
  threadConsumeStratumJob_  = thread(&BlockMaker::runThreadConsumeStratumJob, this);
  sleep(3);

  runThreadConsumeSovledShare();
}

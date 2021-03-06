#pragma once

#include <stdint.h>
#include <cstdlib>
#include <string>
#include <sstream>
#include <memory>
#include <option.h>
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/options.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/transaction_log.h>
#include "string_utils.h"
#include "logger.h"

class UInt64AddOperator : public rocksdb::AssociativeMergeOperator {
public:
    virtual bool Merge(const rocksdb::Slice& key, const rocksdb::Slice* existing_value, const rocksdb::Slice& value,
                       std::string* new_value, rocksdb::Logger* logger) const override {
        uint64_t existing = 0;
        if (existing_value) {
            existing = StringUtils::deserialize_uint32_t(existing_value->ToString());
        }
        *new_value = StringUtils::serialize_uint32_t(existing + StringUtils::deserialize_uint32_t(value.ToString()));
        return true;
    }

    virtual const char* Name() const override {
        return "UInt64AddOperator";
    }
};

enum StoreStatus {
    FOUND,
    NOT_FOUND,
    ERROR
};

/*
 *  Abstraction for underlying KV store (RocksDB)
 */
class Store {
private:

    const std::string state_dir_path;
    rocksdb::DB *db;
    rocksdb::Options options;
    rocksdb::WriteOptions write_options;

public:

    Store() = delete;

    Store(const std::string & state_dir_path,
          const size_t wal_ttl_secs = 24*60*60,
          const size_t wal_size_mb = 1024, bool disable_wal = true): state_dir_path(state_dir_path) {
        // Optimize RocksDB
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();
        // create the DB if it's not already present
        options.create_if_missing = true;
        options.write_buffer_size = 4*1048576;
        options.max_write_buffer_number = 2;
        options.merge_operator.reset(new UInt64AddOperator);
        options.compression = rocksdb::CompressionType::kSnappyCompression;

        // these need to be high for replication scenarios
        options.WAL_ttl_seconds = wal_ttl_secs;
        options.WAL_size_limit_MB = wal_size_mb;

        // Disable WAL for master writes (Raft's WAL is used)
        // The replica uses native WAL, though.
        write_options.disableWAL = disable_wal;

        // open DB
        init_db();
    }

    ~Store() {
        close();
    }

    rocksdb::Status init_db() {
        rocksdb::Status s = rocksdb::DB::Open(options, state_dir_path, &db);
        if(!s.ok()) {
            LOG(ERROR) << "Error while initializing store: " << s.ToString();
            if(s.code() == rocksdb::Status::Code::kIOError) {
                LOG(ERROR) << "It seems like the data directory " << state_dir_path << " is already being used by "
                           << "another Typesense server. ";
                LOG(ERROR) << "If you are SURE that this is not the case, delete the LOCK file "
                           << "in the data db directory and try again.";
            }
        }

        assert(s.ok());
        return s;
    }

    bool insert(const std::string& key, const std::string& value) {
        rocksdb::Status status = db->Put(write_options, key, value);
        return status.ok();
    }

    bool batch_write(rocksdb::WriteBatch& batch) {
        rocksdb::Status status = db->Write(write_options, &batch);
        return status.ok();
    }

    bool contains(const std::string& key) const {
        std::string value;
        rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);
        return status.ok() && !status.IsNotFound();
    }

    StoreStatus get(const std::string& key, std::string& value) const {
        rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);

        if(status.ok()) {
            return StoreStatus::FOUND;
        }

        if(status.IsNotFound()) {
            return StoreStatus::NOT_FOUND;
        }

        LOG(ERROR) << "Error while fetching the key: " << key << " - status is: " << status.ToString();
        return StoreStatus::ERROR;
    }

    bool remove(const std::string& key) {
        rocksdb::Status status = db->Delete(write_options, key);
        return status.ok();
    }

    rocksdb::Iterator* scan(const std::string & prefix) {
        rocksdb::Iterator *iter = db->NewIterator(rocksdb::ReadOptions());
        iter->Seek(prefix);
        return iter;
    }

    rocksdb::Iterator* get_iterator() {
        rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
        return it;
    };

    void scan_fill(const std::string & prefix, std::vector<std::string> & values) {
        rocksdb::Iterator *iter = db->NewIterator(rocksdb::ReadOptions());
        for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix); iter->Next()) {
            values.push_back(iter->value().ToString());
        }

        delete iter;
    }

    void increment(const std::string & key, uint32_t value) {
        db->Merge(write_options, key, StringUtils::serialize_uint32_t(value));
    }

    uint64_t get_latest_seq_number() const {
        return db->GetLatestSequenceNumber();
    }

    Option<std::vector<std::string>*> get_updates_since(const uint64_t seq_number_org, const uint64_t max_updates) const {
        const uint64_t local_latest_seq_num = db->GetLatestSequenceNumber();

        // Since GetUpdatesSince(0) == GetUpdatesSince(1)
        const uint64_t seq_number = (seq_number_org == 0) ? 1 : seq_number_org;

        if(seq_number == local_latest_seq_num+1) {
            // replica has caught up, send an empty list as result
            std::vector<std::string>* updates = new std::vector<std::string>();
            return Option<std::vector<std::string>*>(updates);
        }

        rocksdb::unique_ptr<rocksdb::TransactionLogIterator> iter;
        rocksdb::Status status = db->GetUpdatesSince(seq_number, &iter);

        if(!status.ok()) {
            LOG(ERROR) << "Error while fetching updates for replication: " << status.ToString();

            std::ostringstream error;
            error << "Unable to fetch updates. " << "Master's latest sequence number is " << local_latest_seq_num
                  << " but requested sequence number is " << seq_number;
            LOG(ERROR) << error.str();

            return Option<std::vector<std::string>*>(400, error.str());
        }

        if(!iter->Valid()) {
            std::ostringstream error;
            error << "Invalid iterator. Master's latest sequence number is " << local_latest_seq_num << " but "
                  << "updates are requested from sequence number " << seq_number << ". "
                  << "The master's WAL entries might have expired (they are kept only for 24 hours).";
            LOG(ERROR) << error.str();
            return Option<std::vector<std::string>*>(400, error.str());
        }

        uint64_t num_updates = 0;
        std::vector<std::string>* updates = new std::vector<std::string>();

        bool first_iteration = true;
        
        while(iter->Valid() && num_updates < max_updates) {
            const rocksdb::BatchResult & batch = iter->GetBatch();
            if(first_iteration) {
                first_iteration = false;
                if(batch.sequence != seq_number) {
                    std::ostringstream error;
                    error << "Invalid iterator. Requested sequence number is " << seq_number << " but "
                          << "updates are available only from sequence number " << batch.sequence << ". "
                          << "The master's WAL entries might have expired (they are kept only for 24 hours).";
                    LOG(ERROR) << error.str();
                    return Option<std::vector<std::string>*>(400, error.str());
                }
            }

            const std::string & write_batch_serialized = batch.writeBatchPtr->Data();
            updates->push_back(write_batch_serialized);
            num_updates += 1;
            iter->Next();
        }

        return Option<std::vector<std::string>*>(updates);
    }

    void close() {
        delete db;
        db = nullptr;
    }

    void flush() {
        rocksdb::FlushOptions options;
        db->Flush(options);
    }

    // Only for internal tests
    rocksdb::DB* _get_db_unsafe() const {
        return db;
    }

    const std::string& get_state_dir_path() const {
        return state_dir_path;
    }

    const rocksdb::Options &get_db_options() const {
        return options;
    }

    void print_memory_usage() {
        std::string index_usage;
        db->GetProperty("rocksdb.estimate-table-readers-mem", &index_usage);
        LOG(INFO) << "rocksdb.estimate-table-readers-mem: " << index_usage;

        std::string memtable_usage;
        db->GetProperty("rocksdb.cur-size-all-mem-tables", &memtable_usage);
        LOG(INFO) << "rocksdb.cur-size-all-mem-tables: " << memtable_usage;
    }
};
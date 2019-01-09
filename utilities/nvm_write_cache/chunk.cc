//
// Created by 张艺文 on 2018/11/5.
//
#include "util/coding.h"
#include "include/rocksdb/filter_policy.h"

#include "chunk.h"
#include "fixed_range_chunk_based_nvm_write_cache.h"
#include "debug.h"


namespace rocksdb {
ArrayBasedChunk::ArrayBasedChunk() {
    raw_data_.clear();
    entry_offset_.clear();
    now_offset_ = 0;
}


/* ------------
 * | key_size | // 64bit
 * ------------
 * |    key   |
 * ------------
 * |value_size| // 64bit
 * ------------
 * |   value  |
 * ------------
 * */
void ArrayBasedChunk::Insert(const Slice &key, const Slice &value) {
    //unsigned int total_size = key.size_ + value.size_ + 8 + 8;
    unsigned int total_size = key.size_ + 1 + 8 + 8;
    PutFixed64(&raw_data_, key.size_);
    raw_data_.append(key.data_, key.size_);

    // put fake data
    string fakedata("0");
    PutFixed64(&raw_data_, fakedata.size());
    raw_data_.append(fakedata);
    /*PutFixed64(&raw_data_, value.size_);
    raw_data_.append(value.data_, value.size_);*/


    entry_offset_.push_back(now_offset_);
    now_offset_ += total_size;
    num_entries++;
}

/*
 * |      chunk1     |
 * |      chunk2     |
 * |       ...       |
 * | chunk offset 1  |
 * | chunk offset 2  |
 * |       ...       |
 * |    chunk num    |
 * */
std::string *ArrayBasedChunk::Finish() {
    for (auto offset : entry_offset_) {
        PutFixed64(&raw_data_, offset);
    }
    // 写num_pairs
    //DBG_PRINT("has kv item [%lu]", entry_offset_.size());
    PutFixed64(&raw_data_, entry_offset_.size());
    // new string
    auto *result = new std::string(raw_data_);
    return result;
}

BuildingChunk::BuildingChunk(const FilterPolicy *filter_policy, const std::string &prefix)
        : prefix_(prefix),
          chunk_(new ArrayBasedChunk()),
          filter_policy_(filter_policy) {
    if (filter_policy_ == nullptr) {
        printf("empty filter policy\n");
    }
    num_entries_ = 0;

}

BuildingChunk::~BuildingChunk() {
    delete chunk_;
    for (auto tmp : keys_) {
        delete[] tmp.data_;
    }
    keys_.clear();
}

uint64_t BuildingChunk::NumEntries() {
    return num_entries_;
}

void BuildingChunk::Insert(const rocksdb::Slice &key, const rocksdb::Slice &value) {
    chunk_->Insert(key, value);
    // delete in Deconstructor
    char *key_rep = new char[key.size_];
    memcpy(key_rep, key.data_, key.size_);
    // InternalKey in keys
    keys_.emplace_back(key_rep, key.size());
    user_keys_.emplace_back(key_rep, key.size() - 8);
    num_entries_++;
}


std::string *BuildingChunk::Finish(string& bloom_data, rocksdb::Slice &cur_start, rocksdb::Slice &cur_end) {
    std::string *chunk_data;
    // get kv data
    // delete in FlushJob
    chunk_data = chunk_->Finish();
    // get bloom data
    // Build bloom filter by internal_key
    filter_policy_->CreateFilter(&user_keys_[0], user_keys_.size(), &bloom_data);
    // get key range
    cur_start = keys_[0];
    cur_end = keys_[keys_.size() - 1];
    //DBG_PRINT("Get Kv Item num[%lu] recorded[%lu]", DecodeFixed64(chunk_data->c_str() + chunk_data->size() - 8), num_entries_);
    //delete chunk_bloom_data;
    return chunk_data;
}
}

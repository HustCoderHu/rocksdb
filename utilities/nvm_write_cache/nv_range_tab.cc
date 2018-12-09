//
// Created by 张艺文 on 2018/11/27.
//
#include <city.h>
#include "nv_range_tab.h"


namespace rocksdb{
NvRangeTab::NvRangeTab(pool_base &pop, char* raw, uint64_t off, const string &prefix, uint64_t range_size)
: raw_(raw){
    transaction::run(pop, [&] {
        offset_ = off;
        prefix_ = make_persistent<char[]>(prefix.size());
        memcpy(prefix_.get(), prefix.c_str(), prefix.size());

        range_buf_len_ = 200;
        key_range_ = make_persistent<char[]>(200);
        pair_buf_ = nullptr;

        prefix_len_ = prefix.size();
        chunk_num_ = 0;
        seq_num_ = 0;
        buf_size_ = range_size;
        data_len_ = 0;
        hash_ = CityHash64WithSeed(prefix_.get(), prefix_len_, 16);
    });
}


bool NvRangeTab::equals(const string &prefix) {
    string cur_prefix(prefix_.get(), prefix_len_);
    return cur_prefix == prefix;
}

bool NvRangeTab::equals(rocksdb::p_buf &prefix, size_t len) {
    return equals(string(prefix.get(), len));
}

bool NvRangeTab::equals(rocksdb::NvRangeTab &b) {
    return equals(b.prefix_, b.prefix_len_);
}
}
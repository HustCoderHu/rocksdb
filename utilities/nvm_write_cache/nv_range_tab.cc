//
// Created by 张艺文 on 2018/11/27.
//
#include <city.h>
#include "nv_range_tab.h"


namespace rocksdb{
NvRangeTab::NvRangeTab(pool_base &pop, const string &prefix, uint64_t range_size) {
    transaction::run(pop, [&] {
        prefix_ = make_persistent<char[]>(prefix.size());
        memcpy(prefix_.get(), prefix.c_str(), prefix.size());

        rangebufLen = 200;
        key_range_ = make_persistent<char[]>(200);
        extra_buf = nullptr;
        buf = make_persistent<char[]>(range_size);

        prefixLen = prefix.size();
        chunk_num_ = 0;
        seq_num_ = 0;
        bufSize = range_size;
        dataLen = 0;
        hash_ = CityHash64WithSeed(prefix_.get(), prefixLen, 16);
    });
}


bool NvRangeTab::equals(const string &prefix) {
    string cur_prefix(prefix_.get(), prefixLen);
    return cur_prefix == prefix;
}

bool NvRangeTab::equals(rocksdb::p_buf &prefix, size_t len) {
    return equals(string(prefix.get(), len));
}

bool NvRangeTab::equals(rocksdb::NvRangeTab &b) {
    return equals(b.prefix_, b.prefixLen);
}
}
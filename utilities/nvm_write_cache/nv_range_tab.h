
#pragma once

#include <string>

#include "libpmemobj++/p.hpp"
#include "libpmemobj++/persistent_ptr.hpp"
#include "libpmemobj++/pool.hpp"
#include "libpmemobj++/transaction.hpp"
#include "libpmemobj++/make_persistent.hpp"
#include "libpmemobj++/make_persistent_array.hpp"

#include "rocksdb/slice.h"

namespace rocksdb{
using std::string;
using pmem::obj::p;
using pmem::obj::persistent_ptr;
using pmem::obj::pool;
using pmem::obj::pool_base;
using pmem::obj::make_persistent;
using pmem::obj::transaction;

using p_buf = persistent_ptr<char[]>;


struct NvRangeTab {
public:
    NvRangeTab(pool_base &pop, p_buf buf_, uint64_t off, const string &prefix, uint64_t range_size);

    uint64_t hashCode() {
        return hash_;
    }

    //char *GetRawBuf() { return raw_; }

    /*void SetRaw(char* raw){
        raw_ = raw;
    }*/

    // 通过比价前缀，比较两个NvRangeTab是否相等
    bool equals(const string &prefix);

    bool equals(p_buf &prefix, size_t len);

    bool equals(NvRangeTab &b);

    //char* raw_;
    p_buf buf_;
    p<int> offset_;
    p<uint64_t> hash_;
    p<bool> writting_;

    p<size_t> prefix_len_; // string prefix_ tail 0 not included
    p_buf prefix_; // prefix

    p<size_t> range_buf_len_;
    p_buf key_range_; //key range

    p<size_t> chunk_num_;
    p<uint64_t> seq_num_;

    p<size_t> buf_size_; // capacity
    p<size_t> data_len_; // exact data len

    persistent_ptr<NvRangeTab> pair_buf_;

};
}

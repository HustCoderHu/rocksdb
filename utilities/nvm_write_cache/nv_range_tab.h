
#pragma once

#include <string>

#include "libpmemobj++/p.hpp"
#include "libpmemobj++/persistent_ptr.hpp"
#include "libpmemobj++/pool.hpp"
#include "libpmemobj++/transaction.hpp"
#include "libpmemobj++/make_persistent.hpp"
#include "libpmemobj++/make_persistent_array.hpp"

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
    NvRangeTab(pool_base &pop, const string &prefix, uint64_t range_size);

    uint64_t hashCode() {
        return hash_;
    }

    char *GetRawBuf() { return buf.get(); }

    // 通过比价前缀，比较两个NvRangeTab是否相等
    bool equals(const string &prefix);

    bool equals(p_buf &prefix, size_t len);

    bool equals(NvRangeTab &b);

    p<uint64_t> hash_;

    p<size_t> prefixLen; // string prefix_ tail 0 not included
    p_buf prefix_; // prefix

    p<size_t> rangebufLen;
    p_buf key_range_; //key range

    p<size_t> chunk_num_;
    p<uint64_t> seq_num_;

    p<size_t> bufSize; // capacity
    p<size_t> dataLen; // exact data len

    persistent_ptr<NvRangeTab> extra_buf;
    p_buf buf; // buf data

};
}

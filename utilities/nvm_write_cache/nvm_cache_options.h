//
// Created by 张艺文 on 2018/11/2.
//

#pragma once

#include <string>
#include "include/rocksdb/options.h"
#include "utilities/nvm_write_cache/prefix_extractor.h"
#include "utilities/nvm_write_cache/nvm_write_cache.h"
#include "debug.h"

namespace rocksdb {

class NVMWriteCache;

//class PrefixExtractor;
class FixedRangeChunkBasedNVMWriteCache;

struct PMemInfo {
    std::string pmem_path_;
    uint64_t pmem_size_;
};

enum NVMCacheType {
    kRangeFixedChunk,
    kRangeDynamic,
    kTreeBased,
};

enum DrainStrategy {
    kCompaction,
    kNoDrain,
};

struct FixedRangeBasedOptions {

    const uint16_t chunk_bloom_bits_ = 16;
    const uint16_t prefix_bits_ = 3;
    unique_ptr<PrefixExtractor> prefix_extractor_ = nullptr;
    const FilterPolicy *filter_policy_ = nullptr;
    //const uint64_t range_num_threshold_ = 0;
    const size_t range_size_ = 128;
    const int range_num_ = 100;

    FixedRangeBasedOptions(
            uint16_t chunk_bloom_bits,
            uint16_t prefix_bits,
            //uint64_t range_num_threashold,
            uint64_t range_size,
            int range_num,
            int key_num)
            :
            chunk_bloom_bits_(chunk_bloom_bits),
            prefix_bits_(prefix_bits),
            prefix_extractor_(ArbitrarilyExtractor::NewArbitrarilyExtractor(key_num / range_num)),
            filter_policy_(NewBloomFilterPolicy(chunk_bloom_bits, false)),
            //range_num_threshold_(range_num_threashold),
            range_size_(range_size),
            range_num_(range_num){
        DBG_PRINT("num per range [%lu]", key_num / range_num);

    }

    ~FixedRangeBasedOptions(){
        delete filter_policy_;
    }

};

struct DynamicRangeBasedOptions {

};

struct TreeBasedOptions {

};

using std::string;
struct NVMCacheSetup{
    bool use_nvm_cache_ = false;

    bool reset_cache_ = false;

    string pmem_path;

    size_t prefix_bytes = 0;

    int bloom_bits = 16;

    int range_num = 100;

    uint64_t range_size = 1<<27;

    int key_num = 1000000;

    NVMCacheType cache_type_ = kRangeFixedChunk;

    NVMCacheSetup& operator=(const NVMCacheSetup& setup) =default;
    NVMCacheSetup() =default;
};


struct NVMCacheOptions {
    NVMCacheOptions();

    NVMCacheOptions(const shared_ptr<NVMCacheSetup> setup);

    ~NVMCacheOptions(){
        delete nvm_write_cache_;
    }

    bool use_nvm_write_cache_;
    bool reset_nvm_write_cache;
    PMemInfo pmem_info_;
    NVMCacheType nvm_cache_type_;
    DrainStrategy drain_strategy_;
    NVMWriteCache* nvm_write_cache_;

    static FixedRangeChunkBasedNVMWriteCache *NewFixedRangeChunkBasedCache(const NVMCacheOptions *nvm_cache_options,
                                                                           FixedRangeBasedOptions *foptions,
                                                                           const InternalKeyComparator* icmp);
};


} //end rocksdb


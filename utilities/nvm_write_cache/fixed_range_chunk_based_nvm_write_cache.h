//
// Created by zzyyy on 2018/11/2.
// 胡泽鑫负责
//

#pragma once

#include "utilities/nvm_write_cache/nvm_write_cache.h"
#include "utilities/nvm_write_cache/nvm_cache_options.h"

namespace rocksdb{

    struct FixedRangeChunkBasedCacheStats{

        uint64_t  used_bits_;

        std::unordered_map<std::string, uint64_t> range_list_;

        std::vector<std::string*> chunk_bloom_data_;

    };

    class RangeBasedChunk{
    public:
        explicit RangeBasedChunk();

        ~RangeBasedChunk();

        void Insert(const Slice& key, const Slice& value);

        void TransferToPersistent(const Slice& extra_data);

        void TranserToVolatile();

        Iterator* NewIterator();

        void ParseRawData();
    };

    class BuildingChunk{
    public:
        explicit BuildingChunk();
        ~BuildingChunk();

        void Insert(const Slice& key, const Slice& value);

        const char* Finish();

        uint64_t NumEntries();
    };

    class FixedRangeChunkBasedNVMWriteCache: public NVMWriteCache{

    public:

        explicit FixedRangeChunkBasedNVMWriteCache(FixedRangeBasedOptions* cache_options_);

        ~FixedRangeChunkBasedNVMWriteCache();

        // insert data to cache
        Status Insert(const Slice& cached_data, void* insert_mark);

        // get data from cache
        Status Get(const Slice& key, std::string* value);

        // get iterator of the total cache
        Iterator* NewIterator();

        //get iterator of data that will be drained
        Iterator* GetDraineddata();

        // add a range with a new prefix to range mem
        uint64_t NewRange(const std::string& prefix);

        // get internal options of this cache
        const FixedRangeBasedOptions* internal_options(){return internal_options_;}

        // get stats of this cache
        FixedRangeChunkBasedCacheStats* stats(){return cache_stats_;}


    private:
        const FixedRangeBasedOptions* internal_options_;
        FixedRangeChunkBasedCacheStats* cache_stats_;


    };
}
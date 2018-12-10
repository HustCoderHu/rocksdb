//
// Created by 张艺文 on 2018/11/2.
//

#include "nvm_cache_options.h"
#include "fixed_range_chunk_based_nvm_write_cache.h"


namespace rocksdb {
NVMCacheOptions::NVMCacheOptions()
        : use_nvm_write_cache_(false) {

}

NVMCacheOptions::NVMCacheOptions(const shared_ptr<NVMCacheSetup> setup)
        :   use_nvm_write_cache_(setup->use_nvm_cache_),
            reset_nvm_write_cache(setup->reset_cache_),
            nvm_cache_type_(setup->cache_type_),
            drain_strategy_(kCompaction),
            nvm_write_cache_(nullptr)
{
    pmem_info_.pmem_path_ = setup->pmem_path;
    pmem_info_.pmem_size_ = 400ul * 1024 * 1024 * 1024;
}

FixedRangeChunkBasedNVMWriteCache *NVMCacheOptions::NewFixedRangeChunkBasedCache(const NVMCacheOptions *nvm_cache_options,
                                                                                 FixedRangeBasedOptions *foptions,
                                                                                 const InternalKeyComparator* icmp) {
    return new FixedRangeChunkBasedNVMWriteCache(foptions,
                                                 icmp,
                                                 nvm_cache_options->pmem_info_.pmem_path_,
                                                 nvm_cache_options->pmem_info_.pmem_size_,
                                                 nvm_cache_options->reset_nvm_write_cache);
}


}//end rocksdb

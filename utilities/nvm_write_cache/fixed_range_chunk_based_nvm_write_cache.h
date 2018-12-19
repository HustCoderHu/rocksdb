#pragma once

#include <queue>
#include <mutex>
#include <unordered_map>

#include "rocksdb/iterator.h"
#include "utilities/nvm_write_cache/nvm_write_cache.h"
#include "utilities/nvm_write_cache/nvm_cache_options.h"
#include "table/internal_iterator.h"

#include "libpmemobj++/p.hpp"
#include "libpmemobj++/persistent_ptr.hpp"
#include "libpmemobj++/pool.hpp"
#include "libpmemobj++/transaction.hpp"
#include "libpmemobj++/make_persistent.hpp"
#include "libpmemobj++/make_persistent_array.hpp"

#include "fixed_range_tab.h"
#include "persistent_allocator.h"
#include "pmem_hash_map.h"
#include "persistent_bitmap.h"
#include "common.h"

#include "debug.h"

using std::string;
using std::mutex;
using std::unordered_map;
using namespace pmem::obj;
using p_range::pmem_hash_map;
//using p_range::p_node_t;

namespace rocksdb {

using std::function;



struct CompactionItem {
    FixedRangeTab *pending_compated_range_;
    Usage range_usage;
    PersistentAllocator* allocator_;

    CompactionItem() =default;

    explicit CompactionItem(FixedRangeTab *range, PersistentAllocator* allocator)
            :   pending_compated_range_(range),
                range_usage(range->RangeUsage(kForCompaction)),
                allocator_(allocator){
    }

    CompactionItem(const CompactionItem &item) =default;
};

struct ChunkMeta {
    string prefix;
    Slice cur_start;
    Slice cur_end;
};



using p_buf = persistent_ptr<char[]>;
using std::atomic;

class FixedRangeChunkBasedNVMWriteCache : public NVMWriteCache {
public:
    explicit FixedRangeChunkBasedNVMWriteCache(
            const FixedRangeBasedOptions *ioptions,
            const InternalKeyComparator* icmp,
            const string &file, uint64_t pmem_size,
            bool reset = false);

    ~FixedRangeChunkBasedNVMWriteCache() override;

    // insert data to cache
    // insert_mark is (uint64_t)range_id
//  Status Insert(const Slice& cached_data, void* insert_mark) override;

    // get data from cache
    bool Get(const InternalKeyComparator &internal_comparator, Status *s, const LookupKey &lkey, std::string *value) override;

    void AppendToRange(const InternalKeyComparator &icmp, const string& bloom_data, const Slice &chunk_data,
                       const ChunkMeta &meta);

    // get iterator of the total cache
    InternalIterator *NewIterator(const InternalKeyComparator *icmp, Arena *arena) override;

    // return there is need for compaction or not
    /*bool NeedCompaction() override { return !vinfo_->range_queue_.empty(); }*/
    bool NeedCompaction() override { return vinfo_->compaction_requested_; }

    //get iterator of data that will be drained
    // get 之后释放没有 ?
    void GetCompactionData(CompactionItem *compaction);

    // get internal options of this cache
    const FixedRangeBasedOptions *internal_options() { return vinfo_->internal_options_; }

    void MaybeNeedCompaction();

    void RangeExistsOrCreat(const std::string &prefix);

	FixedRangeTab* GetRangeTab(const std::string &prefix);

	void RollbackCompaction(FixedRangeTab* range);

	//TODO: DeleteCache;

private:

    persistent_ptr<NvRangeTab> NewContent(const string& prefix, size_t bufSize);
    FixedRangeTab *NewRange(const std::string &prefix);

    void RebuildFromPersistentNode();

    void Deallocate(int offset){
        pinfo_->allocator_->Free(offset);
    }

    struct PersistentInfo {
        p<bool> inited_;
        p<uint64_t> allocated_bits_;
        persistent_ptr<pmem_hash_map<NvRangeTab> > range_map_;
        // TODO: allocator分配的空间没法收回
        persistent_ptr<PersistentAllocator> allocator_;
    };

    pool<PersistentInfo> pop_;
    persistent_ptr<PersistentInfo> pinfo_;

    struct VolatileInfo {
        const FixedRangeBasedOptions *internal_options_;
        const InternalKeyComparator* icmp_;
        unordered_map<string, FixedRangeTab*> prefix2range;
        std::vector<FixedRangeTab*> range_queue_;
        InstrumentedMutex queue_lock_;
        atomic<uint64_t> total_size_;
        atomic_bool compaction_requested_;
        bool queue_sorted_;


        explicit VolatileInfo(const FixedRangeBasedOptions *ioptions, const InternalKeyComparator* icmp)
                :   internal_options_(ioptions),
                    icmp_(icmp){
            total_size_ = 0;
            queue_sorted_ = false;
            compaction_requested_ = false;
        }
    };

    VolatileInfo *vinfo_;
};

} // namespace rocksdb
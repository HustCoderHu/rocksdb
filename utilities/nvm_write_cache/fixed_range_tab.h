#pragma once
#include <list>
#include <memory>


#include "monitoring/instrumented_mutex.h"

//#include "libpmemobj.h"
#include "libpmemobj++/p.hpp"
#include "libpmemobj++/persistent_ptr.hpp"
#include "libpmemobj++/pool.hpp"
#include "libpmemobj++/transaction.hpp"
#include "libpmemobj++/make_persistent.hpp"
#include "libpmemobj++/make_persistent_array.hpp"

#include "nv_range_tab.h"
#include "persistent_chunk.h"
#include "pmem_hash_map.h"
#include "nvm_cache_options.h"
#include "chunkblk.h"

using namespace pmem::obj;
using namespace p_range;
using std::string;
using std::unique_ptr;

namespace rocksdb {

using pmem::obj::persistent_ptr;

using p_buf = persistent_ptr<char[]>;

class Usage {
public:
    uint64_t chunk_num;
    uint64_t range_size;
    Slice start_, end_;

    Usage() {}

    Usage(const Usage& u) {
        *this = u;
    }

    Usage& operator=(const Usage& u) {
        if (this != &u) {
            chunk_num = u.chunk_num;
            range_size = u.range_size;
            //start.DecodeFrom(Slice(*u.start.rep()));
            start_ = u.start_;
            end_ = u.end_;
        }
        return *this;
    }

    unique_ptr<InternalKey> start() {
        if(!start_.empty()){
            unique_ptr<InternalKey> ptr(new InternalKey());
            ptr->DecodeFrom(start_);
            return ptr;
        }else{
            return nullptr;
        }
    }

    unique_ptr<InternalKey> end() const{
        if(!end_.empty()){
            unique_ptr<InternalKey> ptr(new InternalKey());
            ptr->DecodeFrom(end_);
            return ptr;
        }else{
            return nullptr;
        }
    }



};


class FixedRangeTab {


public:
    //FixedRangeTab(pool_base &pop, FixedRangeBasedOptions *options);

    FixedRangeTab(pool_base &pop, const FixedRangeBasedOptions *options,
                  persistent_ptr<NvRangeTab> &nonVolatileTab);

    //FixedRangeTab(pool_base &pop, p_node pmap_node_, FixedRangeBasedOptions *options);

//  FixedRangeTab(pool_base& pop, p_node pmap_node_, FixedRangeBasedOptions *options);

    ~FixedRangeTab() = default;

public:
    //void reservePersistent();

    // 返回当前RangeMemtable中所有chunk的有序序列
    // 基于MergeIterator
    // 参考 DBImpl::NewInternalIterator
    InternalIterator *NewInternalIterator(const InternalKeyComparator *icmp, Arena *arena);

    Status Get(const InternalKeyComparator &internal_comparator, const LookupKey &lkey,
               std::string *value);

    persistent_ptr<NvRangeTab> getPersistentData() { return nonVolatileTab_; }

    // 将新的chunk数据添加到RangeMemtable
    Status Append(const InternalKeyComparator &icmp,
                  const string& bloom_data, const Slice &chunk_data,
                  const Slice &start, const Slice &end);

    // 返回当前range tab是否正在被compact
    bool IsCompactWorking() { return in_compaction_; }

    // 设置compaction状态
    void SetCompactionWorking(bool working) {
        if(working && pendding_clean_ == 0){
			// set为true的时候一定是pendding_clean为0的情况
            pendding_clean_ = blklist.size();
        }
        in_compaction_ = working;
    }

    // 返回当前range tab是否在compaction队列里面
    bool IsCompactPendding() { return pendding_compaction_; }

    // 设置compaction queue状态
    void SetCompactionPendding(bool pendding) {
        pendding_compaction_ = pendding;
    }

    bool IsExtraBufExists(){return nonVolatileTab_->extra_buf != nullptr;}

    // 设置extra buf，同时更新raw
    void SetExtraBuf(persistent_ptr<NvRangeTab> extra_buf);

    Usage RangeUsage();

    // 释放当前RangeMemtable的所有chunk以及占用的空间
    void Release();

    // 重置Stat数据以及bloom filter
    void CleanUp();

    uint64_t max_range_size() {
        return nonVolatileTab_->bufSize;
    }

    void lock(){
        //DBG_PRINT("tab lock[%d]", lock_count);
        tab_lock_.Lock();
        lock_count++;
        //DBG_PRINT("in tab lock[%d]", lock_count);
    }

    void unlock(){
        //DBG_PRINT("before tab unlock[%d]", lock_count);
        tab_lock_.Unlock();
        lock_count--;
        //DBG_PRINT("tab unlock[%d]", lock_count);
    }

    string prefix(){
        return string(nonVolatileTab_->prefix_.get(), nonVolatileTab_->prefixLen);
    }

//#ifdef TAB_DEBUG
    // 输出range信息
    void GetProperties();
//#endif

private:

    void RebuildBlkList();

    uint64_t max_chunk_num_to_flush() const {
        // TODO: set a max chunk num
        return 1024;
    }

    // 返回当前RangeMem的真实key range（stat里面记录）
    void GetRealRange(Slice &real_start, Slice &real_end);

    Status searchInChunk(PersistentChunkIterator *iter,
                         const InternalKeyComparator &icmp,
                         const Slice &key, std::string *value);

    Slice GetKVData(char *raw, uint64_t item_off);

    void CheckAndUpdateKeyRange(const InternalKeyComparator &icmp, const Slice &new_start, const Slice &new_end);

    void ConsistencyCheck();


    // persistent info
    //p_node pmap_node_;
    pool_base &pop_;
    persistent_ptr<NvRangeTab> nonVolatileTab_;


    // volatile info
    const FixedRangeBasedOptions *interal_options_;
    port::Mutex tab_lock_;
    int lock_count;
    vector<ChunkBlk> blklist;
    char *raw_;
    bool in_compaction_;
    bool pendding_compaction_;
    size_t pendding_clean_;


};

} // namespace rocksdb


#pragma once
#include <list>
#include <memory>
#include <wsman.h>


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

    Usage():chunk_num(0), range_size(0) {}

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

enum SwitchDirection{
    kToWBuffer,
    kToCBuffer,
};

class PersistentAllocator;
class FixedRangeTab {


public:
    //FixedRangeTab(pool_base &pop, FixedRangeBasedOptions *options);

    FixedRangeTab(pool_base &pop,
            const FixedRangeBasedOptions *options,
            const InternalKeyComparator* icmp,
            persistent_ptr<NvRangeTab> &wbuffer);

    //FixedRangeTab(pool_base &pop, p_node pmap_node_, FixedRangeBasedOptions *options);

//  FixedRangeTab(pool_base& pop, p_node pmap_node_, FixedRangeBasedOptions *options);

    ~FixedRangeTab() = default;

public:
    // 将新的chunk数据添加到RangeMemtable
    Status Append(const string& bloom_data, const Slice &chunk_data,
                  const Slice &start, const Slice &end);

    bool Get(Status *s, const LookupKey &lkey, std::string *value);

    // 返回当前RangeMemtable中所有chunk的有序序列
    // 基于MergeIterator
    // 参考 DBImpl::NewInternalIterator
    InternalIterator *NewInternalIterator(Arena *arena, bool for_compaction = false);

    //persistent_ptr<NvRangeTab> getPersistentData() { return w_buffer_; }

    // 返回当前range tab是否正在被compact
    bool IsCompactWorking() { return compaction_working_; }

    // 设置compaction状态
    void SetCompactionWorking(bool working) {
        compaction_working_ = working;
    }

    // 返回当前range tab是否在compaction队列里面
    bool IsCompactPendding() { return compaction_pendding_; }

    // 设置compaction queue状态
    void SetCompactionPendding(bool pendding) {
        compaction_pendding_ = pendding;
    }

    //bool IsExtraBufExists(){return nonVolatileTab_->pair_buf_ != nullptr;}

    // 设置extra buf，同时更新raw
    //void SetExtraBuf(persistent_ptr<NvRangeTab> extra_buf);

    Usage RangeUsage(bool for_compaction = false);

    // 释放当前RangeMemtable的所有chunk以及占用的空间
    void Release();

    // 重置Stat数据以及bloom filter
    //void CleanUp();

    void SwitchBuffer(SwitchDirection direction);

    bool EnoughFroWriting(uint64_t wsize){
        return wsize < (w_buffer_->buf_size_ - w_buffer_->data_len_);
    }

    bool HasCompactionBuf(){
        return c_buffer_ != nullptr;
    }

    uint64_t max_range_size() {
        return w_buffer_->buf_size_;
    }

    void lock(){
        //DBG_PRINT("tab lock[%d]", lock_count);
        tab_lock_.Lock();
        //DBG_PRINT("in tab lock[%d]", lock_count);
    }

    void unlock(){
        //DBG_PRINT("before tab unlock[%d]", lock_count);
        tab_lock_.Unlock();
        //DBG_PRINT("tab unlock[%d]", lock_count);
    }

    string prefix(){
        return string(w_buffer_->prefix_.get(), w_buffer_->prefix_len_);
    }

    // 输出range信息
    void GetProperties();

private:

    void RebuildBlkList();

    uint64_t max_chunk_num_to_flush() const {
        // TODO: set a max chunk num
        return 1024;
    }

    // 返回当前RangeMem的真实key range（stat里面记录）
    void GetRealRange(NvRangeTab* tab, Slice &real_start, Slice &real_end);

    Status searchInChunk(PersistentChunkIterator *iter,
                         const Slice &key, std::string *value);

    Slice GetKVData(char *raw, uint64_t item_off);

    void CheckAndUpdateKeyRange(const Slice &new_start, const Slice &new_end);

    void ConsistencyCheck();

    void CleanUp(NvRangeTab* tab);

    pool_base &pop_;
    persistent_ptr<NvRangeTab> w_buffer_;
    persistent_ptr<NvRangeTab> c_buffer_;
    vector<ChunkBlk> wblklist_;
    vector<ChunkBlk> cblklist_;
    char *raw_;

    // volatile info
    const FixedRangeBasedOptions *interal_options_;
    const InternalKeyComparator* icmp_;
    port::Mutex tab_lock_;

    bool compaction_working_;
    bool compaction_pendding_;
    //size_t pendding_clean_;


};

} // namespace rocksdb


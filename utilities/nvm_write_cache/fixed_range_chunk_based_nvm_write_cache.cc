#include "utilities/nvm_write_cache/skiplist/test_common.h"
#include "fixed_range_chunk_based_nvm_write_cache.h"

namespace rocksdb {

using std::string;

FixedRangeChunkBasedNVMWriteCache::FixedRangeChunkBasedNVMWriteCache(
        const FixedRangeBasedOptions *ioptions,
        const InternalKeyComparator *icmp,
        const string &file, uint64_t pmem_size,
        bool reset) {
    //bool justCreated = false;
    vinfo_ = new VolatileInfo(ioptions, icmp);
    if (file_exists(file.c_str()) != 0) {
        // creat pool
        DBG_PRINT("pmem size[%f]GB", pmem_size/(1073741824.0));
        pop_ = pmem::obj::pool<PersistentInfo>::create(file.c_str(), "FixedRangeChunkBasedNVMWriteCache", pmem_size,
                                                       CREATE_MODE_RW);
    } else {
        // open pool
        pop_ = pmem::obj::pool<PersistentInfo>::open(file.c_str(), "FixedRangeChunkBasedNVMWriteCache");
    }

    pinfo_ = pop_.root();
    if (!pinfo_->inited_) {
        // init cache
        uint64_t range_pool_size = pmem_size / 10;
        transaction::run(pop_, [&] {
            DBG_PRINT("alloc range map");
            pinfo_->range_map_ = make_persistent<pmem_hash_map<NvRangeTab>>(pop_, 0.75, 256);
            DBG_PRINT("alloc raw buf[%f]GB", range_pool_size/(1073741824.0));
            persistent_ptr<char[]> buf = make_persistent<char[]>(range_pool_size);
            DBG_PRINT("alloc bitmap[%lu]bits",range_pool_size / ioptions->range_size_);
            persistent_ptr<PersistentBitMap> bitmap = make_persistent<PersistentBitMap>(pop_,
                                                                                        range_pool_size /
                                                                                        ioptions->range_size_);
            pinfo_->allocator_ = make_persistent<PersistentAllocator>(pop_, buf, range_pool_size, ioptions->range_size_, bitmap);
            pinfo_->inited_ = true;
        });
    } else if (reset) {
        // reset cache
        transaction::run(pop_, [&] {
            delete_persistent<pmem_hash_map<NvRangeTab>>(pinfo_->range_map_);
            pinfo_->range_map_ = make_persistent<pmem_hash_map<NvRangeTab>>(pop_, 0.75, 256);
        });
        pinfo_->allocator_->Reset();
    } else {
        // rebuild cache
        RebuildFromPersistentNode();
    }

}

FixedRangeChunkBasedNVMWriteCache::~FixedRangeChunkBasedNVMWriteCache() {
    for (auto range: vinfo_->prefix2range) {
        // 释放FixedRangeTab的空间
        delete range.second;
    }
    vinfo_->prefix2range.clear();
    delete vinfo_->internal_options_;
    delete vinfo_;
    pop_.close();
}

bool FixedRangeChunkBasedNVMWriteCache::Get(const InternalKeyComparator &internal_comparator, Status *s,
                                            const LookupKey &lkey,
                                            std::string *value) {
    std::string prefix = (*vinfo_->internal_options_->prefix_extractor_)(lkey.user_key().data(),
                                                                         lkey.user_key().size());
    DBG_PRINT("prefix: [%s], size[%lu]", prefix.c_str(), prefix.size());
    auto found_tab = vinfo_->prefix2range.find(prefix);
    if (found_tab == vinfo_->prefix2range.end()) {
        // not found
        DBG_PRINT("NotFound prefix");
        return false;
    } else {
        // found
        DBG_PRINT("Found prefix");
        FixedRangeTab *tab = found_tab->second;
        return tab->Get(s, lkey, value);
    }
}

void FixedRangeChunkBasedNVMWriteCache::AppendToRange(const rocksdb::InternalKeyComparator &icmp,
                                                      const string &bloom_data, const rocksdb::Slice &chunk_data,
                                                      const rocksdb::ChunkMeta &meta) {
    /*
     * 1. 获取prefix
     * 2. 调用tangetab的append
     * */
    FixedRangeTab *now_range = nullptr;
    auto tab_found = vinfo_->prefix2range.find(meta.prefix);
    assert(tab_found != vinfo_->prefix2range.end());
    now_range = tab_found->second;

    //DBG_PRINT("Append to Range[%s]", meta.prefix.c_str());
    //DBG_PRINT("start append");
    if (!now_range->EnoughFroWriting(bloom_data.size() + chunk_data.size())) {
        // not enough
        if (now_range->HasCompactionBuf()) {
            // has no space need wait
            while (now_range->EnoughFroWriting(bloom_data.size() + chunk_data.size())) {
                sleep(1);
            }
        } else {
            // switch buffer
            now_range->lock();
            now_range->SwitchBuffer(kToCBuffer);
            now_range->unlock();
        }
    }
    now_range->lock();
    now_range->Append(bloom_data, chunk_data, meta.cur_start, meta.cur_end);
    now_range->unlock();
    //DBG_PRINT("end append");

}

persistent_ptr<NvRangeTab> FixedRangeChunkBasedNVMWriteCache::NewContent(const string &prefix, size_t bufSize) {
    persistent_ptr<NvRangeTab> p_content_1, p_content_2;
    int offset1 = 0, offset2 = 0;
    char *pmem1 = pinfo_->allocator_->Allocate(offset1);
    char *pmem2 = pinfo_->allocator_->Allocate(offset2);
    transaction::run(pop_, [&]{
        p_content_1 = make_persistent<NvRangeTab>(pop_, pmem1, offset1, prefix, bufSize);
        p_content_2 = make_persistent<NvRangeTab>(pop_, pmem2, offset2, prefix, bufSize);
        // NvRangeTab怎么释放空间
    });
    p_content_1->pair_buf_ = p_content_2;
    p_content_2->pair_buf_ = p_content_1;
    return p_content_1;
}


FixedRangeTab *FixedRangeChunkBasedNVMWriteCache::NewRange(const std::string &prefix) {
    persistent_ptr<NvRangeTab> p_content = NewContent(prefix, vinfo_->internal_options_->range_size_);
    pinfo_->range_map_->put(pop_, p_content);
    p_content->writting_ = true;
    FixedRangeTab *range = new FixedRangeTab(pop_, vinfo_->internal_options_, vinfo_->icmp_, p_content);
    vinfo_->prefix2range.insert({prefix, range});
    return range;
}

void FixedRangeChunkBasedNVMWriteCache::MaybeNeedCompaction() {
    //DBG_PRINT("start compaction check");
    // 选择所有range中数据大小占总容量80%的range并按照总容量的大小顺序插入compaction queue
    std::vector<CompactionItem> pendding_compact;
    for (auto range : vinfo_->prefix2range) {
        FixedRangeTab *tab = range.second;
        if (tab->IsCompactWorking()) {
            continue;
        }
        if (tab->IsCompactPendding()) {
            continue;
        }
        Usage range_usage = range.second->RangeUsage(kForWritting);
        if (range_usage.range_size >= tab->max_range_size() * 0.9 || (tab->HasCompactionBuf()&&
                                                                        !tab->IsCompactPendding() &&
                                                                        !tab->IsCompactWorking())) {
            DBG_PRINT("Range [%s] Need Compaction [%f]MB > [%f]MB", tab->prefix().c_str(),
                    range_usage.range_size/10418576.0,
                    tab->max_range_size() * 0.9);
            vinfo_->queue_lock_.Lock();
            tab->SetCompactionPendding(true);
            vinfo_->range_queue_.push_back(tab);
            vinfo_->queue_lock_.Unlock();
        }
    }
    DBG_PRINT("[%lu]range need compaction", pendding_compact.size());
}

// call by compaction thread
void FixedRangeChunkBasedNVMWriteCache::GetCompactionData(rocksdb::CompactionItem *compaction) {
    assert(!vinfo_->range_queue_.empty());
    vinfo_->queue_lock_.Lock();
    std::sort(vinfo_->range_queue_.begin(), vinfo_->range_queue_.end(),
              [](const FixedRangeTab *ltab, const FixedRangeTab *rtab) {
                  return ltab->RangeUsage(kForWritting).range_size <
                         rtab->RangeUsage(kForWritting).range_size;
              });
    //DBG_PRINT("In cache lock");
    compaction->pending_compated_range_ = vinfo_->range_queue_.back();
    compaction->range_usage = compaction->pending_compated_range_->RangeUsage(kForCompaction);
    compaction->allocator_ = nullptr;

    vinfo_->range_queue_.pop_back();
    compaction->pending_compated_range_->SetCompactionPendding(false);
    compaction->pending_compated_range_->SetCompactionWorking(true);
    vinfo_->queue_lock_.Unlock();
    //DBG_PRINT("end get compaction and unlock");
}

void FixedRangeChunkBasedNVMWriteCache::RebuildFromPersistentNode() {
    // 遍历每个Node，获取NvRangeTab
    // 根据NvRangeTab构建FixeRangeTab
    PersistentInfo *vpinfo = pinfo_.get();
    pmem_hash_map<NvRangeTab> *vhash_map = vpinfo->range_map_.get();
    vector<persistent_ptr<NvRangeTab> > tab_vec;
    vhash_map->getAll(tab_vec);
    char *raw_space = vpinfo->allocator_->raw().get();
    for (auto content : tab_vec) {
        // 一对buf的状态一定是不一样的
        assert(content->writting_ != content->pair_buf_->writting_);
        NvRangeTab *ptab = content.get();
        if(!ptab->writting_){
            content = content->pair_buf_;
        }
        // 恢复tab的char*指针，这是一个易失量
        // offset记录的是第几个分配单位，分配单位是range size
        ptab->SetRaw(raw_space + ptab->offset_ * vinfo_->internal_options_->range_size_);
        FixedRangeTab *recovered_tab = new FixedRangeTab(pop_, vinfo_->internal_options_, vinfo_->icmp_, content);
        string recoverd_prefix(content->prefix_.get(), content->prefix_len_);
        vinfo_->prefix2range[recoverd_prefix] = recovered_tab;
    }
    MaybeNeedCompaction();
}


InternalIterator *FixedRangeChunkBasedNVMWriteCache::NewIterator(const InternalKeyComparator *icmp, Arena *arena) {
    InternalIterator *internal_iter;
    MergeIteratorBuilder merge_iter_builder(icmp, arena);
    for (auto range : vinfo_->prefix2range) {
        merge_iter_builder.AddIterator(range.second->NewInternalIterator(arena));
    }

    internal_iter = merge_iter_builder.Finish();
    return internal_iter;
}

void FixedRangeChunkBasedNVMWriteCache::RangeExistsOrCreat(const std::string &prefix) {
    auto tab_idx = vinfo_->prefix2range.find(prefix);
    if (tab_idx == vinfo_->prefix2range.end()) {
        DBG_PRINT("Need to create range[%s][%lu]", prefix.c_str(), prefix.size());
        NewRange(prefix);
        //DBG_PRINT("End of creating range");
    }
}

// IMPORTANT!!!
// ONLY FOR TEST
FixedRangeTab *FixedRangeChunkBasedNVMWriteCache::GetRangeTab(const std::string &prefix) {
    auto res_ = vinfo_->prefix2range.find(prefix);
    return res_->second;
}

} // namespace rocksdb


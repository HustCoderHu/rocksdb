#include "utilities/nvm_write_cache/skiplist/test_common.h"
#include "fixed_range_chunk_based_nvm_write_cache.h"

namespace rocksdb {

using std::string;

FixedRangeChunkBasedNVMWriteCache::FixedRangeChunkBasedNVMWriteCache(
        const FixedRangeBasedOptions *ioptions,
        const string &file, uint64_t pmem_size,
        bool reset) {
    //bool justCreated = false;
    vinfo_ = new VolatileInfo(ioptions);
    vinfo_->lock_count = 0;
    if (file_exists(file.c_str()) != 0) {
        pop_ = pmem::obj::pool<PersistentInfo>::create(file.c_str(), "FixedRangeChunkBasedNVMWriteCache", pmem_size,
                                                       CREATE_MODE_RW);
        //justCreated = true;

    } else {
        pop_ = pmem::obj::pool<PersistentInfo>::open(file.c_str(), "FixedRangeChunkBasedNVMWriteCache");
    }

    pinfo_ = pop_.root();
    if (!pinfo_->inited_ || reset) {
        transaction::run(pop_, [&] {
            // TODO 配置
            pinfo_->range_map_ = make_persistent<pmem_hash_map<NvRangeTab>>(pop_, 0.75, 256);
            /*pinfo_->range_map_->tabLen = 0;
             pinfo_->range_map_->tab = make_persistent<p_node_t<NvRangeTab>[]>(pinfo_->range_map_->tabLen);
             pinfo_->range_map_->loadFactor = 0.75f;
             pinfo_->range_map_->threshold = pinfo_->range_map_->tabLen * pinfo_->range_map_->loadFactor;
             pinfo_->range_map_->size = 0;

             persistent_ptr<char[]> data_space = make_persistent<char[]>(pmem_size);
             pinfo_->allocator_ = make_persistent<PersistentAllocator>(data_space, pmem_size);*/

            pinfo_->inited_ = true;
        });
    } else {
        RebuildFromPersistentNode();
    }

}

FixedRangeChunkBasedNVMWriteCache::~FixedRangeChunkBasedNVMWriteCache() {
    for(auto range: vinfo_->prefix2range){
        // 释放FixedRangeTab的空间
        delete range.second;
    }
    vinfo_->prefix2range.clear();
    delete vinfo_->internal_options_;
    delete vinfo_;
    pop_.close();
}

Status FixedRangeChunkBasedNVMWriteCache::Get(const InternalKeyComparator &internal_comparator, const LookupKey &lkey,
                                              std::string *value) {
    std::string prefix = (*vinfo_->internal_options_->prefix_extractor_)(lkey.user_key().data(), lkey.user_key().size());
    DBG_PRINT("prefix: [%s], size[%lu]", prefix.c_str(), prefix.size());
    auto found_tab = vinfo_->prefix2range.find(prefix);
    if (found_tab == vinfo_->prefix2range.end()) {
        // not found
        DBG_PRINT("NotFound prefix");
        return Status::NotFound("no this range");
    } else {
        // found
        DBG_PRINT("Found prefix");
        FixedRangeTab *tab = found_tab->second;
        return tab->Get(internal_comparator, lkey, value);
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

    //DBG_PRINT("before append");
    now_range->lock();
    //DBG_PRINT("start append");
    if (now_range->IsCompactWorking() && !now_range->IsExtraBufExists()) {
        persistent_ptr<NvRangeTab> p_content = NewContent(meta.prefix, vinfo_->internal_options_->range_size_);
        now_range->SetExtraBuf(p_content);
    }
    now_range->Append(icmp, bloom_data, chunk_data, meta.cur_start, meta.cur_end);
    now_range->unlock();
    //DBG_PRINT("end append");

}

persistent_ptr<NvRangeTab> FixedRangeChunkBasedNVMWriteCache::NewContent(const string &prefix, size_t bufSize) {
    persistent_ptr<NvRangeTab> p_content;
    transaction::run(pop_, [&] {
        p_content = make_persistent<NvRangeTab>(pop_, prefix, bufSize);
    });
    return p_content;
}


FixedRangeTab *FixedRangeChunkBasedNVMWriteCache::NewRange(const std::string &prefix) {
    persistent_ptr<NvRangeTab> p_content = NewContent(prefix, vinfo_->internal_options_->range_size_);
    pinfo_->range_map_->put(pop_, p_content);


    //p_range::p_node new_node = pinfo_->range_map_->get_node(_hash, prefix);
    FixedRangeTab *range = new FixedRangeTab(pop_, vinfo_->internal_options_, p_content);
    vinfo_->prefix2range.insert({prefix, range});
    return range;
}

void FixedRangeChunkBasedNVMWriteCache::MaybeNeedCompaction() {
    //DBG_PRINT("start compaction check");
    // 选择所有range中数据大小占总容量80%的range并按照总容量的大小顺序插入compaction queue
    std::vector<CompactionItem> pendding_compact;
    for (auto range : vinfo_->prefix2range) {
        if (range.second->IsCompactPendding() || range.second->IsCompactWorking()) {
            // this range has already in compaction queue
            continue;
        }

        if(range.second->IsExtraBufExists()){
            // 对于已有extra buffer的range直接加入
            pendding_compact.emplace_back(range.second);
            continue;
        }

        Usage range_usage = range.second->RangeUsage();
        if (range_usage.range_size >= range.second->max_range_size() * 0.8) {
            pendding_compact.emplace_back(range.second);
        }
    }
    //DBG_PRINT("[%lu]range need compaction", pendding_compact.size());
    std::sort(pendding_compact.begin(), pendding_compact.end(),
              [](const CompactionItem &litem, const CompactionItem &ritem) {
                  return litem.pending_compated_range_->RangeUsage().range_size >
                         ritem.pending_compated_range_->RangeUsage().range_size;
              });

    // TODO 是否需要重新添加queue
    //DBG_PRINT("Cache lock[%d]", vinfo_->lock_count);
    vinfo_->queue_lock_.Lock();
    vinfo_->lock_count++;
    //DBG_PRINT("In cache lock[%d]", vinfo_->lock_count);
    for (auto pendding_range : pendding_compact) {
        if (!pendding_range.pending_compated_range_->IsCompactPendding()) {
            pendding_range.pending_compated_range_->SetCompactionPendding(true);
            vinfo_->range_queue_.push(std::move(pendding_range));
        }
    }
    vinfo_->queue_lock_.Unlock();
    vinfo_->lock_count--;
    //DBG_PRINT("end compaction check and unlock[%d]", vinfo_->lock_count);
}

void FixedRangeChunkBasedNVMWriteCache::GetCompactionData(rocksdb::CompactionItem *compaction) {

    assert(!vinfo_->range_queue_.empty());
    //DBG_PRINT("Cache lock[%d]", vinfo_->lock_count);
    vinfo_->queue_lock_.Lock();
    vinfo_->lock_count++;
    //DBG_PRINT("In cache lock[%d]", vinfo_->lock_count);
    *compaction = vinfo_->range_queue_.front();
    vinfo_->range_queue_.pop();
    vinfo_->queue_lock_.Unlock();
    vinfo_->lock_count--;
    //DBG_PRINT("end get compaction and unlock[%d]", vinfo_->lock_count);

}

void FixedRangeChunkBasedNVMWriteCache::RebuildFromPersistentNode() {
    // 遍历每个Node，获取NvRangeTab
    // 根据NvRangeTab构建FixeRangeTab
    PersistentInfo *vpinfo = pinfo_.get();
    pmem_hash_map<NvRangeTab> *vhash_map = vpinfo->range_map_.get();
    vector<persistent_ptr<NvRangeTab> > tab_vec;
    vhash_map->getAll(tab_vec);
    for (auto content : tab_vec) {
        FixedRangeTab *recovered_tab = new FixedRangeTab(pop_, vinfo_->internal_options_, content);
        string recoverd_prefix(content->prefix_.get(), content->prefixLen);
        vinfo_->prefix2range[recoverd_prefix] = recovered_tab;
    }
    MaybeNeedCompaction();
}


InternalIterator *FixedRangeChunkBasedNVMWriteCache::NewIterator(const InternalKeyComparator *icmp, Arena *arena) {
    InternalIterator *internal_iter;
    MergeIteratorBuilder merge_iter_builder(icmp, arena);
    for (auto range : vinfo_->prefix2range) {
        merge_iter_builder.AddIterator(range.second->NewInternalIterator(icmp, arena));
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
FixedRangeTab* FixedRangeChunkBasedNVMWriteCache::GetRangeTab(const std::string &prefix) {
	auto res_ = vinfo_->prefix2range.find(prefix);
	return res_->second;
}

} // namespace rocksdb


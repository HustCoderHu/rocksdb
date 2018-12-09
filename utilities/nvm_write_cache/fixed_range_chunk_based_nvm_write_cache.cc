#include "utilities/nvm_write_cache/skiplist/test_common.h"
#include "fixed_range_chunk_based_nvm_write_cache.h"

namespace rocksdb {

using std::string;

FixedRangeChunkBasedNVMWriteCache::FixedRangeChunkBasedNVMWriteCache(
        const FixedRangeBasedOptions *ioptions,
        const InternalKeyComparator* icmp,
        const string &file, uint64_t pmem_size,
        bool reset) {
    //bool justCreated = false;
    vinfo_ = new VolatileInfo(ioptions, icmp);
    vinfo_->lock_count = 0;
    if (file_exists(file.c_str()) != 0) {
        // creat pool
        pop_ = pmem::obj::pool<PersistentInfo>::create(file.c_str(), "FixedRangeChunkBasedNVMWriteCache", pmem_size,
                                                       CREATE_MODE_RW);
    } else {
        // open pool
        pop_ = pmem::obj::pool<PersistentInfo>::open(file.c_str(), "FixedRangeChunkBasedNVMWriteCache");
    }

    pinfo_ = pop_.root();
    if (!pinfo_->inited_) {
        // init cache
        transaction::run(pop_, [&] {
            pinfo_->range_map_ = make_persistent<pmem_hash_map<NvRangeTab>>(pop_, 0.75, 256);
            persistent_ptr<char[]> buf = make_persistent<char[]>(pmem_size);
            persistent_ptr<PersistentBitMap> bitmap = make_persistent<PersistentBitMap>(pop_,
                    pmem_size / ioptions->range_size_);
            pinfo_->allocator_ = make_persistent<PersistentAllocator>(buf, pmem_size, ioptions->range_size_, bitmap);
            pinfo_->inited_ = true;
        });
    }else if(reset){
        // reset cache
        transaction::run(pop_, [&] {
            delete_persistent<pmem_hash_map>(pinfo_->range_map_);
            pinfo_->range_map_ = make_persistent<pmem_hash_map<NvRangeTab>>(pop_, 0.75, 256);
        });
        pinfo_->allocator_->Reset();
    }else {
        // rebuild cache
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

bool FixedRangeChunkBasedNVMWriteCache::Get(const InternalKeyComparator &internal_comparator, Status *s, const LookupKey &lkey,
                                              std::string *value) {
    std::string prefix = (*vinfo_->internal_options_->prefix_extractor_)(lkey.user_key().data(), lkey.user_key().size());
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
    if(!now_range->EnoughFroWriting(bloom_data.size() + chunk_data.size())){
        // not enough
        if(now_range->HasCompactionBuf()){
            // has no space need wait
            while (now_range->EnoughFroWriting(bloom_data.size() + chunk_data.size())){
                sleep(1);
            }
        }else{
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
    char* pmem1 = pinfo_->allocator_->Allocate(offset1);
    char* pmem2 = pinfo_->allocator_->Allocate(offset2);
    transaction::run(pop_, [&] {
        p_content_1 = make_persistent<NvRangeTab>(pop_, pmem1, offset1, bufSize);
        p_content_2 = make_persistent<NvRangeTab>(pop_, pmem2, offset2, bufSize);
        // NvRangeTab怎么释放空间
    });
    p_content_1->pair_buf_ = p_content_2;
    return p_content_1;
}


FixedRangeTab *FixedRangeChunkBasedNVMWriteCache::NewRange(const std::string &prefix) {
    persistent_ptr<NvRangeTab> p_content = NewContent(prefix, vinfo_->internal_options_->range_size_);
    pinfo_->range_map_->put(pop_, p_content);


    //p_range::p_node new_node = pinfo_->range_map_->get_node(_hash, prefix);
    FixedRangeTab *range = new FixedRangeTab(pop_, vinfo_->internal_options_, vinfo_->icmp_, p_content);
    vinfo_->prefix2range.insert({prefix, range});
    return range;
}

void FixedRangeChunkBasedNVMWriteCache::MaybeNeedCompaction() {
    //DBG_PRINT("start compaction check");
    // 选择所有range中数据大小占总容量80%的range并按照总容量的大小顺序插入compaction queue
    std::vector<CompactionItem> pendding_compact;
    for (auto range : vinfo_->prefix2range) {

        /*{
            Usage range_usage = range.second->RangeUsage();
            DBG_PRINT("range[%s] size[%f]MB threshold [%f]MB",range.first.c_str(), range_usage.range_size / 1048576.0, (range.second->max_range_size() / 1048576.0) * 0.8);
        }*/
        Usage range_usage = range.second->RangeUsage();
        if (range.second->IsCompactPendding() || range.second->IsCompactWorking()) {
            // this range has already in compaction queue
            if(range.second->IsCompactPendding()){
                DBG_PRINT("Already in queue");
            }else{
                DBG_PRINT("Compaction Working");
            }
            //DBG_PRINT("pendding or compaction range[%s] size[%f]MB threshold [%f]MB",range.first.c_str(), range_usage.range_size / 1048576.0, (range.second->max_range_size() / 1048576.0) * 0.8);
            continue;
        }
        if(range.second->IsExtraBufExists() && !range.second->IsCompactWorking()){
            // 对于已有extra buffer的range直接加入
            //DBG_PRINT("has extra buf range size[%f]MB threshold [%f]MB", range_usage.range_size / 1048576.0, (range.second->max_range_size() / 1048576.0) * 0.8);
            DBG_PRINT("Has Extra Buffer");
            pendding_compact.emplace_back(range.second, pinfo_->allocator_);
            continue;
        }

        if (range_usage.range_size >= range.second->max_range_size() * 0.8) {
            DBG_PRINT("Oversize and not in queue");
            //DBG_PRINT("general range[%s] size[%f]MB threshold [%f]MB add to queue",range.first.c_str(), range_usage.range_size / 1048576.0, (range.second->max_range_size() / 1048576.0) * 0.8);
            pendding_compact.emplace_back(range.second, pinfo_->allocator_);
        }
    }
    DBG_PRINT("[%lu]range need compaction", pendding_compact.size());
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
        DBG_PRINT("pendding range size[%lu]", pendding_range.pending_compated_range_->RangeUsage().range_size);
        if (!pendding_range.pending_compated_range_->IsCompactPendding()) {
            pendding_range.pending_compated_range_->SetCompactionPendding(true);
            vinfo_->range_queue_.push(std::move(pendding_range));
        }
    }
    vinfo_->lock_count--;
    vinfo_->queue_lock_.Unlock();

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
    char* raw_space = vpinfo->allocator_->raw().get();
    for (auto content : tab_vec) {
        NvRangeTab* ptab = content.get();
        ptab->SetRaw(raw_space + ptab->offset_);
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
FixedRangeTab* FixedRangeChunkBasedNVMWriteCache::GetRangeTab(const std::string &prefix) {
	auto res_ = vinfo_->prefix2range.find(prefix);
	return res_->second;
}

} // namespace rocksdb


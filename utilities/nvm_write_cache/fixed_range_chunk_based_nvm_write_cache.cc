#include "utilities/nvm_write_cache/skiplist/test_common.h"
#include "fixed_range_chunk_based_nvm_write_cache.h"
#include "global_statistic.h"
//#define RANGE_SIZE_TEST
//#define FLUSH_CACUL
#define RANGE_SIZE_MULTIPLE 1
namespace rocksdb {
pool<FixedRangeChunkBasedNVMWriteCache::PersistentInfo> FixedRangeChunkBasedNVMWriteCache::pop_ =
	pool<FixedRangeChunkBasedNVMWriteCache::PersistentInfo>();
int FixedRangeChunkBasedNVMWriteCache::refs_ = 0;

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
        DBG_PRINT("pool:create : %s", file.c_str());
        DBG_PRINT("pmem size[%f]GB", pmem_size / (1073741824.0));
        pop_ = pmem::obj::pool<PersistentInfo>::create(file, "FixedRangeChunkBasedNVMWriteCache", pmem_size,
                                                       CREATE_MODE_RW | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        ++refs_;
    } else if (0 == refs_){
        // open pool
        DBG_PRINT("pool:open : %s", file.c_str());
        pop_ = pmem::obj::pool<PersistentInfo>::open(file, "FixedRangeChunkBasedNVMWriteCache");
        DBG_PRINT("pool:open finish");
        ++refs_;
    } else {
        DBG_PRINT("%s opened already", file.c_str());
    }

    pinfo_ = pop_.root();
    if (!pinfo_->inited_) {
        // init cache
        //uint64_t range_pool_size = pmem_size / 4;
        //range_pool_size += ioptions->range_size_ * 5;
        uint64_t total_range_num = ioptions->range_num_ * 2 + 6;
        transaction::run(pop_, [&] {
            DBG_PRINT("alloc range map");
            pinfo_->range_map_ = make_persistent<pmem_hash_map<NvRangeTab>>(pop_, 0.75, 256);
            //DBG_PRINT("alloc raw buf[%f]GB", range_pool_size/(1073741824.0));
            DBG_PRINT("alloc bitmap[%d]bits", ioptions->range_num_);
            persistent_ptr<PersistentBitMap> bitmap = make_persistent<PersistentBitMap>(pop_, total_range_num);
            pinfo_->allocator_ = make_persistent<PersistentAllocator>(file + ".PersistentAllocator",
                total_range_num * ioptions->range_size_ * RANGE_SIZE_MULTIPLE,
                ioptions->range_size_ * RANGE_SIZE_MULTIPLE, bitmap);
            pinfo_->inited_ = true;
            FixedRangeTab::base_raw_ = pinfo_->allocator_->raw();
        });
    } else if (reset) {
        // reset cache
        pinfo_->allocator_->Recover();
        FixedRangeTab::base_raw_ = pinfo_->allocator_->raw();
        transaction::run(pop_, [&] {
            delete_persistent<pmem_hash_map<NvRangeTab>>(pinfo_->range_map_);
            pinfo_->range_map_ = make_persistent<pmem_hash_map<NvRangeTab>>(pop_, 0.75, 256);
        });
        pinfo_->allocator_->Reset();
    } else {
        // rebuild cache
        DBG_PRINT("recover cache");
        pinfo_->allocator_->Recover();
        FixedRangeTab::base_raw_ = pinfo_->allocator_->raw();
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
	--refs_;
	if (0 == refs_)
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
        //printf("NotFound prefix\n");
        return false;
    } else {
        // found
        DBG_PRINT("Found prefix");
        FixedRangeTab *tab = found_tab->second;
        tab->lock();
        bool result = tab->Get(s, lkey, value);
        tab->unlock();
       /* if(result){
            printf("found\n");
        }else{
            printf("not found\n");
        }*/
        return result;
    }
}

void FixedRangeChunkBasedNVMWriteCache::AppendToRange(const rocksdb::InternalKeyComparator &icmp,
                                                      const string &bloom_data, const rocksdb::Slice &chunk_data,
                                                      const rocksdb::ChunkMeta &meta, int key_num) {
    /*
     * 1. 获取prefix
     * 2. 调用tangetab的append
     * */

    while(CompactionScore() > 1){
        sleep(1);
    }
    FixedRangeTab *now_range = nullptr;
    auto tab_found = vinfo_->prefix2range.find(meta.prefix);
    assert(tab_found != vinfo_->prefix2range.end());
    now_range = tab_found->second;

    //DBG_PRINT("Append to Range[%s]", meta.prefix.c_str());
    //DBG_PRINT("start append");
    if (!now_range->EnoughFroWriting(bloom_data.size() + chunk_data.size() + 2 * 8)) {
        // not enough
        /*if (now_range->HasCompactionBuf()) {

            while (now_range->EnoughFroWriting(bloom_data.size() + chunk_data.size())) {
                sleep(1);
            }
        }*/
        printf("no enough for w_buffer\n");
        while (now_range->HasCompactionBuf()) {
            //printf("wait for compaction\n");
            // has no space need wait
            // wait fo compaction end
            //sleep(1);
            //usleep(10);
        }
        // switch buffer
        now_range->lock();
        now_range->SwitchBuffer(kToCBuffer);
        now_range->unlock();
    }
    now_range->lock();
#ifdef FLUSH_CACUL
    uint64_t start_time = Env::Default()->NowMicros();
#endif
    //printf("num key = [%d]\n", key_num);
    now_range->Append(bloom_data, chunk_data, meta.cur_start, meta.cur_end, key_num);
#ifdef FLUSH_CACUL
    uint64_t end_time = Env::Default()->NowMicros();
    FILE *fp = fopen("append_time", "a");
    fprintf(fp, "flush %lu bytes spent %lu time\n", bloom_data.size() + chunk_data.size() + 16, end_time - start_time);
    fclose(fp);
#endif
    now_range->unlock();
    // atomic add
    //vinfo_->total_size_.fetch_add(bloom_data.size() + chunk_data.size() + 2 * 8);
#ifdef RANGE_SIZE_TEST
    {
        vinfo_->total_size_ += bloom_data.size() + chunk_data.size();
        if(vinfo_->total_size_ >= 1ul * 1024 * 1024 * 1024){
            vinfo_->total_size_ = 0;
            /*vector<persistent_ptr<NvRangeTab> > tab_vec;
            pinfo_->range_map_->getAll(tab_vec);
            FILE* fp = fopen("/home/hustzyw/nvm-rocksdb/range-data-size", "a");
            for(auto tab : tab_vec){
                fprintf(fp, "%f,", (tab->data_len_ + tab->pair_buf_->data_len_)/1048576.0);
            }
            fprintf(fp, "\n");
            fclose(fp);*/

            uint64_t total_size = 0;
            FILE* fp2 = fopen("/home/hustzyw/nvm-rocksdb/range_statitics.csv", "a");
            for(auto range : vinfo_->prefix2range){
                uint64_t range_size = range.second->IsCompactWorking() ? range.second->WriteBufferSize() : range.second->RangeTotalSize();
                fprintf(fp2, "%f,", range_size / 1048576.0 / 1024);
                total_size += range_size;
            }
            fprintf(fp2, "\ntotal,%f\n", total_size / 1048576.0 / 1024);
            fclose(fp2);

        }
    }

#endif
    //DBG_PRINT("end append");

}

persistent_ptr<NvRangeTab> FixedRangeChunkBasedNVMWriteCache::NewContent(const string &prefix, size_t bufSize) {
    persistent_ptr<NvRangeTab> p_content_1, p_content_2;
    int offset1 = 0, offset2 = 0;
    /*p_buf pmem1 = */pinfo_->allocator_->Allocate(offset1);
    /*p_buf pmem2 = */pinfo_->allocator_->Allocate(offset2);
    DBG_PRINT("alloc range[%d][%d]", offset1, offset2);
    transaction::run(pop_, [&] {
        p_content_1 = make_persistent<NvRangeTab>(pop_,/* pmem1, */offset1, prefix, bufSize);
        p_content_2 = make_persistent<NvRangeTab>(pop_,/* pmem2, */offset2, prefix, bufSize);
        // NvRangeTab怎么释放空间
    });
    p_content_1->pair_buf_ = p_content_2;
    p_content_2->pair_buf_ = p_content_1;
    return p_content_1;
}


FixedRangeTab *FixedRangeChunkBasedNVMWriteCache::NewRange(const std::string &prefix) {
    persistent_ptr<NvRangeTab> p_content = NewContent(prefix, vinfo_->internal_options_->range_size_ * RANGE_SIZE_MULTIPLE);
    pinfo_->range_map_->put(pop_, p_content);
    p_content->writting_ = true;
    FixedRangeTab *range = new FixedRangeTab(pop_, vinfo_->internal_options_, vinfo_->icmp_, p_content);
    vinfo_->prefix2range.insert({prefix, range});
    return range;
}

void FixedRangeChunkBasedNVMWriteCache::MaybeNeedCompaction() {
    //DBG_PRINT("start compaction check");
    // 选择所有range中数据大小占总容量80%的range并按照总容量的大小顺序插入compaction queue
    /*std::vector<CompactionItem> pendding_compact;
    int compaction_working_range = 0, compaction_pendding_range = 0;
    for (auto range : vinfo_->prefix2range) {
        FixedRangeTab *tab = range.second;
        if (tab->IsCompactWorking()) {
            compaction_working_range++;
            continue;
        }
        if (tab->IsCompactPendding()) {
            compaction_pendding_range++;
            continue;
        }
        //Usage range_usage = range.second->RangeUsage(kForWritting);
        if (tab->WriteBufferSize() >= tab->max_range_size() * 0.45 || (tab->HasCompactionBuf()&&
                                                                        !tab->IsCompactPendding() &&
                                                                        !tab->IsCompactWorking())) {
            DBG_PRINT("Range [%s] Need Compaction [%f]MB > [%f]MB", tab->prefix().c_str(),
                      tab->WriteBufferSize() / 1048576.0,
                      (tab->max_range_size() / 1048576.0) * 0.45);
            vinfo_->queue_lock_.Lock();
            tab->SetCompactionPendding(true);
            vinfo_->range_queue_.push_back(tab);
            vinfo_->queue_sorted_ = false;
            vinfo_->queue_lock_.Unlock();
        }
    }
    DBG_PRINT("[%lu]range need compaction", pendding_compact.size());
    DBG_PRINT("[%d]range compaction working", compaction_working_range);
    DBG_PRINT("[%d]range compaction pendding", compaction_pendding_range);*/
#ifdef DENY_COMPACTION

#else
#ifdef DELAY_COUNT
    int total = 0;
    for(int i = 0 ; i < 5; i++){
        total += delay_stat[i];
    }
    int threshold = total / 5;
    uint64_t total_buffer_size = vinfo_->internal_options_->range_num_ * vinfo_->internal_options_->range_size_;
    uint64_t total_size = 0;
    for(auto range : vinfo_->prefix2range){
        total_size += range.second->RangeTotalSize();
    }
    if(total_size > total_buffer_size * 0.8 && delay_count < static_cast<int>(threshold * key_percent)){
        printf("compaction triggered by small delay[%d]<[%d]([%d]*[%f])[%d]\n", delay_count, static_cast<int>(threshold * key_percent), threshold, key_percent, key_written);
        vinfo_->compaction_requested_ = true;
    }else if(total_size > total_buffer_size * 0.9){
        printf("compaction triggered by data size delay[%d]:threshold[%d]*[%f]=[%d]\n", delay_count, threshold, key_percent,key_percent*threshold);
        vinfo_->compaction_requested_ = true;
    }
#else
    /*uint64_t total_buffer_size = vinfo_->internal_options_->range_num_ * vinfo_->internal_options_->range_size_;
    uint64_t total_size = 0;
    for (auto range : vinfo_->prefix2range) {
        if(range.second->IsCompactWorking()){
            total_size += range.second->WriteBufferSize();
        }else{
            total_size += range.second->RangeTotalSize();
        }
    }*/
    CaculateScore();
    if (CompactionScore() > 0.7) {
        vinfo_->compaction_requested_ = true;
    }
    //printf("score = [%f]\n", vinfo_->compaction_score_);
#endif
#endif

}

void FixedRangeChunkBasedNVMWriteCache::RollbackCompaction(rocksdb::FixedRangeTab *range) {
    DBG_PRINT("Rollback compaction[%s]", range->prefix().c_str());
    vinfo_->last_canceled_ = range;
    //vinfo_->queue_lock_.Lock();
    //range->SetCompactionPendding(true);
    //vinfo_->range_queue_.push_back(range);
    //vinfo_->queue_lock_.Unlock();
    //vinfo_->total_size_.fetch_add(range->RangeUsage(kForCompaction).range_size);
}

// call by compaction thread
void FixedRangeChunkBasedNVMWriteCache::GetCompactionData(rocksdb::CompactionItem *compaction) {
    assert(!vinfo_->range_queue_.empty());
    //vinfo_->queue_lock_.Lock();
    /*if(!vinfo_->queue_sorted_){
        std::sort(vinfo_->range_queue_.begin(), vinfo_->range_queue_.end(),
                  [](const FixedRangeTab *ltab, const FixedRangeTab *rtab) {
                      // 升序
                      return ltab->RangeUsage(kForTotal).range_size <
                             rtab->RangeUsage(kForTotal).range_size;
                  });
        vinfo_->queue_sorted_ = true;
    }*/
    uint64_t max_range_size = 0;
    FixedRangeTab *pendding_range = nullptr;
    for (auto range : vinfo_->prefix2range) {
        // choose a range with max size
        // skip range which is in compaction
        if (range.second->IsCompactWorking()) continue;
        if (range.second == vinfo_->last_canceled_) {
            printf("skip last cancled range\n");
            continue;
        }
        uint64_t range_size = range.second->RangeTotalSize();
        if (max_range_size < range_size) {
            max_range_size = range_size;
            pendding_range = range.second;
        }
    }
    compaction->pending_compated_range_ = pendding_range;
    vinfo_->last_canceled_ = nullptr;
    //DBG_PRINT("In cache lock");
    /*DBG_PRINT("Get range[%s], size[%f]",pendding_range->prefix().c_str(),
              pendding_range->RangeUsage(kForCompaction).range_size / 1048576.0);*/
    //compaction->pending_compated_range_ = vinfo_->range_queue_.back();
    //compaction->pending_compated_range_ = vinfo_->range_queue_.back();

    //assert(pendding_range != nullptr);
    if (!compaction->pending_compated_range_->HasCompactionBuf()) {
        // TODO : 可能有问题
        compaction->pending_compated_range_->lock();
        compaction->pending_compated_range_->SwitchBuffer(kToCBuffer);
        compaction->pending_compated_range_->unlock();
    }
    compaction->range_usage = compaction->pending_compated_range_->RangeUsage(kForCompaction);
    DBG_PRINT("Get range[%s], size[%f]", compaction->pending_compated_range_->prefix().c_str(),
              compaction->range_usage.range_size / 1048576.0);
    compaction->allocator_ = nullptr;

    //vinfo_->range_queue_.pop_back();
    compaction->pending_compated_range_->SetCompactionPendding(false);
    compaction->pending_compated_range_->SetCompactionWorking(true);

    // total size decline and caculate state
    //uint64_t total_buffer_size = vinfo_->internal_options_->range_size_ * vinfo_->internal_options_->range_num_;
    //atomic sub
    //vinfo_->total_size_.fetch_sub(compaction->range_usage.range_size);
    //uint64_t total_size = 0;
    /*for (auto range : vinfo_->prefix2range) {
        if(range.second->IsCompactWorking()){
            total_size += range.second->WriteBufferSize();
        }else{
            total_size += range.second->RangeTotalSize();
        }
    }*/
    CaculateScore();
    if (CompactionScore() <= 0.7) vinfo_->compaction_requested_ = false;

    //vinfo_->queue_lock_.Unlock();
    //DBG_PRINT("end get compaction and unlock");
}

void FixedRangeChunkBasedNVMWriteCache::RebuildFromPersistentNode() {
    // 遍历每个Node，获取NvRangeTab
    // 根据NvRangeTab构建FixeRangeTab
    PersistentInfo *vpinfo = pinfo_.get();
    pmem_hash_map<NvRangeTab> *vhash_map = vpinfo->range_map_.get();
    vector<persistent_ptr<NvRangeTab> > tab_vec;
    vhash_map->getAll(tab_vec);
    //char *raw_space = vpinfo->allocator_->raw().get();
    DBG_PRINT("get all content");
    for (auto content : tab_vec) {
        // 一对buf的状态一定是不一样的
        assert(content->writting_ != content->pair_buf_->writting_);
        NvRangeTab *ptab = content.get();
        DBG_PRINT("Recover range[%s]", string(ptab->prefix_.get(), ptab->prefix_len_).c_str());
        if (!ptab->writting_) {
            content = content->pair_buf_;
        }
        // 恢复tab的char*指针，这是一个易失量
        // offset记录的是第几个分配单位，分配单位是range size
        //ptab->SetRaw(raw_space + ptab->offset_ * vinfo_->internal_options_->range_size_);
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

double FixedRangeChunkBasedNVMWriteCache::CompactionScore() {
    return vinfo_->compaction_score_;
}

void FixedRangeChunkBasedNVMWriteCache::CaculateScore() {
    uint64_t total_size = 0;
    for(auto range : vinfo_->prefix2range){
        if(range.second->IsCompactWorking()){
            total_size += range.second->WriteBufferSize();
        }else{
            total_size += range.second->RangeTotalSize();
        }
    }
    vinfo_->compaction_score_ = static_cast<double>(total_size) / (vinfo_->internal_options_->range_size_ * vinfo_->internal_options_->range_num_);
}

} // namespace rocksdb


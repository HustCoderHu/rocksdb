#include <iostream>

#include "util/coding.h"
#include "table/merging_iterator.h"

#include "fixed_range_tab.h"
#include "persistent_chunk.h"
#include "persistent_chunk_iterator.h"
#include "debug.h"

namespace rocksdb {

using std::cout;
using std::endl;
using pmem::obj::persistent_ptr;

/*FixedRangeTab::FixedRangeTab(pool_base &pop, p_range::p_node node, FixedRangeBasedOptions *options)
        :   pop_(pop),
            interal_options_(options) {
    if(!pmap_node_->seq_num_ == 0){
        // new node
        pmap_node_ = node;
        raw_ = pmap_node_->buf->get();
        // set cur_
        EncodeFixed64(raw_, 0);
        // set seq_
        EncodeFixed64(raw_ + sizeof(uint64_t), 0);
        raw_ += 2 * sizeof(uint64_t);
        in_compaction_ = false;
        pmap_node_->inited_ = true;
    }else{
        // rebuild
        RebuildBlkList();
    }

}*/


FixedRangeTab::FixedRangeTab(pool_base &pop, const rocksdb::FixedRangeBasedOptions *options,
                             persistent_ptr<NvRangeTab> &nonVolatileTab)
        : pop_(pop),
          nonVolatileTab_(nonVolatileTab),
          interal_options_(options),
          tab_lock_(false){
    DBG_PRINT("constructor of FixedRangeTab");
    NvRangeTab *raw_tab = nonVolatileTab_.get();
    pendding_clean_ = 0;
    lock_count = 0;
    in_compaction_ = false;
    pendding_compaction_ = false;
    DBG_PRINT("seq_num is %lu", raw_tab->seq_num_.get_ro());
    if (0 == raw_tab->seq_num_.get_ro()) {
        // new node
        raw_ = raw_tab->buf.get();
        // set cur_
        EncodeFixed64(raw_, 0);
        // set seq_
        EncodeFixed64(raw_ + sizeof(uint64_t), 0);
        raw_ += 2 * sizeof(uint64_t);
        //GetProperties();
    } else {
        DBG_PRINT("seq != 0");
        // rebuild
        RebuildBlkList();
        DBG_PRINT("after rebuild");
        GetProperties();
        DBG_PRINT("end rebuild");
    }
}


/* *
 * | prefix data | prefix size |
 * | cur_ | seq_ |
 * | chunk blmFilter | chunk len | chunk data ..| 不定长
 * | chunk blmFilter | chunk len | chunk data ...| 不定长
 * | chunk blmFilter | chunk len | chunk data .| 不定长
 * */

InternalIterator *FixedRangeTab::NewInternalIterator(
        const InternalKeyComparator *icmp, Arena *arena) {
    InternalIterator *internal_iter;
    MergeIteratorBuilder merge_iter_builder(icmp,
                                            arena);

    char* pbuf = nonVolatileTab_->buf.get() + 2 * sizeof(uint64_t);
    // TODO
    // 预设 range 持久化
    //  char *chunkBlkOffset = data_ + sizeof(stat.used_bits_) + sizeof(stat.start_)
    //      + sizeof(stat.end_);
    PersistentChunk pchk;
    for (ChunkBlk &blk : blklist) {
        pchk.reset(blk.bloom_bytes_, blk.chunkLen_, pbuf + blk.getDatOffset());
        merge_iter_builder.AddIterator(pchk.NewIterator(arena));
    }

    internal_iter = merge_iter_builder.Finish();
    return internal_iter;
}

Status FixedRangeTab::Get(const InternalKeyComparator &internal_comparator,
                          const LookupKey &lkey, std::string *value) {
    // 1.从下往上遍历所有的chunk
    PersistentChunkIterator *iter = new PersistentChunkIterator();
    // shared_ptr能够保证资源回收
    char* buf = raw_;
    DBG_PRINT("blklist: size[%lu], pendding_clean[%lu]", blklist.size(), pendding_clean_);
	assert(blklist.size() >= pendding_clean_);
    for (int i = blklist.size() - 1; i >= 0; i--) {
        assert(i >= 0);
        ChunkBlk &blk = blklist.at(i);
        // |--bloom len--|--bloom data--|--chunk len--|--chunk data|
        // ^
        // |
        // bloom data
        char* chunk_head = buf + blk.offset_;
        uint64_t bloom_bytes = blk.bloom_bytes_;
        if (interal_options_->filter_policy_->KeyMayMatch(lkey.user_key(), Slice(chunk_head + 8, bloom_bytes))) {
            // 3.如果有则读取元数据进行chunk内的查找
            new(iter) PersistentChunkIterator(buf + blk.getDatOffset(), blk.chunkLen_, nullptr);
            Status s = searchInChunk(iter, internal_comparator, lkey.user_key(), value);
            if (s.ok()) {
                delete iter;
                DBG_PRINT("found it!");
                return s;
            }
        } else {
            continue;
        }
    } // 4.循环直到查找完所有的chunk
    delete iter;
    DBG_PRINT("Not found");
    return Status::NotFound("not found");
}

/* range data format:
 *
 * |--  cur_  --|
 * |--  seq_  --|
 * |-- chunk1 --|
 * |-- chunk2 --|
 * |--   ...  --|
 *
 * */


/* chunk data format:
 *
 * |--  bloom len  --| // 8bytes
 * |--  bloom data --| // variant
 * |--  chunk len  --| // 8bytes
 * |--  chunk data --| // variant
 * |--    . . .    --|
 *
 * */

Status FixedRangeTab::Append(const InternalKeyComparator &icmp,
                             const string& bloom_data, const Slice &chunk_data,
                             const Slice &start, const Slice &end) {
    DBG_PRINT("start Append");
    if (nonVolatileTab_->dataLen + chunk_data.size_ >= nonVolatileTab_->bufSize
        || nonVolatileTab_->chunk_num_ > max_chunk_num_to_flush()) {
        // TODO：mark tab as pendding compaction
        cout<<"chunk data size : [" << chunk_data.size() / 1048576.0 << "]MB" << endl;
        GetProperties();
        printf("full\n");
    }
    //size_t cur_len = node_in_pmem_map->dataLen;
    size_t chunk_blk_len = bloom_data.size() + chunk_data.size() + 2 * sizeof(uint64_t);
    uint64_t raw_cur = DecodeFixed64(raw_ - 2 * sizeof(uint64_t));
    uint64_t last_seq = DecodeFixed64(raw_ - sizeof(uint64_t));
    char *dst = raw_ + raw_cur; // move to start of this chunk
    // append bloom data
    EncodeFixed64(dst, bloom_data.size()); //+8
    memcpy(dst + sizeof(uint64_t), bloom_data.c_str(), bloom_data.size()); //+bloom data size
    // append chunk data size
    EncodeFixed64(dst + bloom_data.size() + sizeof(uint64_t), chunk_data.size()); //+8

    dst += bloom_data.size() + sizeof(uint64_t) * 2;
    // append data
    memcpy(dst, chunk_data.data(), chunk_data.size()); //+chunk data size
    /*{
    	DBG_PRINT("write bloom size [%lu]", bloom_data.size());
		DBG_PRINT("write chunk size [%lu]", chunk_data.size());
        //debug
        char* debug = raw_ + raw_cur;
        uint64_t chunk_size, bloom_size;
        bloom_size = DecodeFixed64(debug);
        chunk_size = DecodeFixed64(debug + 8 + bloom_size);
        DBG_PRINT("read bloom size [%lu]", bloom_size);
        DBG_PRINT("read chunk size [%lu]", chunk_size);
    }*/

    // update cur and seq
    // transaction

    {
    	if (raw_cur + chunk_blk_len >= max_range_size()) {
			DBG_PRINT("assert: raw_cur[%lu] chunk_blk_len[%lu] max_range_size()[%lu]", raw_cur, chunk_blk_len, max_range_size());
		}
        assert(raw_cur + chunk_blk_len < max_range_size());
        EncodeFixed64(raw_ - 2 * sizeof(uint64_t), raw_cur + chunk_blk_len);
        EncodeFixed64(raw_ - sizeof(uint64_t), last_seq + 1);
    }
    // update meta info

    CheckAndUpdateKeyRange(icmp, start, end);
    // update version
    // transaction
    if (nonVolatileTab_->extra_buf != nullptr) {
        nonVolatileTab_->extra_buf->seq_num_ = nonVolatileTab_->seq_num_ + 1;
        nonVolatileTab_->extra_buf->chunk_num_ = nonVolatileTab_->chunk_num_ + 1;
        nonVolatileTab_->extra_buf->dataLen = nonVolatileTab_->dataLen + chunk_blk_len;
    } else {
        nonVolatileTab_->seq_num_ = nonVolatileTab_->seq_num_ + 1;
        nonVolatileTab_->chunk_num_ = nonVolatileTab_->chunk_num_ + 1;
        nonVolatileTab_->dataLen = nonVolatileTab_->dataLen + chunk_blk_len;
    }

    // record this offset to volatile vector
    blklist.emplace_back(bloom_data.size(), raw_cur, chunk_data.size());
    DBG_PRINT("end Append");

    return Status::OK();
}

void FixedRangeTab::CheckAndUpdateKeyRange(const InternalKeyComparator &icmp, const Slice &new_start,
                                           const Slice &new_end) {
    DBG_PRINT("start update range");
    Slice cur_start, cur_end;
    bool update_start = false, update_end = false;
    GetRealRange(cur_start, cur_end);

    if (cur_start.size() == 0 || icmp.Compare(cur_start, new_start) >= 0) {
        cur_start = new_start;
        update_start = true;
    }

    if (cur_end.size() == 0 || icmp.Compare(cur_end, new_end) <= 0) {
        cur_end = new_end;
        update_end = true;
    }

    if (update_start || update_end) {
        size_t range_data_size = cur_start.size() + cur_end.size() + 2 * sizeof(uint64_t);
        auto UpdateRangeBuf = [&](persistent_ptr<NvRangeTab> p_content){

            auto AllocBufAndUpdate = [&](size_t range_size){
                persistent_ptr<char[]> new_range = nullptr;
                transaction::run(pop_, [&] {
                    new_range = make_persistent<char[]>(range_size);
                    // get raw ptr
                    char *p_new_range = new_range.get();
                    // put start
                    EncodeFixed64(p_new_range, cur_start.size());
                    memcpy(p_new_range + sizeof(uint64_t), cur_start.data(), cur_start.size());
                    // put end
                    p_new_range += sizeof(uint64_t) + cur_start.size();
                    EncodeFixed64(p_new_range, cur_end.size());
                    memcpy(p_new_range + sizeof(uint64_t), cur_end.data(), cur_end.size());

                    // switch old range with new range
                    delete_persistent<char[]>(p_content->key_range_, p_content->rangebufLen);
                    p_content->key_range_ = new_range;
                    p_content->rangebufLen = range_size;
                });
            };

            if(range_data_size > nonVolatileTab_->rangebufLen){
                // 新的range data size大于已有的range buf空间
                AllocBufAndUpdate(range_data_size);
            }else if(range_data_size < (nonVolatileTab_->rangebufLen / 2)){
                // 此时rangebuf大小缩减一半
                AllocBufAndUpdate(nonVolatileTab_->rangebufLen / 2);
            }else{
                // 直接写进去
                char *range_buf = nonVolatileTab_->key_range_.get();
                // put start
                EncodeFixed64(range_buf, cur_start.size());
                memcpy(range_buf + sizeof(uint64_t), cur_start.data(), cur_start.size());
                // put end
                range_buf += sizeof(uint64_t) + cur_start.size();
                EncodeFixed64(range_buf, cur_end.size());
                memcpy(range_buf + sizeof(uint64_t), cur_end.data(), cur_end.size());
            }
        };

        if(nonVolatileTab_->extra_buf == nullptr){
            UpdateRangeBuf(nonVolatileTab_);
        }else{
            UpdateRangeBuf(nonVolatileTab_->extra_buf);
        }

    }
    DBG_PRINT("end update range");

}

void FixedRangeTab::Release() {
    //TODO: release
    // 删除这个range
}

void FixedRangeTab::CleanUp() {
    // 清除这个range的数据
    EncodeFixed64(raw_ - 2 * sizeof(uint64_t), 0);//set cur to 0
    // 清除被compact的chunk
    blklist.erase(blklist.begin(), blklist.begin() + pendding_clean_);
    pendding_clean_ = 0;
    //in_compaction_ = false;

    NvRangeTab *raw_tab = nonVolatileTab_.get();
    if (raw_tab->extra_buf != nullptr) {
        persistent_ptr<NvRangeTab> obsolete_tab = nonVolatileTab_;
        NvRangeTab *vtab = obsolete_tab.get();
        nonVolatileTab_ = nonVolatileTab_->extra_buf;
		nonVolatileTab_->extra_buf = nullptr;		// Clear it!
        transaction::run(pop_, [&] {
            delete_persistent<char[]>(vtab->prefix_, vtab->prefixLen);
            delete_persistent<char[]>(vtab->key_range_, vtab->rangebufLen);
            delete_persistent<char[]>(vtab->buf, vtab->bufSize);
            delete_persistent<NvRangeTab>(obsolete_tab);
        });
    } else {
        transaction::run(pop_, [&] {
            raw_tab->chunk_num_ = 0;
            raw_tab->dataLen = 0;
            Slice start, end;
            GetRealRange(start, end);
            delete_persistent<char[]>(raw_tab->key_range_, start.size() + end.size() + 2 * sizeof(uint64_t));
            raw_tab->key_range_ = nullptr;
        });
    }


}

Status FixedRangeTab::searchInChunk(PersistentChunkIterator *iter, const InternalKeyComparator &icmp,
                                    const Slice &key, std::string *value) {
    int left = 0, right = iter->count() - 1;
    const Comparator* cmp = icmp.user_comparator();
    DBG_PRINT("left[%d]   right[%d]", left, right);
    while (left <= right) {
        int middle = left + ((right - left) >> 1);
        //printf("lest[%d], right[%d], middle[%d]\n", left, right, middle);
        iter->SeekTo(middle);
        const Slice &ml_key = iter->key();
        ParsedInternalKey ikey;
        ParseInternalKey(ml_key, &ikey);
        //DBG_PRINT("ikey[%s] size[%lu] lkey[%s] size[%lu]",ikey.user_key.data(), ikey.user_key.size(),key.data(), key.size());
        int result = cmp->Compare(ikey.user_key, key);
        if (result == 0) {
            //found
            const Slice &raw_value = iter->value();
            value->assign(raw_value.data(), raw_value.size());
            return Status::OK();
        } else if (result < 0) {
            // middle < key
            left = middle + 1;
        } else if (result > 0) {
            // middle >= key
            right = middle - 1;
        }
    }
    return Status::NotFound("not found");
}

Slice FixedRangeTab::GetKVData(char *raw, uint64_t item_off) {
    char *target = raw + item_off;
    uint64_t target_size = DecodeFixed64(target);
    return Slice(target + sizeof(uint64_t), static_cast<size_t>(target_size));
}

void FixedRangeTab::GetRealRange(rocksdb::Slice &real_start, rocksdb::Slice &real_end) {
    if (nonVolatileTab_->key_range_ != nullptr) {
        char *raw = nonVolatileTab_->key_range_.get();
        real_start = GetKVData(raw, 0);
        real_end = GetKVData(raw, real_start.size() + sizeof(uint64_t));
    } else {
        // if there is no key_range return null Slice
        real_start = Slice();
        real_end = Slice();
    }

}

void FixedRangeTab::RebuildBlkList() {
    // TODO :check consistency
    //ConsistencyCheck();
    size_t dataLen;
    dataLen = nonVolatileTab_->dataLen;
    DBG_PRINT("dataLen = %lu", dataLen);
    char* raw_buf = nonVolatileTab_->buf.get();
    // TODO
    // range 从一开始就存 chunk ?
    uint64_t offset = 0;
    auto build_blklist = [&](){
        char* chunk_head = raw_buf + 2 * sizeof(uint64_t);
        while (offset < dataLen) {
            uint64_t bloom_size = DecodeFixed64(chunk_head);
            uint64_t chunk_size = DecodeFixed64(chunk_head + bloom_size + sizeof(uint64_t));
            blklist.emplace_back(bloom_size, offset, chunk_size);
            // next chunk block
            offset += bloom_size + chunk_size + sizeof(uint64_t) * 2;
            //DBG_PRINT("off = %lu, bloom_size = %lu, chunk_size = %lu", offset, bloom_size, chunk_size);
        }
    };

    build_blklist();
    raw_ = raw_buf + 2 * sizeof(uint64_t);

    if(nonVolatileTab_->extra_buf != nullptr){
        pendding_clean_ = blklist.size();
        dataLen = nonVolatileTab_->extra_buf->dataLen;
        offset = 0;
        raw_buf = nonVolatileTab_->extra_buf->buf.get();
        build_blklist();
        raw_ = raw_buf + 2 * sizeof(uint64_t);
    }
}

Usage FixedRangeTab::RangeUsage() {
    Usage usage;
    Slice start, end;
    GetRealRange(start, end);
    usage.range_size = nonVolatileTab_->dataLen;
    usage.chunk_num = nonVolatileTab_->chunk_num_;
    usage.start_ = start;
    usage.end_ = end;
    return usage;
}

void FixedRangeTab::ConsistencyCheck() {
    uint64_t data_seq_num;
    data_seq_num = DecodeFixed64(raw_ - sizeof(uint64_t));
    NvRangeTab *raw_tab = nonVolatileTab_.get();
    if (data_seq_num != raw_tab->seq_num_) {
        // TODO:又需要一个comparator
        /*Slice last_start, last_end;
        GetLastChunkKeyRange(last_start, last_end);*/
    }
}

void FixedRangeTab::SetExtraBuf(persistent_ptr<rocksdb::NvRangeTab> extra_buf) {
    NvRangeTab *vtab = nonVolatileTab_.get();
    vtab->extra_buf = extra_buf;
    extra_buf->seq_num_ = vtab->seq_num_;
    raw_ = extra_buf->buf.get();
	EncodeFixed64(raw_, 0);
    // set seq_
    uint64_t seq_ = DecodeFixed64(vtab->buf.get() + sizeof(uint64_t));
    EncodeFixed64(raw_ + sizeof(uint64_t), seq_);
    raw_ += 2 * sizeof(uint64_t);
}

//#ifdef TAB_DEBUG
void FixedRangeTab::GetProperties() {
    NvRangeTab *vtab = nonVolatileTab_.get();
    uint64_t raw_cur = DecodeFixed64(raw_ - 2 * sizeof(uint64_t));
    uint64_t raw_seq = DecodeFixed64(raw_ - sizeof(uint64_t));
    cout<<"raw_cur [" << raw_cur << "]"<<endl;
    cout<<"raw_seq = [" << raw_seq << "]"<<endl;
    string prefix(vtab->prefix_.get(), vtab->prefixLen.get_ro());
    cout<<"prefix = [" << prefix << "]"<<endl;
    cout<<"capacity = [" << vtab->bufSize / 1048576.0 << "]MB"<<endl;
    Usage usage = RangeUsage();
    cout<<"datalen in vtab = [" << vtab->dataLen << "]"<<endl;
    cout<<"range size = [" << usage.range_size / 1048576.0 << "]MB, chunk_num = ["<< usage.chunk_num <<"]"<<endl;
    if(vtab->key_range_ != nullptr){
        //printf("keyrange = [%s]-[%s]\n", usage.start()->user_key().data(), usage.end()->user_key().data());
        if(usage.start() != nullptr){
            cout << "start_key = [" << usage.start()->user_key().data()<<"]"<<endl;
        }
        if(usage.end() != nullptr){
            cout << "end_key = [" << usage.end()->user_key().data()<<"]"<<endl;
        }
        //cout << "keyrange = [" << usage.start()->user_key().data() << "][" << usage.end()->user_key().data()<<"]"<<endl;
    }
    cout<<endl;
}
//#endif

} // namespace rocksdb

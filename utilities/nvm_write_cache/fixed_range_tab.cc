#include <iostream>
#include "libpmem.h"

#include "util/coding.h"
#include "table/merging_iterator.h"

#include "fixed_range_tab.h"
#include "persistent_chunk.h"
#include "persistent_chunk_iterator.h"
#include "debug.h"
#include "persistent_allocator.h"
//#define FLUSH_CACUL
namespace rocksdb {

using std::cout;
using std::endl;
using pmem::obj::persistent_ptr;

inline void PmemEncodeFixed64(char* buf, uint64_t value) {
    if (port::kLittleEndian) {
        pmem_memcpy_persist(buf, &value, sizeof(value));
    } else {
        buf[0] = value & 0xff;
        buf[1] = (value >> 8) & 0xff;
        buf[2] = (value >> 16) & 0xff;
        buf[3] = (value >> 24) & 0xff;
        buf[4] = (value >> 32) & 0xff;
        buf[5] = (value >> 40) & 0xff;
        buf[6] = (value >> 48) & 0xff;
        buf[7] = (value >> 56) & 0xff;
        pmem_persist(buf, 8);
    }
}

char* FixedRangeTab::base_raw_ = nullptr;

char* FixedRangeTab::get_raw(rocksdb::NvRangeTab *tab) {
    return base_raw_ + tab->buf_size_ * tab->offset_;
}

/*char* FixedRangeTab::get_raw(rocksdb::NvRangeTab *tab) {
    return tab->buf_.get();
}*/

FixedRangeTab::FixedRangeTab(pool_base &pop, const FixedRangeBasedOptions *options,
                             const InternalKeyComparator* icmp,
                             persistent_ptr<NvRangeTab> &wbuffer)
        : pop_(pop),
          w_buffer_(wbuffer),
          interal_options_(options),
          icmp_(icmp),
          tab_lock_(false) {
    DBG_PRINT("constructor of FixedRangeTab");
    assert(wbuffer->pair_buf_ != nullptr);
    NvRangeTab *raw_tab = wbuffer.get();
    compaction_working_ = false;
    compaction_pendding_ = false;
    DBG_PRINT("seq_num is %lu", raw_tab->seq_num_.get_ro());
    if (0 == raw_tab->seq_num_.get_ro()) {
        // new node
        //raw_ = raw_tab->raw_;
        //raw_ = base_raw_ + raw_tab->offset_ * raw_tab->buf_size_;
        raw_ = get_raw(raw_tab);
        // set cur_
        PmemEncodeFixed64(raw_, 0);
        // set seq_
        PmemEncodeFixed64(raw_ + sizeof(uint64_t), 0);
        raw_ += 2 * sizeof(uint64_t);
    } else {
        // rebuild
        DBG_PRINT("seq != 0");
        RebuildBlkList();
        GetProperties();
    }
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

Status FixedRangeTab::Append(const string &bloom_data, const Slice &chunk_data,
                             const Slice &start, const Slice &end) {
    DBG_PRINT("start Append bloom size[%lu] chunk size[%lu]", bloom_data.size(), chunk_data.size());
    assert(w_buffer_->data_len_ + chunk_data.size_ + 2 * 8 <= max_range_size());
    size_t chunk_blk_len = bloom_data.size() + chunk_data.size() + 2 * sizeof(uint64_t);
    uint64_t raw_cur = DecodeFixed64(raw_ - 2 * sizeof(uint64_t));
    uint64_t last_seq = DecodeFixed64(raw_ - sizeof(uint64_t));
    //DBG_PRINT("raw cur[%lu]-datalen[%lu]", raw_cur, w_buffer_->data_len_);
    assert(raw_cur == w_buffer_->data_len_);
    char *dst = raw_ + raw_cur; // move to start of this chunk
    // append bloom data
#ifdef FLUSH_CACUL
    uint64_t start_time = Env::Default()->NowMicros();
#endif

    PmemEncodeFixed64(dst, bloom_data.size()); //+8
    pmem_memcpy_nodrain(dst + sizeof(uint64_t), bloom_data.c_str(), bloom_data.size()); //+bloom data size
    // append chunk data size
    PmemEncodeFixed64(dst + bloom_data.size() + sizeof(uint64_t), chunk_data.size()); //+8

    dst += bloom_data.size() + sizeof(uint64_t) * 2;
    // append data
    pmem_memcpy_nodrain(dst, chunk_data.data(), chunk_data.size()); //+chunk data size
    pmem_drain();

#ifdef FLUSH_CACUL
    uint64_t write_end_time = Env::Default()->NowMicros();
#endif
    /*{
    	//DBG_PRINT("write bloom size [%lu]", bloom_data.size());
		//DBG_PRINT("write chunk size [%lu]", chunk_data.size());
        //debug
        char* debug = raw_ + raw_cur + chunk_blk_len;
        uint64_t kv_item_num;
        kv_item_num = DecodeFixed64(debug - 8);
        //DBG_PRINT("appended chunk include items[%lu]", kv_item_num);
        //DBG_PRINT("decode from raw chunk data[%lu] chunkdata size[%lu]", DecodeFixed64(chunk_data.data() + chunk_data.size() - 8), chunk_data.size());
        //chunk_size = DecodeFixed64(debug + 8 + bloom_size);
        //("read bloom size [%lu]", bloom_size);
        //DBG_PRINT("read chunk size [%lu]", chunk_size);
    }*/
    // update cur and seq
    // transaction
    {
        if (raw_cur + chunk_blk_len >= max_range_size()) {
            DBG_PRINT("prefix [%s]", string(w_buffer_->prefix_.get(), w_buffer_->prefix_len_).c_str());
            DBG_PRINT("assert: buffer[%lu] raw_cur[%lu] data_len_[%lu]", w_buffer_->offset_, raw_cur,
                    w_buffer_->data_len_, max_range_size());
            DBG_PRINT("assert: chunk_blk_len[%lu] max_range_size()[%lu]",chunk_blk_len,max_range_size());
            assert(false);
        }
        // TODO : transaction1
        PmemEncodeFixed64(raw_ - 2 * sizeof(uint64_t), raw_cur + chunk_blk_len);
        PmemEncodeFixed64(raw_ - sizeof(uint64_t), last_seq + 1);
    }
    // update meta info

    CheckAndUpdateKeyRange(start, end);
    // update version
    // transaction
    // TODO : transaction2
    w_buffer_->seq_num_ = w_buffer_->seq_num_ + 1;
    w_buffer_->chunk_num_ = w_buffer_->chunk_num_ + 1;
    w_buffer_->data_len_ = w_buffer_->data_len_ + chunk_blk_len;

#ifdef FLUSH_CACUL
    uint64_t meta_end_time = Env::Default()->NowMicros();
    FILE *fp = fopen("write_time", "a");
    fprintf(fp, "flush %lu bytes spent %lu time write + %lu time meta\n", bloom_data.size() + chunk_data.size() + 16,
            write_end_time - start_time, meta_end_time - write_end_time);
    fclose(fp);
#endif


    // record this offset to volatile vector
    wblklist_.emplace_back(bloom_data.size(), raw_cur, chunk_data.size());
    //DBG_PRINT("end Append");

    return Status::OK();
}

bool FixedRangeTab::Get(Status *s,
                        const LookupKey &lkey, std::string *value){
    // 1.从下往上遍历所有的chunk
    auto *iter = new PersistentChunkIterator();
    // shared_ptr能够保证资源回收
    DBG_PRINT("wblklist: size[%lu]", wblklist_.size());
    bool result = false;
    char* buf = get_raw(w_buffer_.get()) + 2 * sizeof(uint64_t);
    result = SearchBlockList(buf, wblklist_, s, iter, lkey, value);
    if (!result && !cblklist_.empty()) {
        DBG_PRINT("search cblklist[%lu]", cblklist_.size());
        buf = get_raw(c_buffer_.get()) + 2 * sizeof(uint64_t);
        result = SearchBlockList(buf, cblklist_, s, iter, lkey, value);
    }
    delete iter;
    return result;
}

bool FixedRangeTab::SearchBlockList(char* buf, vector<rocksdb::ChunkBlk> &blklist, Status *s,
        PersistentChunkIterator* iter, const LookupKey& lkey, std::string *value){
    for (int i = blklist.size() - 1; i >= 0; i--) {
        assert(i >= 0);
        ChunkBlk &blk = blklist.at(i);
        // |--bloom len--|--bloom data--|--chunk len--|--chunk data|
        // ^
        // |
        // bloom data
        char *chunk_head = buf + blk.offset_;
        uint64_t bloom_bytes = blk.bloom_bytes_;
        if (interal_options_->filter_policy_->KeyMayMatch(lkey.user_key(), Slice(chunk_head + 8, bloom_bytes))) {
            // 3.如果有则读取元数据进行chunk内的查找
            DBG_PRINT("Key in chunk and search");
            new(iter) PersistentChunkIterator(buf + blk.getDatOffset(), blk.chunkLen_, nullptr);
            Status result = searchInChunk(iter, lkey.user_key(), value);
            if (result.ok()) {
                *s = Status::OK();
                DBG_PRINT("found it!");
                return true;
            }
        } else {
            DBG_PRINT("filtered by bloom");
            continue;
        }
    } // 4.循环直到查找完所有的chunk
    return false;
}

/* *
 * | prefix data | prefix size |
 * | cur_ | seq_ |
 * | chunk blmFilter | chunk len | chunk data ..| 不定长
 * | chunk blmFilter | chunk len | chunk data ...| 不定长
 * | chunk blmFilter | chunk len | chunk data .| 不定长
 * */

InternalIterator *FixedRangeTab::NewInternalIterator(Arena *arena, bool for_comapction) {
    //DBG_PRINT("In NewIterator");
    int num = 0;
    PersistentChunk pchk;
    InternalIterator **list;
    if (for_comapction) {
        // Iterator for compaction
        list = new InternalIterator *[cblklist_.size()];
    } else {
        // Iterator for total iterate
        list = new InternalIterator *[wblklist_.size() + cblklist_.size()];
        char *rbuf = get_raw(w_buffer_.get()) + 2 * sizeof(uint64_t);
        // add chunk in wbuffer to iterator
        for (auto chunk : wblklist_) {
            pchk.reset(chunk.bloom_bytes_, chunk.chunkLen_, rbuf + chunk.getDatOffset());
            list[num++] = pchk.NewIterator(arena);
        }
    }
    // add chunk in cbuffer to iterator
    char *rbuf = get_raw(c_buffer_.get()) + 2 * sizeof(uint64_t);
    for (auto chunk : cblklist_) {
        pchk.reset(chunk.bloom_bytes_, chunk.chunkLen_, rbuf + chunk.getDatOffset());
        list[num++] = pchk.NewIterator(arena);
    }
    InternalIterator *result = NewMergingIterator(icmp_, list, num, arena, false);
    delete[] list;
    //DBG_PRINT("End Newiterator");
    return result;
}


void FixedRangeTab::CheckAndUpdateKeyRange(const Slice &new_start, const Slice &new_end) {
    //DBG_PRINT("start update range");
    Slice cur_start, cur_end;
    bool update_start = false, update_end = false;
    GetRealRange(w_buffer_.get(), cur_start, cur_end);
    //DBG_PRINT("compare start");
    if (cur_start.size() == 0 || icmp_->Compare(cur_start, new_start) >= 0) {
        cur_start = new_start;
        update_start = true;
    }
    //DBG_PRINT("compare end");
    if (cur_end.size() == 0 || icmp_->Compare(cur_end, new_end) <= 0) {
        cur_end = new_end;
        update_end = true;
    }

    if (update_start || update_end) {
        size_t range_data_size = cur_start.size() + cur_end.size() + 2 * sizeof(uint64_t);
        auto AllocBufAndUpdate = [&](size_t range_size) {
            persistent_ptr<char[]> new_range = nullptr;
            transaction::run(pop_, [&] {
                new_range = make_persistent<char[]>(range_size);
                // get raw ptr
                char *p_new_range = new_range.get();
                // update range data size
                PmemEncodeFixed64(p_new_range, cur_start.size() + cur_end.size() + 2 * sizeof(uint64_t));
                p_new_range += sizeof(uint64_t);
                // put start
                EncodeFixed64(p_new_range, cur_start.size());
                pmem_memcpy_persist(p_new_range + sizeof(uint64_t), cur_start.data(), cur_start.size());
                // put end
                p_new_range += sizeof(uint64_t) + cur_start.size();
                EncodeFixed64(p_new_range, cur_end.size());
                pmem_memcpy_persist(p_new_range + sizeof(uint64_t), cur_end.data(), cur_end.size());

                // switch old range with new range
                delete_persistent<char[]>(w_buffer_->key_range_, w_buffer_->range_buf_len_);
                w_buffer_->key_range_ = new_range;
                w_buffer_->range_buf_len_ = range_size;
            });
        };

        if (range_data_size > w_buffer_->range_buf_len_) {
            // 新的range data size大于已有的range buf空间
            AllocBufAndUpdate(range_data_size);
            printf("large range data\n");
        } else if (range_data_size < (w_buffer_->range_buf_len_ / 2) && w_buffer_->range_buf_len_ > 200) {
            // 此时rangebuf大小缩减一半
            AllocBufAndUpdate(w_buffer_->range_buf_len_ / 2);
            printf("shrink range size\n");
        } else {
            // 直接写进去
            /*{
                InternalKey s,e;
                s.DecodeFrom(cur_start);
                e.DecodeFrom(cur_end);
                DBG_PRINT("put key range[%s]-[%s]",s.DebugString(true).c_str(), e.DebugString(true).c_str());
            }*/

            char *range_buf = w_buffer_->key_range_.get();
            // put range data size
            PmemEncodeFixed64(range_buf, cur_start.size() + cur_end.size() + 2 * sizeof(uint64_t));
            range_buf += sizeof(uint64_t);
            // put start
            PmemEncodeFixed64(range_buf, cur_start.size());
            pmem_memcpy_nodrain(range_buf + sizeof(uint64_t), cur_start.data(), cur_start.size());
            // put end
            range_buf += sizeof(uint64_t) + cur_start.size();
            PmemEncodeFixed64(range_buf, cur_end.size());
            pmem_memcpy_nodrain(range_buf + sizeof(uint64_t), cur_end.data(), cur_end.size());
            pmem_drain();
            /*{
                InternalKey s,e;
                GetRealRange(w_buffer_.get(), cur_start, cur_end);
                s.DecodeFrom(cur_start);
                e.DecodeFrom(cur_end);
                DBG_PRINT("re-read key range[%s]-[%s]",s.DebugString(true).c_str(), e.DebugString(true).c_str());
            }*/
        }
    }
    //DBG_PRINT("end update range");
}

void FixedRangeTab::Release() {
    //TODO: release
    // 删除这个range
}

void FixedRangeTab::CleanUp(NvRangeTab* tab) {
    // 清除这个range的数据
    // 清除被compact的chunk
    tab->data_len_ = 0;
    tab->chunk_num_ = 0;
    pmem_memset_persist(tab->key_range_.get(), 0, tab->range_buf_len_);
    PmemEncodeFixed64(get_raw(tab), 0);
    DBG_PRINT("clear bufsize[%lu] off[%lu] cur[%lu]", tab->buf_size_, tab->offset_, DecodeFixed64(get_raw(tab)));
}

Status FixedRangeTab::searchInChunk(PersistentChunkIterator *iter,
                                    const Slice &key, std::string *value) const{
    int left = 0, right = iter->count() - 1;
    const Comparator *cmp = icmp_->user_comparator();
    //DBG_PRINT("left[%d]   right[%d]", left, right);
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

Slice FixedRangeTab::GetKVData(char *raw, uint64_t item_off) const{
    char *target = raw + item_off;
    uint64_t target_size = DecodeFixed64(target);
    return Slice(target + sizeof(uint64_t), static_cast<size_t>(target_size));
}

void FixedRangeTab::GetRealRange(NvRangeTab *tab, Slice &real_start, Slice &real_end) const{
    char *raw = tab->key_range_.get();
    uint64_t real_size = DecodeFixed64(raw);
    if (real_size != 0) {
        raw += sizeof(uint64_t);
        real_start = GetKVData(raw, 0);
        real_end = GetKVData(raw, real_start.size() + sizeof(uint64_t));
        /*{
            InternalKey s,e;
            s.DecodeFrom(real_start);
            e.DecodeFrom(real_end);
            DBG_PRINT("key range[%s]-[%s] size[%lu]",s.DebugString(true).c_str(), e.DebugString(true).c_str(), real_size);
        }*/
    } else {
        // if there is no key_range return null Slice
        real_start = Slice();
        real_end = Slice();
    }

}

void FixedRangeTab::RebuildBlkList() {
    // TODO :check consistency
    //ConsistencyCheck();
    // TODO : 确保传进来的是wbuffer
    auto build_blklist = [](NvRangeTab* tab, vector<ChunkBlk>& blklist) {
        char *chunk_head = get_raw(tab) + 2 * sizeof(uint64_t);
        size_t data_len = tab->data_len_;
        uint64_t offset = 0;
        DBG_PRINT("dataLen = %lu", data_len);
        while (offset < data_len) {
            uint64_t bloom_size = DecodeFixed64(chunk_head);
            uint64_t chunk_size = DecodeFixed64(chunk_head + bloom_size + sizeof(uint64_t));
            blklist.emplace_back(bloom_size, offset, chunk_size);
            // next chunk block
            offset += bloom_size + chunk_size + sizeof(uint64_t) * 2;
            chunk_head += bloom_size + chunk_size + sizeof(uint64_t) * 2;
            DBG_PRINT("off = %lu, bloom_size = %lu, chunk_size = %lu", offset, bloom_size, chunk_size);
        }

    };

    build_blklist(w_buffer_.get(), wblklist_);
    if(w_buffer_->pair_buf_->chunk_num_ != 0){
        DBG_PRINT("recover cbuffer");
        build_blklist(w_buffer_->pair_buf_.get(), cblklist_);
        c_buffer_ = w_buffer_->pair_buf_;
    }

    //raw_ = w_buffer_->raw_ + 2 * sizeof(uint64_t);
    raw_ = get_raw(w_buffer_.get())+ 2 * sizeof(uint64_t);
    DBG_PRINT("wchunk[%lu][%d] cchunk[%lu][%d]", w_buffer_->chunk_num_, w_buffer_->writting_, w_buffer_->pair_buf_->chunk_num_, w_buffer_->pair_buf_->writting_);
}

Usage FixedRangeTab::RangeUsage(UsageType type) const{
    Usage usage;
    Slice start, end;
    auto get_usage = [&](NvRangeTab* tab){
        if(tab != nullptr){
            GetRealRange(tab, start, end);
            usage.range_size = tab->data_len_;
            usage.chunk_num = tab->chunk_num_;
            usage.start_ = start;
            usage.end_ = end;
            if(usage.chunk_num == 0) usage.type = kEmptyUsage;
        }else{
            usage.type = kEmptyUsage;
        }
    };
    switch (type){
        case kForTotal:{
            get_usage(w_buffer_.get());
            Usage wusage = usage;
            get_usage(c_buffer_.get());
            Usage cusage = usage;
            if(wusage.type == kEmptyUsage){
                return cusage;
            }else if(cusage.type == kEmptyUsage){
                return wusage;
            }else{
                wusage.chunk_num += cusage.chunk_num;
                wusage.range_size += cusage.range_size;
                if (icmp_->Compare(wusage.start_, cusage.start_) > 0) {
                    wusage.start_ = cusage.start_;
                }
                if (icmp_->Compare(wusage.end_, cusage.end_) < 0) {
                    wusage.end_ = cusage.end_;
                }
                return wusage;
            }
        }

        case kForCompaction:{
            get_usage(c_buffer_.get());
            //DBG_PRINT("get range[%s]-[%s]", usage.start().DebugString(true).c_str(), usage.end().DebugString(true).c_str());
            return usage;
        }
        case kForWritting:{
            get_usage(w_buffer_.get());
            return usage;
        }

        default:
            assert(usage.type == kEmptyUsage);
            return usage;
    }
}

void FixedRangeTab::ConsistencyCheck() {
    /*uint64_t data_seq_num;
    data_seq_num = DecodeFixed64(raw_ - sizeof(uint64_t));
    NvRangeTab *raw_tab = nonVolatileTab_.get();
    if (data_seq_num != raw_tab->seq_num_) {
        // TODO:又需要一个comparator
        *//*Slice last_start, last_end;
        GetLastChunkKeyRange(last_start, last_end);*//*
    }*/
}

/*void FixedRangeTab::SetExtraBuf(persistent_ptr<rocksdb::NvRangeTab> extra_buf) {
    NvRangeTab *vtab = nonVolatileTab_.get();
    DBG_PRINT("set extra buf for[%s]", string(vtab->prefix_.get(), vtab->prefixLen).c_str());
    vtab->extra_buf = extra_buf;
    extra_buf->seq_num_ = vtab->seq_num_;
    raw_ = extra_buf->raw_;
	EncodeFixed64(raw_, 0);
    // set seq_
    uint64_t seq_ = DecodeFixed64(vtab->raw_ + sizeof(uint64_t));
    EncodeFixed64(raw_ + sizeof(uint64_t), seq_);
    raw_ += 2 * sizeof(uint64_t);
}*/

void FixedRangeTab::GetProperties() const{
    NvRangeTab *vtab = w_buffer_.get();
    uint64_t raw_cur = DecodeFixed64(raw_ - 2 * sizeof(uint64_t));
    uint64_t raw_seq = DecodeFixed64(raw_ - sizeof(uint64_t));
    cout << "raw_cur [" << raw_cur << "]" << endl;
    cout << "raw_seq = [" << raw_seq << "]" << endl;
    string prefix(vtab->prefix_.get(), vtab->prefix_len_.get_ro());
    cout << "prefix = [" << prefix << "]" << endl;
    cout << "capacity = [" << vtab->buf_size_ / 1048576.0 << "]MB" << endl;
    // 传入comparator
    Usage usage = RangeUsage(kForTotal);
    cout << "datalen in vtab = [" << vtab->data_len_ << "]" << endl;
    cout << "range size = [" << usage.range_size / 1048576.0 << "]MB, chunk_num = [" << usage.chunk_num << "]" << endl;
    if(vtab->pair_buf_->data_len_ != 0){
        cout << "datalen in vtab = [" << vtab->pair_buf_->data_len_ << "]" << endl;
        cout << "range size = [" << vtab->pair_buf_->data_len_ / 1048576.0 << "]MB, chunk_num = [" << vtab->pair_buf_->chunk_num_ << "]" << endl;
    }
    /*if (vtab->key_range_ != nullptr) {
        if (!usage.start_.empty()) {
            cout << "start_key = [" << usage.start().user_key().data() << "]" << endl;
        }
        if (!usage.end_.empty()) {
            cout << "end_key = [" << usage.end().user_key().data() << "]" << endl;
        }
    }*/
    cout << endl;
}

void FixedRangeTab::SwitchBuffer(SwitchDirection direction) {
    switch(direction){
        case kToWBuffer:
            assert(c_buffer_->writting_ == false);
            DBG_PRINT("[%lu]switch to wbuffer", c_buffer_->offset_);
            CleanUp(c_buffer_.get());
            cblklist_.clear();
            c_buffer_ = nullptr;
            break;

        case kToCBuffer:
            // 将当前w_buffer切换为c_buffer
            assert(c_buffer_ == nullptr);
            DBG_PRINT("[%lu]switch to cbuffer", w_buffer_->offset_);
            c_buffer_ = w_buffer_;
            // c_buffer的writting为false
            c_buffer_->writting_=false;
            w_buffer_ = w_buffer_->pair_buf_;
            //DBG_PRINT("new wbuffer [%lu] cur [%f]",w_buffer_->offset_,  DecodeFixed64(base_raw_+w_buffer_->buf_size_*w_buffer_->offset_) / 1048576.0);
            w_buffer_->writting_ = true;
            // 更新seq
            PmemEncodeFixed64(get_raw(w_buffer_.get()) + sizeof(uint64_t),
                    DecodeFixed64(get_raw(c_buffer_.get()) + sizeof(uint64_t)));
            // 设置raw指针
            raw_ = get_raw(w_buffer_.get()) + 2 * sizeof(uint64_t);
            //DBG_PRINT("raw switch form [%p ]to [%p]",base_raw_+c_buffer_->buf_size_*c_buffer_->offset_,  raw_);
            // 交换blklist的数据
            cblklist_.swap(wblklist_);
            wblklist_.clear();
            break;

        default:
            break;
    }
}

} // namespace rocksdb

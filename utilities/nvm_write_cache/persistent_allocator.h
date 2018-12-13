//
// Created by 张艺文 on 2018/12/8.
//

#pragma once

#include "persistent_bitmap.h"
#include "debug.h"
namespace rocksdb{

using pmem::obj::p;
using pmem::obj::persistent_ptr;
using pmem::obj::transaction;
using pmem::obj::pool;
using pmem::obj::pool_base;
using pmem::obj::make_persistent;
using pmem::obj::delete_persistent;

class PersistentAllocator {
public:
    explicit PersistentAllocator(pool_base& pop, persistent_ptr<char[]> raw_space,
                                 uint64_t total_size, uint64_t range_size,
                                 persistent_ptr<PersistentBitMap> bitmap):pop_(pop) {
        bitmap_ = bitmap;
        raw_ = raw_space;
        total_size_ = total_size;
        range_size_ = range_size;
        cur_ = 0;
    }

    ~PersistentAllocator(){
        /*transaction::run(pop_, [&]{
            delete_persistent<char[]>(raw_, total_size_);
        });*/
    };

    char *Allocate(int &offset) {
        assert(Remain() > range_size_);
        offset = bitmap_->GetBit();
        char *alloc = nullptr;
        if(offset != -1){
            alloc = raw_.get() + offset * range_size_;
            cur_ = cur_ + 1;
            bitmap_->SetBit(offset, true);
        }
        return alloc;
    }

    uint64_t Remain() {
        return total_size_ - cur_ * range_size_;
    }

    uint64_t Capacity() {
        return total_size_;
    }

    void Reset(){
        bitmap_->Reset();
        cur_ = 0;
    }

    void Release(){
        transaction::run(pop_, [&]{
            delete_persistent<char[]>(raw_, total_size_);
        });
    }

    void Free(int offset){
        bitmap_->SetBit(offset, false);
        cur_ = cur_ - 1;
    }

    persistent_ptr<char[]> raw(){return raw_;}


private:
    pool_base& pop_;
    persistent_ptr<PersistentBitMap> bitmap_;
    persistent_ptr<char[]> raw_;
    p<uint64_t> total_size_;
    p<uint64_t > range_size_;
    p<uint64_t> cur_;

};

class BlockBasedPersistentAllocator{
public:
    explicit BlockBasedPersistentAllocator(pool_base& pop,
            uint64_t total_size, uint64_t range_size):pop_(pop) {

        range_num_ = ceil(total_size / range_size);
        transaction::run(pop, [&]{
            //初始化bitmap
            bitmap_ = make_persistent<PersistentBitMap>(pop, range_num_);
            buf_array_ = make_persistent<p_buf[]>(range_num_);
            for(size_t i = 0; i < range_num_; i++){
                //分配所有的range空间
                DBG_PRINT("Alloc [%lu]th buf", i);;
                buf_array_[i] = make_persistent<char[]>(range_size);
            }

        });
        total_size_ = total_size;
        range_size_ = range_size;
        allocated_ = 0;
    }

    ~BlockBasedPersistentAllocator(){
        /*transaction::run(pop_, [&]{
            delete_persistent<char[]>(raw_, total_size_);
        });*/
    };

    p_buf Allocate(int &offset) {
        if(Remain() == 0){
            DBG_PRINT("space run out");
            return nullptr;
        }
        // 从bitmap中获取一位
        offset = bitmap_->GetBit();
        char *alloc = nullptr;
        if(offset != -1 && offset < static_cast<int>(range_num_)){
            //alloc = raw_.get() + offset * range_size_;
            // 标记分配状态
            allocated_ = allocated_ + 1;
            bitmap_->SetBit(offset, true);
        }
        return buf_array_[offset];
    }

    uint64_t Remain() {
        assert(range_num_ >= allocated_);
        return range_num_ - allocated_;
    }

    uint64_t Capacity() {
        return total_size_;
    }

    void Reset(){
        bitmap_->Reset();
        allocated_ = 0;
    }

    void Release(){
        transaction::run(pop_, [&]{
            for(size_t i = 0; i < range_num_; i++){
                delete_persistent<char[]>(buf_array_[i], range_size_);
            }
            delete_persistent<p_buf[]>(buf_array_, range_size_);
        });
    }

    void Free(int offset){
        bitmap_->SetBit(offset, false);
        allocated_ = allocated_ - 1;
    }


private:
    pool_base& pop_;
    persistent_ptr<PersistentBitMap> bitmap_;
    persistent_ptr<p_buf[]> buf_array_;
    p<uint64_t> total_size_;
    p<uint64_t > range_size_;
    p<uint64_t> range_num_;
    p<uint64_t> allocated_;
};
}

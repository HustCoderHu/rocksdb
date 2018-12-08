//
// Created by ������ on 2018/12/8.
//

#pragma once
#include "libpmemobj++/p.hpp"
#include "libpmemobj++/persistent_ptr.hpp"
#include "libpmemobj++/pool.hpp"
#include "libpmemobj++/transaction.hpp"
#include "libpmemobj++/make_persistent.hpp"
#include "libpmemobj++/make_persistent_array.hpp"

#include "persistent_bitmap.h"
namespace rocksdb{
class PersistentAllocator {
public:
    explicit PersistentAllocator(pool_base& pop, persistent_ptr<char[]> raw_space,
                                 uint64_t total_size, uint64_t range_size,
                                 persistent_ptr<PersistentBitMap> bitmap):pop_(pop) {
        bitmap_ = bitmap;
        raw_ = raw_space;
        total_size_ = total_size;
        cur_ = 0;
    }

    ~PersistentAllocator(){
        transaction::run(pop_, [&]{
            delete_persistent(raw_, total_size_);
        });
    };

    char *Allocate(int &offset) {
        assert(Remain() > range_size_);
        offset = bitmap_->GetBit();
        char *alloc = nullptr;
        if(offset != -1){
            alloc = raw_.get() + offset * range_size_;
            cur_ = cur_ + 1;
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

    void Free(int offset){
        bitmap_->DelBit(offset);
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
}
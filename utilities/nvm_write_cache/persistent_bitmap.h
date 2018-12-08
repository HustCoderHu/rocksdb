//
// Created by уерунд on 2018/12/8.
//
#pragma once
#include <cstdio>

#include "libpmemobj++/p.hpp"
#include "libpmemobj++/persistent_ptr.hpp"
#include "libpmemobj++/pool.hpp"
#include "libpmemobj++/transaction.hpp"
#include "libpmemobj++/make_persistent.hpp"
#include "libpmemobj++/make_persistent_array.hpp"

namespace rocksdb{
using pmem::obj::p;
using pmem::obj::persistent_ptr;
using pmem::obj::transaction;
using pmem::obj::pool;
using pmem::obj::pool_base;
using pmem::obj::make_persistent;
using pmem::obj::delete_persistent;

class PersistentBitMap{
public:
    PersistentBitMap(pool_base &pop, size_t maplen);

    ~PersistentBitMap();

    int GetBit();

    bool SetBit(size_t pos, bool flag);

    bool GetAndSet();

    void Reset();

    void Print();

private:
    pool_base& pop_;
    p<size_t> len_;
    p<size_t> bytes_;
    persistent_ptr<unsigned char[]> bitmap_;
    //unsigned char* bitmap_;
};
}

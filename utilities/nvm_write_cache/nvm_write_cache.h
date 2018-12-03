//
// Created by 张艺文 on 2018/11/2.
//

#pragma once


#include <unordered_map>
#include <vector>
#include "rocksdb/iterator.h"
#include "table/internal_iterator.h"


namespace rocksdb {

class InternalKeyComparator;
class Arena;

class NVMWriteCache {
public:
    NVMWriteCache() = default;

    virtual ~NVMWriteCache() = default;


    virtual Status Get(const InternalKeyComparator &internal_comparator, const LookupKey &lkey, std::string *value) = 0;

    virtual InternalIterator *NewIterator(const InternalKeyComparator *icmp, Arena *arena) = 0;

    virtual bool NeedCompaction() = 0;

};

} // end rocksdb

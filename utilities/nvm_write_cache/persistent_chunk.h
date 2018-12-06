#ifndef PERSISTENT_CHUNK_H
#define PERSISTENT_CHUNK_H

#include <vector>

#include "rocksdb/iterator.h"
#include "table/merging_iterator.h"
#include "util/arena.h"

#include "libpmemobj++/persistent_ptr.hpp"

#include "persistent_chunk_iterator.h"

namespace rocksdb {
using std::vector;
using pmem::obj::persistent_ptr;
// interface ref  class MemTable
class PersistentChunk {
public:
    PersistentChunk(size_t bloomFilterSize, size_t chunkSize,
                    char* chunkData)
            : bloomFilterSize_(bloomFilterSize), chunkSize_(chunkSize),
              chunkData_(chunkData) {
    }

    PersistentChunk() = default;

    InternalIterator *NewIterator(Arena *arena) {
        //assert(arena != nullptr);
        if(arena != nullptr){
            auto mem = arena->AllocateAligned(sizeof(PersistentChunkIterator));
            return new(mem) PersistentChunkIterator(chunkData_, chunkSize_, arena);
        }else{
            return new PersistentChunkIterator(chunkData_, chunkSize_, nullptr);
        }

    }

    void reset(size_t bloomFilterSize, size_t chunkSize,
               char* chunkData) {
        bloomFilterSize_ = bloomFilterSize;
        chunkSize_ = chunkSize;
        chunkData_ = chunkData;
    }

//  static PersistentChunk* parseFromRaw(const Slice& slc);

    size_t bloomFilterSize_;
    size_t chunkSize_;
    char* chunkData_;
};
} // namespace rocksdb
#endif // PERSISTENT_CHUNK_H

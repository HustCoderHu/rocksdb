#include "persistent_chunk_iterator.h"

namespace rocksdb {

PersistentChunkIterator::PersistentChunkIterator(p_buf data,
                                                 size_t size, Arena *arena)
        : data_(data), arena_(arena) {
    // keysize | key | valsize | val | ... | 1st pair offset
    // | 2nd pair offset | ... | num of pairs
    uint64_t nPairs;
    size_t sizeof_uint64_t = sizeof(uint64_t);

    char* raw_buf = data.get();
    //p_buf nPairsOffset = data + size - sizeof(nPairs);
    char* n_pairs_off = raw_buf + size - sizeof_uint64_t;
    nPairs = DecodeFixed64(n_pairs_off);

//  nPairs = *(reinterpret_cast<size_t*>(nPairsOffset));
    vKey_.reserve(nPairs);
    vValue_.reserve(nPairs);

    char*  metaOffset = n_pairs_off - sizeof_uint64_t * nPairs;// 0 first

    for (size_t i = 0; i < nPairs; ++i) {
        uint64_t pairOffset = DecodeFixed64(metaOffset);

        //memcpy(&pairOffset, metaOffset, sizeof(pairOffset));
//    *(reinterpret_cast<size_t*>(metaOffset));
        //p_buf pairAddr = data + pairOffset;
        char* pairAddr = raw_buf + pairOffset;

        // key size
        uint64_t _size = DecodeFixed64(pairAddr);
        // key
        vKey_.emplace_back(pairAddr + sizeof_uint64_t, _size);
//    size_t _size = *(reinterpret_cast<size_t*>(pairAddr));
//    vKey_.emplace_back(pairAddr + sizeof(_size), _size);


        pairAddr += sizeof_uint64_t + _size;
        // value size
        _size = DecodeFixed64(pairAddr);
        // value
        vValue_.emplace_back(pairAddr + sizeof_uint64_t, _size);
//    _size = *(reinterpret_cast<size_t*>(pairAddr));
//    vValue_.emplace_back(pairAddr + sizeof(_size), _size);

        // next pair
        metaOffset += sizeof_uint64_t;
    }
}

} // namespace rocksdb


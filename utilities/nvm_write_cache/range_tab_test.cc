//
// Created by 张艺文 on 2018/11/26.
//
#include <string>
#include <util/testharness.h>
#include <unistd.h>

#include "third-party/gtest-1.7.0/fused-src/gtest/gtest.h"
#include "util/testutil.h"
#include "util/random.h"

#include "utilities/nvm_write_cache/skiplist/test_common.h"
#include "fixed_range_tab.h"
#include "chunk.h"
#include "fixed_range_chunk_based_nvm_write_cache.h"
#include "common.h"

#define TAB_DEBUG

namespace rocksdb {

struct pRangeRoot {
    p<bool> inited;
    persistent_ptr<NvRangeTab> p_content;
};

enum WriteMode {
    RANDOM, SEQUENTIAL, UNIQUE_RANDOM
};

class KeyGenerator {
public:
    KeyGenerator(Random64 *rand, WriteMode mode, uint64_t num,
                 uint64_t /*num_per_set*/ = 64 * 1024)
            : rand_(rand), mode_(mode), num_(num), next_(0) {
        if (mode_ == UNIQUE_RANDOM) {
            // NOTE: if memory consumption of this approach becomes a concern,
            // we can either break it into pieces and only random shuffle a section
            // each time. Alternatively, use a bit map implementation
            // (https://reviews.facebook.net/differential/diff/54627/)
            values_.resize(num_);
            for (uint64_t i = 0; i < num_; ++i) {
                values_[i] = i;
            }
            std::shuffle(
                    values_.begin(), values_.end(),
                    std::default_random_engine(static_cast<unsigned int>(16)));
        }
    }

    uint64_t Next() {
        switch (mode_) {
            case SEQUENTIAL:
                return next_++;
            case RANDOM:
                return rand_->Next() % num_;
            case UNIQUE_RANDOM:
                assert(next_ < num_);
                return values_[next_++];
        }
        assert(false);
        return std::numeric_limits<uint64_t>::max();
    }

private:
    Random64 *rand_;
    WriteMode mode_;
    const uint64_t num_;
    uint64_t next_;
    std::vector<uint64_t> values_;
};

class RandomGenerator {
private:
    std::string data_;
    unsigned int pos_;

public:
    RandomGenerator() {
        // We use a limited amount of data over and over again and ensure
        // that it is larger than the compression window (32KB), and also
        // large enough to serve all typical value sizes we want to write.
        Random rnd(301);
        std::string piece;
        while (data_.size() < (unsigned) std::max(1048576, 4194304)) {
            // Add a short fragment that is as compressible as specified
            // by FLAGS_compression_ratio.
            test::CompressibleString(&rnd, 0.5, 100, &piece);
            data_.append(piece);
        }
        pos_ = 0;
    }

    Slice Generate(unsigned int len) {
        assert(len <= data_.size());
        if (pos_ + len > data_.size()) {
            pos_ = 0;
        }
        pos_ += len;
        return Slice(data_.data() + pos_ - len, len);
    }

    Slice GenerateWithTTL(unsigned int len) {
        assert(len <= data_.size());
        if (pos_ + len > data_.size()) {
            pos_ = 0;
        }
        pos_ += len;
        return Slice(data_.data() + pos_ - len, len);
    }
};

class RangeTabTest : public testing::Test {
public:
    RangeTabTest()
            : pmem_path_("/pmem/rangetab_test"),
              prefix("test_prefix"),
              icmp_(BytewiseComparator()) {
        foptions_ = new FixedRangeBasedOptions(
                16,
                prefix.size(),
                new SimplePrefixExtractor(prefix.size()),
                NewBloomFilterPolicy(16, false),
                1 << 27
        );
        if (file_exists(pmem_path_.c_str()) != 0) {
            pop_ = pool<pRangeRoot>::create(pmem_path_, "rangetab", 256 * 1024 * 1024, CREATE_MODE_RW);
        } else {
            pop_ = pool<pRangeRoot>::open(pmem_path_, "rangetab");
        }
        rootp_ = pop_.root();

        if (rootp_->inited) {
            transaction::run(pop_, [&] {
                rootp_->p_content = make_persistent<NvRangeTab>(pop_, "test", foptions_->range_size_);
                rootp_->inited = true;
            });
        }
        tab = new FixedRangeTab(pop_, foptions_, rootp_->p_content);

        value_size_ = 4 * 1024 * 1024;
    }

    string pmem_path_;
    string prefix;
    pool<pRangeRoot> pop_;
    persistent_ptr<pRangeRoot> rootp_;

    FixedRangeBasedOptions *foptions_;
    const InternalKeyComparator icmp_;
    size_t value_size_;
    //KeyGenerator generator_;

    FixedRangeTab *tab;
};

TEST_F(RangeTabTest, Append){
    Random64 rand(16);
    KeyGenerator key_gen(&rand, SEQUENTIAL, 100);
    RandomGenerator value_gen;
    for(int i = 0; i < 10; i++){
        BuildingChunk chunk(foptions_->filter_policy_, prefix);
        for(int j = 0; j < 10; j++){
            char key[17];
            sprintf(key, "%016lu", key_gen.Next());
            key[16] = 0;
            chunk.Insert(Slice(key, 16), value_gen.Generate(value_size_));
        }
        char* bloom_data;
        ChunkMeta meta;
        meta.prefix = prefix;
        std::string *output_data = chunk.Finish(&bloom_data, meta.cur_start, meta.cur_end);
        ASSERT_OK(tab->Append(icmp_, bloom_data, *output_data, meta.cur_start, meta.cur_end));
    }
    tab->GetProperties();
}

TEST_F(RangeTabTest, Get){
    Random64 rand(16);
    KeyGenerator key_gen(&rand, SEQUENTIAL, 100);
    string* get_value;
    for(int i = 0; i < 10; i++){
        char key[17];
        sprintf(key, "%016lu", key_gen.Next());
        key[16] = 0;
        get_value = new string();
        Status s = tab->Get(icmp_, Slice(key, 16), get_value);
        ASSERT_OK(s);
    }
    delete get_value;
    tab->GetProperties();
}

TEST_F(RangeTabTest, Iterator){
    Arena arena;
    InternalIterator *iter = tab->NewInternalIterator(&icmp_, &arena);
    Random64 rand(16);
    KeyGenerator key_gen(&rand, SEQUENTIAL, 100);
    iter->SeekToFirst();
    for(; iter->Valid(); iter->Next()){
        char key[17];
        sprintf(key, "%016lu", key_gen.Next());
        key[16] = 0;
        ASSERT_EQ(Slice(key, 16), iter->key());
    }
}

TEST_F(RangeTabTest, Compaction){
    tab->SetCompactionWorking(true);
    sleep(1000);
    tab->CleanUp();
    tab->SetCompactionWorking(false);
    tab->GetProperties();
}

TEST_F(RangeTabTest, AppendWhileCompact){

}

}// end rocksdb
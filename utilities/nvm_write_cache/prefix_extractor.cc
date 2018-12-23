//
// Created by 张艺文 on 2018/11/2.
//

#include <sstream>
#include "prefix_extractor.h"
#include "debug.h"

namespace rocksdb {

SimplePrefixExtractor::SimplePrefixExtractor(size_t prefix_bits) : PrefixExtractor(), prefix_bits_(prefix_bits) {}

std::string SimplePrefixExtractor::operator()(const char *input, size_t length) {
    return length > prefix_bits_ ? std::string(input, prefix_bits_) : std::string(input, length);
}


SimplePrefixExtractor *SimplePrefixExtractor::NewSimplePrefixExtractor(uint16_t prefix_bits) {
    return new SimplePrefixExtractor(prefix_bits);
}

DBBenchDedicatedExtractor::DBBenchDedicatedExtractor(size_t prefix_len)
        : prefix_bits_(prefix_len) {}

std::string DBBenchDedicatedExtractor::operator()(const char *input, size_t length) {
    //uint64_t intkey;
    /*void* rawkey = static_cast<void*>(&intkey);
    for(int i = 0; i < 8; i++){
        memcpy(static_cast<char *>(rawkey)+(7-i), input+i, 1);
    }*/
    unsigned int key_num = 0;
    for (size_t x = 0; x < 8; ++x) {
        key_num = key_num * 16 * 16 + *(unsigned char *) (input + x);
    }

    //DBG_PRINT("key[%u]", key_num);

    for (int i = 0; i < (16 - prefix_bits_); i++) {
        key_num /= 10;
    }
    uint16_t len = prefix_bits_;
    char buf[len];
    for (int i = len - 1; i >= 0; i--) {
        buf[i] = key_num % 10 + '0';
        key_num /= 10;
    }
    return std::string(buf, len);
}

DBBenchDedicatedExtractor *DBBenchDedicatedExtractor::NewDBBenchDedicatedExtractor(uint16_t prefix_bits) {
    return new DBBenchDedicatedExtractor(prefix_bits);
}


ArbitrarilyExtractor::ArbitrarilyExtractor(size_t  range_num)
        : range_num_(range_num) {
    DBG_PRINT("new extractor [%d]", range_num);

}

ArbitrarilyExtractor *ArbitrarilyExtractor::NewArbitrarilyExtractor(size_t range_num) {
    return new ArbitrarilyExtractor(range_num);
}

std::string ArbitrarilyExtractor::operator()(const char *input, size_t length) {
    unsigned int key_num = 0;
    for (size_t x = 0; x < 8; ++x) {
        key_num = key_num * 16 * 16 + *(unsigned char *) (input + x);
    }
    key_num /= range_num_;
    //DBG_PRINT("get num [%u] base[%d]", key_num, range_num_);
    char buf[16];
    for (int i = 15; i >= 0; i--) {
        buf[i] = key_num % 10 + '0';
        key_num /= 10;
    }
    return std::string(buf, 16);
}

}//end rocksdb
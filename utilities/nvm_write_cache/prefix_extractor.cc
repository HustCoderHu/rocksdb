//
// Created by 张艺文 on 2018/11/2.
//

#include <sstream>
#include "prefix_extractor.h"

namespace rocksdb{

    SimplePrefixExtractor::SimplePrefixExtractor(uint16_t prefix_bits):PrefixExtractor(), prefix_bits_(prefix_bits){}

    std::string SimplePrefixExtractor::operator()(const char *input, size_t length) {
        return length > prefix_bits_ ? std::string(input, prefix_bits_) : std::string(input, length);
    }


    SimplePrefixExtractor* SimplePrefixExtractor::NewSimplePrefixExtractor(uint16_t prefix_bits) {
        return new SimplePrefixExtractor(prefix_bits);
    }

    DBBenchDedicatedExtractor::DBBenchDedicatedExtractor(uint16_t prefix_len)
        : prefix_bits_(prefix_len) {}

    std::string DBBenchDedicatedExtractor::operator()(const char *input, size_t length) {
        uint64_t intkey;
        memcpy(static_cast<void*>(&intkey), input, 8);
        for(int i = 0; i < (16 - prefix_bits_); i++){
            intkey /= 10;
        }
        uint16_t len = prefix_bits_;
        char buf[len];
        for(int i = len-1; i >=0; i--){
            buf[i] = intkey % 10 + '0';
            intkey /= 10;
        }
        return std::string(buf);
    }

    DBBenchDedicatedExtractor* DBBenchDedicatedExtractor::NewDBBenchDedicatedExtractor(uint16_t prefix_bits) {
        return new DBBenchDedicatedExtractor(prefix_bits);
    }


}//end rocksdb
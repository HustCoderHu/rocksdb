//
// Created by 张艺文 on 2018/11/2.
//

#pragma once

#include <cstdint>
#include <string>
#include <include/rocksdb/slice.h>

namespace rocksdb {

class YCSBExtractor;
class ArbitrarilyExtractor;

class PrefixExtractor {
public:
    PrefixExtractor() = default;

    virtual ~PrefixExtractor() = default;

    virtual std::string operator()(const char *input, size_t length) = 0;

    static ArbitrarilyExtractor *NewArbitrarilyExtractor(size_t num_in_range);
    static YCSBExtractor *NewYCSBExtractor(size_t num_in_range);
};


class SimplePrefixExtractor : public PrefixExtractor {
public:
    explicit SimplePrefixExtractor(size_t prefix_bits_);

    ~SimplePrefixExtractor() = default;

    std::string operator()(const char *input, size_t length);

    static SimplePrefixExtractor *NewSimplePrefixExtractor(uint16_t prefix_bits);


private:
    uint16_t prefix_bits_;
};


class DBBenchDedicatedExtractor : public PrefixExtractor {
public:
    explicit DBBenchDedicatedExtractor(size_t prefix_len);

    ~DBBenchDedicatedExtractor() = default;

    std::string operator()(const char *input, size_t length);

    static DBBenchDedicatedExtractor *NewDBBenchDedicatedExtractor(uint16_t prefix_bits);

private:
    uint16_t prefix_bits_;
};

class ArbitrarilyExtractor : public PrefixExtractor {
public:
    explicit ArbitrarilyExtractor(size_t num_in_range);

    ~ArbitrarilyExtractor() = default;

    std::string operator()(const char *input, size_t length);

    static ArbitrarilyExtractor *NewArbitrarilyExtractor(size_t num_in_range);

private:
    size_t num_in_range_;
};

class YCSBExtractor : public PrefixExtractor {
public:
    explicit YCSBExtractor(size_t range_num);

    ~YCSBExtractor() = default;

    std::string operator()(const char *input, size_t length) override
    {
        uint64_t key_num = 0;
        size_t _min = length < 8? length : 8;
        for (size_t x = 0; x < _min; ++x) {
          uint64_t toLL = input[x];
          key_num |= (toLL << 8*x);
        }
        key_num %= range_num_;
        //DBG_PRINT("get num [%u] base[%d]", key_num, num_in_range_);
        char buf[16];
        for (int i = 15; i >= 0; i--) {
            buf[i] = key_num % 10 + '0';
            key_num /= 10;
        }
        return std::string(buf, 16);
    }
private:
    size_t range_num_;
};
}// end rocksdb

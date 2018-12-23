//
// Created by 张艺文 on 2018/11/2.
//

#pragma once

#include <cstdint>
#include <string>
#include <include/rocksdb/slice.h>

namespace rocksdb {

class PrefixExtractor {
public:
    PrefixExtractor() = default;

    virtual ~PrefixExtractor() = default;

    virtual std::string operator()(const char *input, size_t length) = 0;

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
    explicit ArbitrarilyExtractor(size_t range_num);

    ~ArbitrarilyExtractor() = default;

    std::string operator()(const char *input, size_t length);

    static ArbitrarilyExtractor *NewArbitrarilyExtractor(size_t range_num);

private:
    uint16_t range_num_;
};
}// end rocksdb


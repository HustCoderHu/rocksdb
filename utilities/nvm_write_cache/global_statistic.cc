//
// Created by уерунд on 2018/12/22.
//

#include <cstdint>
#include "global_statistic.h"

namespace rocksdb{
#ifdef DELAY_COUNT
    int delay_count;
    int cur;
    int delay_stat[5];
    uint64_t compact_count;
#endif
}


//
// Created by уерунд on 2018/12/22.
//

#pragma once

//#define DELAY_COUNT

namespace rocksdb{
#ifdef DELAY_COUNT
    extern int delay_count;
    extern int cur;
    extern int delay_stat[];
    extern uint64_t compact_count;
    extern int key_written;
    extern double key_percent;
#endif
}



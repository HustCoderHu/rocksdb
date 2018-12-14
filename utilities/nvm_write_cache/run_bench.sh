#!/usr/bin/bash

bench_base_setting="--benchmarks=\"fillrandom,readrandom\" \
                    --db=\"/pmem/rocksdb_dir\" \
                    --key_size=16 \
                    --value_size=4096 \
                    --num=10000000 \
                    --reads=1000
                    --write_buffer_size=16777216
                    --max_bytes_for_level_base=`expr 256 \* 1048576 \* 10` \
                    --use_existing_db=0"

nvm_setting="   --use_nvm_write_cache=1 \
                --reset_nvm_write_cache=false \
                --pmem_path=\"/pmem/rocksdb_dir/nvmtest/range\" \
                --chunk_bloom_bits=16"

remake="git pull && make db_bench -j16"

clear="rm -rf /pmem/rocksdb_dir/nvmtest/*"

if test $1 -eq 1
then
echo ${remake}
echo ${clear}
cmd="./db_bench ${bench_base_setting} --value_size=$2 ${nvm_setting} --range_num=$3 --prefix_bits=$4"
echo ${cmd}

else
echo ${clear}
cmd="./db_bench ${bench_base_setting} --value_size=$2 ${nvm_setting} --range_num=$3 --prefix_bits=$4"
echo ${cmd}
fi


## RocksDB-Kai: A Persistent Key-Value Store for Flash and RAM Storage

V1.0

## rocksdbjni
```
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DWITH_SNAPPY=1 -DWITH_JNI=1 -DUSE_RTTI=1 ..
# Release 会增加 -O3
# WITH_JNI 看 CMakeList 可以找到答案
# USE_RTTI 不加就会导致 libpmemobj++ 编译错误 (typeid找不到)
make rocksdbjni-shared
mkdir unzip_classes
cd unzip_classes
cp ../java/librocksdbjni-shared.so librocksdbjni-linux64.so
$JAVA_HOME/bin/jar -xf ../java/rocksdbjni_classes.jar
$JAVA_HOME/bin/jar -cf rocksdbjni-5.18.0.jar ../librocksdb.so librocksdbjni-linux64.so org/rocksdb/* 
```

## run ycsb
```
export YCSBExtractor=111
export FLAGS_wal_dir=/home/kv-pmem/xiaohu/FLAGS_wal_dir
export FLAGS_wal_dir=/mnt/ssd/xiaohu/doubleV2
export PMEM_PATH=/pmem/xiaohu/doubleV2/pmem.map
export RANGE_NUM=128
export RANGE_SIZE=64
export KEY_NUM=200000
echo default > $FLAGS_wal_dir/CF_NAMES
bin/ycsb.sh load rocksdb -P test-workload -p rocksdb.dir=$FLAGS_wal_dir 2>&1 | tee test-workload.log
```

## License

RocksDB is dual-licensed under both the GPLv2 (found in the COPYING file in the root directory) and Apache 2.0 License (found in the LICENSE.Apache file in the root directory).  You may select, at your option, one of the above-listed licenses.

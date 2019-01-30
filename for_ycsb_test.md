## rocksdbjni
```
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DWITH_SNAPPY=1 -DWITH_JNI=1 -DUSE_RTTI=1 ..
# Release 会增加 -O3
# WITH_JNI 看 CMakeList 可以找到答案
# USE_RTTI 不加就会导致 libpmemobj++ 编译错误 (typeid找不到)
make rocksdbjni-shared -j${N}

# 打包 jar
mkdir unzip_classes
cd unzip_classes
cp ../java/librocksdbjni-shared.so librocksdbjni-linux64.so
$JAVA_HOME/bin/jar -xf ../java/rocksdbjni_classes.jar
$JAVA_HOME/bin/jar -cf rocksdbjni-x.xx.jar ../librocksdb.so librocksdbjni-linux64.so org/rocksdb/* 
```
最后把生成的 jar 文件替换 rocksdb-binding/lib 目录下 rocksdbjni.jar

debug 方式生成的 so 较大，且运行慢，调代码时使用  
Release 快很多，但是缺少很多关键输出，不方便调试定位代码问题，仅最终测数据时使用

## 参数传递
通过 ycsb 把参数传到 rocksdb 需要大范围修改 ycsb，目前是直接在 rocksjni.cc 里通过环境变量获取，修改集中在两个`rocksdb_open_helper` 函数

下面的函数在 rocksdbClient 里被用来生成 options
```
ColumnFamilyOptions* ColumnFamilyOptions::OptimizeLevelStyleCompaction(
    uint64_t memtable_memory_budget)
```

## run ycsb
```
export YCSBExtractor=111
export FLAGS_wal_dir=/mnt/ssd/xiaohu/doubleV2
export PMEM_PATH=/pmem/xiaohu/doubleV2/pmem.map
export RANGE_NUM=128
export RANGE_SIZE=64
export KEY_NUM=200000

echo default > $FLAGS_wal_dir/CF_NAMES
bin/ycsb.sh load rocksdb -s -P test-workload -p rocksdb.dir=$FLAGS_wal_dir 2>&1 | tee test-workload.log
bin/ycsb.sh run rocksdb -s -P test-workload -p rocksdb.dir=$FLAGS_wal_dir 2>&1 | tee test-workload.log
```
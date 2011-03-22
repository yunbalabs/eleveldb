// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include "db/db_impl.h"
#include "db/version_set.h"
#include "include/cache.h"
#include "include/db.h"
#include "include/env.h"
#include "include/write_batch.h"
#include "util/histogram.h"
#include "util/random.h"
#include "util/testutil.h"

// Comma-separated list of operations to run in the specified order
//   Actual benchmarks:
//      fillseq       -- write N values in sequential key order in async mode
//      fillrandom    -- write N values in random key order in async mode
//      overwrite     -- overwrite N values in random key order in async mode
//      fillsync      -- write N/100 values in random key order in sync mode
//      fill100K      -- write N/1000 100K values in random order in async mode
//      readseq       -- read N values sequentially
//      readreverse   -- read N values in reverse order
//      readrandom    -- read N values in random order
//   Meta operations:
//      compact     -- Compact the entire DB
//      heapprofile -- Dump a heap profile (if supported by this port)
//      sync        -- switch to synchronous writes (not the default)
//      nosync      -- switch to asynchronous writes (the default)
//      tenth       -- divide N by 10 (i.e., following benchmarks are smaller)
//      normal      -- reset N back to its normal value (1000000)
static const char* FLAGS_benchmarks =
    "fillseq,"
    "fillrandom,"
    "overwrite,"
    "fillsync,"
    "readseq,"
    "readreverse,"
    "readrandom,"
    "compact,"
    "readseq,"
    "readreverse,"
    "readrandom,"
    "fill100K";

// Number of key/values to place in database
static int FLAGS_num = 1000000;

// Size of each value
static int FLAGS_value_size = 100;

// Arrange to generate values that shrink to this fraction of
// their original size after compression
static double FLAGS_compression_ratio = 0.5;

// Print histogram of operation timings
static bool FLAGS_histogram = false;

// Number of bytes to buffer in memtable before compacting
static int FLAGS_write_buffer_size = 1 << 20;

namespace leveldb {

// Helper for quickly generating random data.
namespace {
class RandomGenerator {
 private:
  std::string data_;
  int pos_;

 public:
  RandomGenerator() {
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    Random rnd(301);
    std::string piece;
    while (data_.size() < 1048576) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      test::CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice Generate(int len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }
};

static Slice TrimSpace(Slice s) {
  int start = 0;
  while (start < s.size() && isspace(s[start])) {
    start++;
  }
  int limit = s.size();
  while (limit > start && isspace(s[limit-1])) {
    limit--;
  }
  return Slice(s.data() + start, limit - start);
}

}

class Benchmark {
 private:
  Cache* cache_;
  DB* db_;
  int num_;
  int heap_counter_;
  double start_;
  double last_op_finish_;
  int64_t bytes_;
  std::string message_;
  Histogram hist_;
  RandomGenerator gen_;
  Random rand_;

  // State kept for progress messages
  int done_;
  int next_report_;     // When to report next

  void PrintHeader() {
    const int kKeySize = 16;
    PrintEnvironment();
    fprintf(stdout, "Keys:       %d bytes each\n", kKeySize);
    fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
            FLAGS_value_size,
            static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    fprintf(stdout, "Entries:    %d\n", num_);
    fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
            (((kKeySize + FLAGS_value_size) * num_) / 1048576.0));
    fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
            (((kKeySize + FLAGS_value_size * FLAGS_compression_ratio) * num_)
             / 1048576.0));
    PrintWarnings();
    fprintf(stdout, "------------------------------------------------\n");
  }

  void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    fprintf(stdout,
            "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n"
            );
#endif
#ifndef NDEBUG
    fprintf(stdout,
            "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif
  }

  void PrintEnvironment() {
    fprintf(stderr, "LevelDB:    version %d.%d\n",
            kMajorVersion, kMinorVersion);

#if defined(__linux)
    time_t now = time(NULL);
    fprintf(stderr, "Date:       %s", ctime(&now));  // ctime() adds newline

    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
      char line[1000];
      int num_cpus = 0;
      std::string cpu_type;
      std::string cache_size;
      while (fgets(line, sizeof(line), cpuinfo) != NULL) {
        const char* sep = strchr(line, ':');
        if (sep == NULL) {
          continue;
        }
        Slice key = TrimSpace(Slice(line, sep - 1 - line));
        Slice val = TrimSpace(Slice(sep + 1));
        if (key == "model name") {
          ++num_cpus;
          cpu_type = val.ToString();
        } else if (key == "cache size") {
          cache_size = val.ToString();
        }
      }
      fclose(cpuinfo);
      fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#endif
  }

  void Start() {
    start_ = Env::Default()->NowMicros() * 1e-6;
    bytes_ = 0;
    message_.clear();
    last_op_finish_ = start_;
    hist_.Clear();
    done_ = 0;
    next_report_ = 100;
  }

  void FinishedSingleOp() {
    if (FLAGS_histogram) {
      double now = Env::Default()->NowMicros() * 1e-6;
      double micros = (now - last_op_finish_) * 1e6;
      hist_.Add(micros);
      if (micros > 20000) {
        fprintf(stderr, "long op: %.1f micros%30s\r", micros, "");
        fflush(stderr);
      }
      last_op_finish_ = now;
    }

    done_++;
    if (done_ >= next_report_) {
      if (next_report_ < 1000) {
        next_report_ += 100;
      } else if (next_report_ < 10000) {
        next_report_ += 1000;
      } else if (next_report_ < 100000) {
        next_report_ += 10000;
      } else {
        next_report_ += 100000;
      }
      fprintf(stderr, "... finished %d ops%30s\r", done_, "");
      fflush(stderr);
    }
  }

  void Stop(const Slice& name) {
    double finish = Env::Default()->NowMicros() * 1e-6;

    // Pretend at least one op was done in case we are running a benchmark
    // that does nto call FinishedSingleOp().
    if (done_ < 1) done_ = 1;

    if (bytes_ > 0) {
      char rate[100];
      snprintf(rate, sizeof(rate), "%5.1f MB/s",
               (bytes_ / 1048576.0) / (finish - start_));
      if (!message_.empty()) {
        message_  = std::string(rate) + " " + message_;
      } else {
        message_ = rate;
      }
    }

    fprintf(stdout, "%-12s : %11.3f micros/op;%s%s\n",
            name.ToString().c_str(),
            (finish - start_) * 1e6 / done_,
            (message_.empty() ? "" : " "),
            message_.c_str());
    if (FLAGS_histogram) {
      fprintf(stdout, "Microseconds per op:\n%s\n", hist_.ToString().c_str());
    }
    fflush(stdout);
  }

 public:
  enum Order {
    SEQUENTIAL,
    RANDOM
  };
  enum DBState {
    FRESH,
    EXISTING
  };

  Benchmark() : cache_(NewLRUCache(200<<20)),
                db_(NULL),
                num_(FLAGS_num),
                heap_counter_(0),
                bytes_(0),
                rand_(301) {
    std::vector<std::string> files;
    Env::Default()->GetChildren("/tmp/dbbench", &files);
    for (int i = 0; i < files.size(); i++) {
      if (Slice(files[i]).starts_with("heap-")) {
        Env::Default()->DeleteFile("/tmp/dbbench/" + files[i]);
      }
    }
    DestroyDB("/tmp/dbbench", Options());
  }

  ~Benchmark() {
    delete db_;
    delete cache_;
  }

  void Run() {
    PrintHeader();
    Open();

    const char* benchmarks = FLAGS_benchmarks;
    while (benchmarks != NULL) {
      const char* sep = strchr(benchmarks, ',');
      Slice name;
      if (sep == NULL) {
        name = benchmarks;
        benchmarks = NULL;
      } else {
        name = Slice(benchmarks, sep - benchmarks);
        benchmarks = sep + 1;
      }

      Start();

      WriteOptions write_options;
      write_options.sync = false;
      if (name == Slice("fillseq")) {
        Write(write_options, SEQUENTIAL, FRESH, num_, FLAGS_value_size);
      } else if (name == Slice("fillrandom")) {
        Write(write_options, RANDOM, FRESH, num_, FLAGS_value_size);
      } else if (name == Slice("overwrite")) {
        Write(write_options, RANDOM, EXISTING, num_, FLAGS_value_size);
      } else if (name == Slice("fillsync")) {
        write_options.sync = true;
        Write(write_options, RANDOM, FRESH, num_ / 100, FLAGS_value_size);
      } else if (name == Slice("fill100K")) {
        Write(write_options, RANDOM, FRESH, num_ / 1000, 100 * 1000);
      } else if (name == Slice("readseq")) {
        ReadSequential();
      } else if (name == Slice("readreverse")) {
        ReadReverse();
      } else if (name == Slice("readrandom")) {
        ReadRandom();
      } else if (name == Slice("compact")) {
        Compact();
      } else if (name == Slice("heapprofile")) {
        HeapProfile();
      } else {
        fprintf(stderr, "unknown benchmark '%s'\n", name.ToString().c_str());
      }
      Stop(name);
    }
  }

 private:
  void Open() {
    assert(db_ == NULL);
    Options options;
    options.create_if_missing = true;
    options.max_open_files = 10000;
    options.block_cache = cache_;
    options.write_buffer_size = FLAGS_write_buffer_size;
    Status s = DB::Open(options, "/tmp/dbbench", &db_);
    if (!s.ok()) {
      fprintf(stderr, "open error: %s\n", s.ToString().c_str());
      exit(1);
    }
  }

  void Write(const WriteOptions& options, Order order, DBState state,
             int num_entries, int value_size) {
    if (state == FRESH) {
      delete db_;
      db_ = NULL;
      DestroyDB("/tmp/dbbench", Options());
      Open();
      Start();  // Do not count time taken to destroy/open
    }

    if (num_entries != num_) {
      char msg[100];
      snprintf(msg, sizeof(msg), "(%d ops)", num_entries);
      message_ = msg;
    }

    WriteBatch batch;
    Status s;
    std::string val;
    for (int i = 0; i < num_entries; i++) {
      const int k = (order == SEQUENTIAL) ? i : (rand_.Next() % FLAGS_num);
      char key[100];
      snprintf(key, sizeof(key), "%016d", k);
      batch.Clear();
      batch.Put(key, gen_.Generate(value_size));
      s = db_->Write(options, &batch);
      bytes_ += value_size + strlen(key);
      if (!s.ok()) {
        fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
      FinishedSingleOp();
    }
  }

  void ReadSequential() {
    Iterator* iter = db_->NewIterator(ReadOptions());
    int i = 0;
    for (iter->SeekToFirst(); i < num_ && iter->Valid(); iter->Next()) {
      bytes_ += iter->key().size() + iter->value().size();
      FinishedSingleOp();
      ++i;
    }
    delete iter;
  }

  void ReadReverse() {
    Iterator* iter = db_->NewIterator(ReadOptions());
    int i = 0;
    for (iter->SeekToLast(); i < num_ && iter->Valid(); iter->Prev()) {
      bytes_ += iter->key().size() + iter->value().size();
      FinishedSingleOp();
      ++i;
    }
    delete iter;
  }

  void ReadRandom() {
    ReadOptions options;
    std::string value;
    for (int i = 0; i < num_; i++) {
      char key[100];
      const int k = rand_.Next() % FLAGS_num;
      snprintf(key, sizeof(key), "%016d", k);
      db_->Get(options, key, &value);
      FinishedSingleOp();
    }
  }

  void Compact() {
    DBImpl* dbi = reinterpret_cast<DBImpl*>(db_);
    dbi->TEST_CompactMemTable();
    int max_level_with_files = 1;
    for (int level = 1; level < config::kNumLevels; level++) {
      uint64_t v;
      char name[100];
      snprintf(name, sizeof(name), "leveldb.num-files-at-level%d", level);
      if (db_->GetProperty(name, &v) && v > 0) {
        max_level_with_files = level;
      }
    }
    for (int level = 0; level < max_level_with_files; level++) {
      dbi->TEST_CompactRange(level, "", "~");
    }
  }

  static void WriteToFile(void* arg, const char* buf, int n) {
    reinterpret_cast<WritableFile*>(arg)->Append(Slice(buf, n));
  }

  void HeapProfile() {
    char fname[100];
    snprintf(fname, sizeof(fname), "/tmp/dbbench/heap-%04d", ++heap_counter_);
    WritableFile* file;
    Status s = Env::Default()->NewWritableFile(fname, &file);
    if (!s.ok()) {
      message_ = s.ToString();
      return;
    }
    bool ok = port::GetHeapProfile(WriteToFile, file);
    delete file;
    if (!ok) {
      message_ = "not supported";
      Env::Default()->DeleteFile(fname);
    }
  }
};

}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    double d;
    int n;
    char junk;
    if (leveldb::Slice(argv[i]).starts_with("--benchmarks=")) {
      FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
    } else if (sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
      FLAGS_compression_ratio = d;
    } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_histogram = n;
    } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
      FLAGS_num = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (sscanf(argv[i], "--write_buffer_size=%d%c", &n, &junk) == 1) {
      FLAGS_write_buffer_size = n;
    }  else {
      fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      exit(1);
    }
  }

  leveldb::Benchmark benchmark;
  benchmark.Run();
  return 0;
}

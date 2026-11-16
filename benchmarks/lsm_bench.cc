#include "lsm/db.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static double us(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }
static double cpu() {
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r)) throw std::runtime_error("getrusage failed");
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}
static std::string key(size_t n, size_t width) {
    auto s = std::to_string(n);
    if (s.size() > width) throw std::runtime_error("key width too small");
    return std::string(width - s.size(), '0') + s;
}
static size_t tables(const fs::path& dir) {
    size_t n = 0;
    for (auto& e : fs::directory_iterator(dir)) n += e.path().extension() == ".sst";
    return n;
}
static uintmax_t bytes(const fs::path& dir) {
    uintmax_t n = 0;
    for (auto& e : fs::directory_iterator(dir)) if (e.is_regular_file()) n += e.file_size();
    return n;
}
struct Phase {
    std::string name;
    std::vector<double> latency;
    double cpu_start = cpu();
    Clock::time_point wall_start = Clock::now();
    explicit Phase(std::string n) : name(std::move(n)) {}
    template<class F> void sample(F f) {
        auto start = Clock::now();
        f();
        auto end = Clock::now();
        latency.push_back(us(end-start));
    }
    void report(const fs::path& dir, const std::string& cache, double cpu_end, Clock::time_point end) {
        if (latency.empty()) return;
        double total = std::accumulate(latency.begin(), latency.end(), 0.0);
        std::sort(latency.begin(), latency.end());
        auto p = [&](double q) { return latency[static_cast<size_t>(std::ceil(q * latency.size()))-1]; };
        rusage r{};
        if (getrusage(RUSAGE_SELF, &r)) throw std::runtime_error("getrusage failed");
        double wall = us(end-wall_start) / 1e6;
        std::cout << std::fixed << std::setprecision(3)
          << "{\"phase\":\"" << name << "\",\"cache\":\"" << cache
          << "\",\"operations\":" << latency.size() << ",\"api_seconds\":" << total/1e6
          << ",\"wall_seconds\":" << wall << ",\"ops_per_second\":" << latency.size()*1e6/total
          << ",\"wall_ops_per_second\":" << latency.size()/wall
          << ",\"p50_us\":" << p(.50) << ",\"p95_us\":" << p(.95)
          << ",\"p99_us\":" << p(.99) << ",\"max_us\":" << latency.back()
          << ",\"cpu_seconds\":" << cpu_end-cpu_start
          << ",\"cpu_percent_one_core\":" << (cpu_end-cpu_start)*100/wall
          << ",\"process_peak_rss_kib\":" << r.ru_maxrss
          << ",\"disk_bytes_end\":" << bytes(dir) << ",\"sstables_end\":" << tables(dir)
          << ",\"wal_bytes_end\":" << (fs::exists(dir/"wal.log") ? fs::file_size(dir/"wal.log") : 0)
          << "}\n";
    }
    void finish(const fs::path& dir, const std::string& cache = "not_applicable") {
        auto end = Clock::now(); auto c = cpu(); report(dir, cache, c, end);
    }
};
int main(int argc, char** argv) try {
    if (argc != 11) throw std::runtime_error("usage: lsm-bench DIR MODE N WAL TABLES THRESHOLD KEY_BYTES VALUE_BYTES SEED REOPENS");
    fs::path dir = argv[1]; std::string mode = argv[2];
    size_t n = std::stoull(argv[3]), count = std::stoull(argv[5]);
    size_t width = std::stoull(argv[7]), value_size = std::stoull(argv[8]);
    size_t reopens = std::stoull(argv[10]);
    if (!n || !width || !value_size || !reopens) throw std::runtime_error("sizes must be positive");
    // Never reuse or delete a caller's existing database.
    if (!fs::create_directory(dir)) throw std::runtime_error("benchmark directory must not exist");
    DBOptions opts; opts.enableWal = std::stoull(argv[4]) != 0;
    opts.memTableSizeLimit = std::stoull(argv[6]);
    auto db = std::make_unique<DB>(dir.string(), opts);
    std::mt19937_64 rng(std::stoull(argv[9]));
    std::vector<std::string> keys; keys.reserve(2*n);
    for (size_t i=0; i<2*n; ++i) keys.push_back(key(i,width));
    std::string value(value_size, 'v');
    if (mode == "sequential" || mode == "random" || mode == "overwrite" || mode == "threshold") {
        std::vector<size_t> order(n); std::iota(order.begin(), order.end(), 0);
        if (mode == "random") std::shuffle(order.begin(), order.end(), rng);
        if (mode == "overwrite") for (auto& i : order) i %= std::max<size_t>(1,n/10);
        Phase all("put"), trigger("put_triggered_flush");
        size_t previous = tables(dir);
        for (auto i : order) {
            all.sample([&] { db->Put(keys[i], value); });
            if (mode == "threshold") {
                size_t current = tables(dir);
                if (current != previous) trigger.latency.push_back(all.latency.back());
                previous = current;
            }
        }
        auto end = Clock::now(); auto c = cpu();
        all.report(dir,"not_applicable",c,end);
        if (mode == "threshold") trigger.report(dir,"not_applicable",c,end);
    } else if (mode == "query" || mode == "mixed95" || mode == "mixed50" || mode == "recovery") {
        // Interleaved key ranges force old-hit lookups to visit newer overlapping tables.
        for (size_t t=0; t<count; ++t) {
            for (size_t i=t; i<n; i+=count) db->Put(keys[i],value);
            db->Flush();
        }
        if (!count) for (size_t i=0; i<n; ++i) db->Put(keys[i],value);
        if (mode == "query") {
            std::vector<size_t> order(n); std::iota(order.begin(),order.end(),0);
            std::shuffle(order.begin(),order.end(),rng);
            for (const auto& pass : {std::string("first_pass"),std::string("warm")}) {
                Phase hit("get_hit_"+pass);
                for (auto i : order) {
                    std::optional<std::string> result;
                    hit.sample([&] { result = db->Get(keys[i]); });
                    if (!result || *result != value) throw std::runtime_error("query value mismatch");
                }
                hit.finish(dir, count ? (pass == "warm" ? "warm_os_page_cache" : "first_pass_after_setup_os_cache_uncontrolled") : "memtable");
            }
            for (const auto& pass : {std::string("first_pass"),std::string("warm")}) {
                Phase miss("get_miss_"+pass);
                // Preserve prefixes; replace the last digit with a distinct non-digit.
                for (auto i : order) {
                    auto absent = keys[i]; absent.back() = static_cast<char>('a' + (i % 10));
                    std::optional<std::string> result;
                    miss.sample([&] { result = db->Get(absent); });
                    if (result) throw std::runtime_error("missing key returned a value");
                }
                miss.finish(dir,"os_cache_uncontrolled_or_warmed_by_prior_reads");
            }
        } else if (mode == "recovery") {
            if (count) {
                // Add a WAL tail after the SSTable fixture; n records in both cases.
                for (size_t i=n; i<2*n; ++i) db->Put(keys[i],value);
            }
            db.reset();
            Phase open("open");
            for (size_t i=0; i<reopens; ++i) {
                open.sample([&] { db = std::make_unique<DB>(dir.string(),opts); });
                auto got = db->Get(keys[count ? 2*n-1 : n-1]);
                if (!got || *got != value) throw std::runtime_error("recovery mismatch");
                db.reset();
            }
            open.finish(dir,"reopen_os_cache_uncontrolled_no_crash");
        } else {
            // 95:4:1 and 50:40:10 read:put:delete. Trace generation excluded from API time.
            std::vector<bool> present(n,true);
            Phase all("mixed"), reads("get"), writes("put"), deletes("delete");
            size_t read_pct = mode == "mixed95" ? 95 : 50;
            size_t delete_pct = mode == "mixed95" ? 1 : 10;
            for (size_t i=0; i<n; ++i) {
                auto k = rng()%n, op = rng()%100;
                if (op < read_pct) {
                    std::optional<std::string> got;
                    reads.sample([&] { got = db->Get(keys[k]); });
                    if (got.has_value() != present[k] || (got && *got != value)) throw std::runtime_error("mixed value mismatch");
                    all.latency.push_back(reads.latency.back());
                } else if (op < read_pct+delete_pct) {
                    deletes.sample([&] { db->Delete(keys[k]); }); present[k]=false;
                    all.latency.push_back(deletes.latency.back());
                } else {
                    writes.sample([&] { db->Put(keys[k],value); }); present[k]=true;
                    all.latency.push_back(writes.latency.back());
                }
            }
            auto end = Clock::now(); auto c=cpu();
            for (auto p : {&all,&reads,&writes,&deletes}) p->report(dir,"warm_setup",c,end);
        }
    } else if (mode == "flush") {
        Phase flush("manual_flush"), after("first_put_after_flush");
        for (size_t batch=0; batch<reopens; ++batch) {
            for (size_t i=0; i<n; ++i) db->Put(keys[i],value);
            flush.sample([&] { db->Flush(); });
            after.sample([&] { db->Put(keys[n],value); });
        }
        auto end=Clock::now(); auto c=cpu();
        flush.report(dir,"not_applicable",c,end); after.report(dir,"not_applicable",c,end);
    } else throw std::runtime_error("unknown mode");
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }

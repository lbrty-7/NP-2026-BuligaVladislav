// Laboratory Work 1: Concurrency in Practice
// Author: Buliga Vladislav, FAF-243
//
// One program, every task selected by command-line parameters.
//
//   lab1 gen   --out numbers.txt [--count 50000000] [--seed 42]
//   lab1 split --in numbers.txt --outdir parts [--parts 8]
//   lab1 1a    --in numbers.txt --threads N
//   lab1 1b    --in numbers.txt [--threads N] [--version 1|2|3|all] [--runs1 3]
//   lab1 1c    --in numbers.txt --threads N   [--batch 10000] [--capacity 100]
//   lab1 2     --dir /Volumes/USB --threads N
//   lab1 info
//
// Every task prints one machine-readable line per measurement:
//   RESULT task=1a threads=4 time_ms=... peak_mb=... neg=... pos=... zero=... total=... check=OK

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #define NOMINMAX
  #include <windows.h>
  #include <psapi.h>
#else
  #include <sys/resource.h>
  #include <dirent.h>
  #if defined(__APPLE__)
    #include <mach/mach.h>
  #endif
#endif

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

struct Counts {
    long long neg = 0, pos = 0, zero = 0;
    long long total() const { return neg + pos + zero; }
    Counts& operator+=(const Counts& o) { neg += o.neg; pos += o.pos; zero += o.zero; return *this; }
};

// Peak resident memory (RSS high-water mark) of this process, in MiB.
// Note: getrusage().ru_maxrss is NOT used on Linux, because after fork()+exec()
// it keeps the peak of the parent (e.g. the Python runner); VmHWM belongs only
// to this program's own address space.
static double peak_memory_mb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
    return -1;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size_max / (1024.0 * 1024.0);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss / (1024.0 * 1024.0);   // macOS: bytes
#else
    {
        std::ifstream st("/proc/self/status");
        std::string key;
        while (st >> key) {
            if (key == "VmHWM:") { double kb; st >> kb; return kb / 1024.0; }
            std::getline(st, key);
        }
    }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
  #if defined(__APPLE__)
    return ru.ru_maxrss / (1024.0 * 1024.0);   // macOS: bytes
  #else
    return ru.ru_maxrss / 1024.0;              // Linux: kilobytes
  #endif
#endif
}

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static long long file_size(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::cerr << "cannot open " << path << "\n"; std::exit(1); }
    return static_cast<long long>(f.tellg());
}

// Expected counts are written by "gen" next to the data file (<file>.counts).
static std::optional<Counts> load_expected(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    Counts c;
    if (!(f >> c.neg >> c.pos >> c.zero)) return std::nullopt;
    return c;
}

static std::string check_str(const Counts& c, const std::optional<Counts>& expected) {
    if (!expected) return c.total() == 50000000LL ? "TOTAL_OK" : "FAIL";
    return (c.neg == expected->neg && c.pos == expected->pos && c.zero == expected->zero) ? "OK" : "FAIL";
}

static void print_result(const std::string& head, double time_ms, const Counts& c,
                         const std::optional<Counts>& expected, const std::string& extra = "") {
    std::printf("RESULT %s time_ms=%.1f peak_mb=%.1f neg=%lld pos=%lld zero=%lld total=%lld check=%s%s%s\n",
                head.c_str(), time_ms, peak_memory_mb(), c.neg, c.pos, c.zero, c.total(),
                check_str(c, expected).c_str(), extra.empty() ? "" : " ", extra.c_str());
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Parser: a small state machine that turns bytes into integers and counts signs.
// It can be fed a buffer in pieces; a number split across two pieces is handled.
// ---------------------------------------------------------------------------

struct LineParser {
    Counts counts;
    bool in_line = false;   // we have consumed at least one byte of the current line
    bool negative = false;
    bool has_digit = false;
    long long value = 0;

    inline void finish_line() {
        if (has_digit) {
            long long v = negative ? -value : value;
            if (v < 0) ++counts.neg; else if (v > 0) ++counts.pos; else ++counts.zero;
        }
        in_line = negative = has_digit = false;
        value = 0;
    }

    inline void feed(char ch) {
        if (ch == '\n') { finish_line(); return; }
        in_line = true;
        if (ch >= '0' && ch <= '9') { value = value * 10 + (ch - '0'); has_digit = true; }
        else if (ch == '-') negative = true;
        // '\r' and other bytes are ignored
    }

    void feed(const char* p, size_t n) { for (size_t i = 0; i < n; ++i) feed(p[i]); }
    void end_of_input() { if (in_line) finish_line(); }
};

// Parse one line (a std::string) into an int.
static inline int parse_int(const std::string& s) {
    int v = 0; bool neg = false;
    for (char ch : s) {
        if (ch == '-') neg = true;
        else if (ch >= '0' && ch <= '9') v = v * 10 + (ch - '0');
    }
    return neg ? -v : v;
}

// ---------------------------------------------------------------------------
// gen / split
// ---------------------------------------------------------------------------

static int cmd_gen(const std::string& out, long long count, unsigned long long seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> dist(-1000000, 1000000);
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) { std::cerr << "cannot write " << out << "\n"; return 1; }
    std::vector<char> buf(1 << 22);
    size_t used = 0;
    Counts c;
    char tmp[16];
    auto t0 = Clock::now();
    for (long long i = 0; i < count; ++i) {
        int v = dist(rng);
        if (v < 0) ++c.neg; else if (v > 0) ++c.pos; else ++c.zero;
        int len = std::snprintf(tmp, sizeof tmp, "%d\n", v);
        if (used + len > buf.size()) { std::fwrite(buf.data(), 1, used, f); used = 0; }
        std::memcpy(buf.data() + used, tmp, len);
        used += len;
    }
    std::fwrite(buf.data(), 1, used, f);
    std::fclose(f);
    std::ofstream(out + ".counts") << c.neg << " " << c.pos << " " << c.zero << "\n";
    std::printf("generated %lld numbers into %s in %.1f ms\n", count, out.c_str(), ms_since(t0));
    std::printf("expected: neg=%lld pos=%lld zero=%lld total=%lld (saved to %s.counts)\n",
                c.neg, c.pos, c.zero, c.total(), out.c_str());
    return 0;
}

static int cmd_split(const std::string& in, const std::string& outdir, int parts) {
    // Count the lines first so the parts are exactly equal and in order.
    long long lines = 0;
    {
        std::ifstream f(in, std::ios::binary);
        std::vector<char> buf(1 << 22);
        while (f) {
            f.read(buf.data(), buf.size());
            std::streamsize n = f.gcount();
            lines += std::count(buf.data(), buf.data() + n, '\n');
        }
    }
    long long per = lines / parts;
    std::ifstream f(in, std::ios::binary);
    std::string line;
    for (int p = 0; p < parts; ++p) {
        char name[64];
        std::snprintf(name, sizeof name, "/numbers_part_%d.txt", p + 1);
        std::string path = outdir + name;
        std::ofstream o(path, std::ios::binary);
        if (!o) { std::cerr << "cannot write " << path << " (does the folder exist?)\n"; return 1; }
        long long n = (p == parts - 1) ? lines - per * (parts - 1) : per;
        for (long long i = 0; i < n && std::getline(f, line); ++i) { o << line << '\n'; }
        std::printf("%s: %lld lines\n", path.c_str(), n);
    }
    // Copy the expected counts so Task 2 can check itself.
    if (auto e = load_expected(in + ".counts"))
        std::ofstream(outdir + "/expected.counts") << e->neg << " " << e->pos << " " << e->zero << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Task 1a: N threads, the file is divided into N byte ranges.
//
// Boundary rule: a thread owns every line that STARTS inside [start, end).
//  - If start > 0 and the byte before start is not '\n', the thread is in the
//    middle of a line that belongs to the previous thread, so it skips to the
//    byte after the next '\n'.
//  - It keeps reading past `end` only to finish the line that started before
//    `end`; it never starts a new line at a position >= end.
// So every line is counted by exactly one thread.
// ---------------------------------------------------------------------------

static Counts count_range(const std::string& path, long long start, long long end) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> buf(1 << 20);    // 1 MiB read buffer per thread
    long long pos = start;

    if (start > 0) {
        f.seekg(start - 1);
        char prev; f.get(prev);
        if (prev != '\n') {           // we are inside a line: skip it
            char ch;
            while (f.get(ch)) { ++pos; if (ch == '\n') break; }
        }
    }
    f.seekg(pos);

    LineParser lp;
    bool done = false;
    while (!done && f) {
        f.read(buf.data(), buf.size());
        std::streamsize n = f.gcount();
        if (n <= 0) break;
        for (std::streamsize i = 0; i < n; ++i, ++pos) {
            if (!lp.in_line && pos >= end) { done = true; break; }   // next line is not ours
            lp.feed(buf[i]);
        }
    }
    if (!done) lp.end_of_input();      // last line of the file without '\n'
    return lp.counts;
}

static int cmd_1a(const std::string& in, int threads) {
    auto expected = load_expected(in + ".counts");
    auto t0 = Clock::now();
    long long size = file_size(in);
    std::vector<Counts> results(threads);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        long long s = size * t / threads;
        long long e = size * (t + 1) / threads;
        pool.emplace_back([&, t, s, e] { results[t] = count_range(in, s, e); });
    }
    for (auto& th : pool) th.join();
    Counts c;
    for (auto& r : results) c += r;
    double ms = ms_since(t0);
    print_result("task=1a threads=" + std::to_string(threads), ms, c, expected);
    return 0;
}

// ---------------------------------------------------------------------------
// Task 1b: numbers already in memory, three ways to count.
// ---------------------------------------------------------------------------

static std::vector<int32_t> load_array(const std::string& in) {
    std::vector<int32_t> a;
    a.reserve(50000000);
    std::ifstream f(in, std::ios::binary);
    std::vector<char> buf(1 << 22);
    int v = 0; bool neg = false, any = false;
    while (f) {
        f.read(buf.data(), buf.size());
        std::streamsize n = f.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            char ch = buf[i];
            if (ch == '\n') { if (any) a.push_back(neg ? -v : v); v = 0; neg = any = false; }
            else if (ch == '-') neg = true;
            else if (ch >= '0' && ch <= '9') { v = v * 10 + (ch - '0'); any = true; }
        }
    }
    if (any) a.push_back(neg ? -v : v);
    return a;
}

// Version 1: shared counters, NO synchronisation.
// `volatile` only stops the compiler from keeping the counter in a register for
// the whole loop (which would hide the race); every increment is still an
// unsynchronised read-modify-write: load, add 1, store.
struct SharedCounters { long long neg = 0, pos = 0, zero = 0; };

static Counts run_v1(const std::vector<int32_t>& a, int threads) {
    SharedCounters sc;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        size_t s = a.size() * t / threads, e = a.size() * (t + 1) / threads;
        pool.emplace_back([&, s, e] {
            volatile long long* neg = &sc.neg;
            volatile long long* pos = &sc.pos;
            volatile long long* zero = &sc.zero;
            for (size_t i = s; i < e; ++i) {
                int v = a[i];
                if (v < 0) *neg = *neg + 1;
                else if (v > 0) *pos = *pos + 1;
                else *zero = *zero + 1;
            }
        });
    }
    for (auto& th : pool) th.join();
    return Counts{sc.neg, sc.pos, sc.zero};
}

// Version 2: shared counters protected by one mutex.
static Counts run_v2(const std::vector<int32_t>& a, int threads) {
    Counts shared;
    std::mutex m;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        size_t s = a.size() * t / threads, e = a.size() * (t + 1) / threads;
        pool.emplace_back([&, s, e] {
            for (size_t i = s; i < e; ++i) {
                int v = a[i];
                std::lock_guard<std::mutex> lock(m);
                if (v < 0) ++shared.neg; else if (v > 0) ++shared.pos; else ++shared.zero;
            }
        });
    }
    for (auto& th : pool) th.join();
    return shared;
}

// Version 3: each thread has its own counters (local variables); they are
// written once into results[t] and added together after join().
static Counts run_v3(const std::vector<int32_t>& a, int threads) {
    std::vector<Counts> results(threads);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        size_t s = a.size() * t / threads, e = a.size() * (t + 1) / threads;
        pool.emplace_back([&, t, s, e] {
            Counts local;
            for (size_t i = s; i < e; ++i) {
                int v = a[i];
                if (v < 0) ++local.neg; else if (v > 0) ++local.pos; else ++local.zero;
            }
            results[t] = local;
        });
    }
    for (auto& th : pool) th.join();
    Counts c;
    for (auto& r : results) c += r;
    return c;
}

static int cmd_1b(const std::string& in, int threads, const std::string& version, int runs1) {
    auto expected = load_expected(in + ".counts");
    auto tl = Clock::now();
    std::vector<int32_t> a = load_array(in);
    std::fprintf(stderr, "loaded %zu numbers in %.1f ms (not measured)\n", a.size(), ms_since(tl));

    auto run = [&](int ver, int run_no) {
        auto t0 = Clock::now();
        Counts c = ver == 1 ? run_v1(a, threads) : ver == 2 ? run_v2(a, threads) : run_v3(a, threads);
        double ms = ms_since(t0);
        long long lost = 50000000LL - c.total();
        print_result("task=1b version=" + std::to_string(ver) + " run=" + std::to_string(run_no) +
                     " threads=" + std::to_string(threads), ms, c, expected,
                     "lost=" + std::to_string(lost));
    };
    if (version == "1" || version == "all") for (int r = 1; r <= runs1; ++r) run(1, r);
    if (version == "2" || version == "all") run(2, 1);
    if (version == "3" || version == "all") run(3, 1);
    return 0;
}

// ---------------------------------------------------------------------------
// Task 1c: one reader thread, N worker threads, bounded blocking queue.
// ---------------------------------------------------------------------------

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : cap_(capacity) {}

    // Blocks while the queue is full.
    void push(T item) {
        std::unique_lock<std::mutex> lk(m_);
        not_full_.wait(lk, [&] { return q_.size() < cap_; });
        q_.push_back(std::move(item));
        if (q_.size() > max_seen_) max_seen_ = q_.size();
        lk.unlock();
        not_empty_.notify_one();
    }

    // Blocks while the queue is empty and open. Returns nullopt when it is
    // closed AND empty: that is the signal for a worker to stop.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(m_);
        not_empty_.wait(lk, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;
        T item = std::move(q_.front());
        q_.pop_front();
        lk.unlock();
        not_full_.notify_one();
        return item;
    }

    void close() {
        { std::lock_guard<std::mutex> lk(m_); closed_ = true; }
        not_empty_.notify_all();
    }

    size_t max_seen() const { return max_seen_; }

private:
    size_t cap_;
    std::deque<T> q_;
    std::mutex m_;
    std::condition_variable not_full_, not_empty_;
    bool closed_ = false;
    size_t max_seen_ = 0;
};

using Batch = std::vector<std::string>;

static int cmd_1c(const std::string& in, int workers, size_t batch_size, size_t capacity) {
    auto expected = load_expected(in + ".counts");
    auto t0 = Clock::now();
    BoundedQueue<Batch> queue(capacity);
    std::vector<Counts> results(workers);
    std::atomic<long long> reader_wait_ns{0};

    std::thread reader([&] {
        std::ifstream f(in, std::ios::binary);
        std::vector<char> iobuf(1 << 20);
        f.rdbuf()->pubsetbuf(iobuf.data(), iobuf.size());
        Batch batch;
        batch.reserve(batch_size);
        std::string line;
        while (std::getline(f, line)) {                 // line by line
            batch.push_back(std::move(line));
            if (batch.size() == batch_size) {
                auto w = Clock::now();
                queue.push(std::move(batch));           // waits if the queue is full
                reader_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - w).count();
                batch = Batch();
                batch.reserve(batch_size);
            }
        }
        if (!batch.empty()) queue.push(std::move(batch));
        queue.close();                                  // end of file
    });

    std::vector<std::thread> pool;
    for (int t = 0; t < workers; ++t) {
        pool.emplace_back([&, t] {
            Counts local;
            while (auto b = queue.pop()) {              // stops when closed and empty
                for (const std::string& s : *b) {
                    int v = parse_int(s);
                    if (v < 0) ++local.neg; else if (v > 0) ++local.pos; else ++local.zero;
                }
            }
            results[t] = local;
        });
    }
    reader.join();
    for (auto& th : pool) th.join();
    Counts c;
    for (auto& r : results) c += r;
    double ms = ms_since(t0);
    char extra[128];
    std::snprintf(extra, sizeof extra, "reader_push_wait_ms=%.1f max_queue=%zu",
                  reader_wait_ns.load() / 1e6, queue.max_seen());
    print_result("task=1c threads=" + std::to_string(workers), ms, c, expected, extra);
    return 0;
}

// ---------------------------------------------------------------------------
// Task 2: 8 files on a USB drive; a file is the unit of work.
// Each thread reads a whole file in ONE read into one byte buffer, then parses it.
// ---------------------------------------------------------------------------

static std::vector<std::string> list_part_files(const std::string& dir) {
    std::vector<std::string> files;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\numbers_part_*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do files.push_back(dir + "\\" + fd.cFileName); while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("numbers_part_", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".txt")
                files.push_back(dir + "/" + n);
        }
        closedir(d);
    }
#endif
    std::sort(files.begin(), files.end());
    return files;
}

static int cmd_2(const std::string& dir, int threads) {
    auto files = list_part_files(dir);
    if (files.empty()) { std::cerr << "no numbers_part_*.txt files in " << dir << "\n"; return 1; }
    auto expected = load_expected(dir + "/expected.counts");

    auto t0 = Clock::now();
    std::vector<Counts> results(threads);
    std::vector<double> read_ms(threads, 0), parse_ms(threads, 0);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            // Files are given out round-robin: thread t gets files t, t+N, t+2N, ...
            for (size_t i = t; i < files.size(); i += threads) {
                auto r0 = Clock::now();
                FILE* f = std::fopen(files[i].c_str(), "rb");
                if (!f) { std::cerr << "cannot open " << files[i] << "\n"; std::exit(1); }
                std::fseek(f, 0, SEEK_END);
                long long size = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                std::vector<char> buf(static_cast<size_t>(size));
                size_t got = std::fread(buf.data(), 1, buf.size(), f);   // one read operation
                std::fclose(f);
                read_ms[t] += ms_since(r0);

                auto p0 = Clock::now();
                LineParser lp;
                lp.feed(buf.data(), got);
                lp.end_of_input();
                results[t] += lp.counts;
                parse_ms[t] += ms_since(p0);
            }
        });
    }
    for (auto& th : pool) th.join();
    Counts c;
    for (auto& r : results) c += r;
    double ms = ms_since(t0);
    double rs = 0, ps = 0;
    for (int t = 0; t < threads; ++t) { rs += read_ms[t]; ps += parse_ms[t]; }
    char extra[160];
    std::snprintf(extra, sizeof extra, "files=%zu sum_read_ms=%.1f sum_parse_ms=%.1f", files.size(), rs, ps);
    print_result("task=2 threads=" + std::to_string(threads), ms, c, expected, extra);
    return 0;
}

// ---------------------------------------------------------------------------

static int cmd_info() {
    std::printf("hardware_concurrency (logical processors): %u\n", std::thread::hardware_concurrency());
#if defined(__clang__)
    std::printf("compiler: clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    std::printf("compiler: g++ %s\n", __VERSION__);
#elif defined(_MSC_VER)
    std::printf("compiler: MSVC %d\n", _MSC_VER);
#endif
    std::printf("C++ standard: %ld\n", static_cast<long>(__cplusplus));
    return 0;
}

static void usage() {
    std::cerr <<
        "usage:\n"
        "  lab1 gen   --out numbers.txt [--count 50000000] [--seed 42]\n"
        "  lab1 split --in numbers.txt --outdir parts [--parts 8]\n"
        "  lab1 1a    --in numbers.txt --threads N\n"
        "  lab1 1b    --in numbers.txt [--threads N] [--version 1|2|3|all] [--runs1 3]\n"
        "  lab1 1c    --in numbers.txt --threads N [--batch 10000] [--capacity 100]\n"
        "  lab1 2     --dir <folder with numbers_part_*.txt> --threads N\n"
        "  lab1 info\n";
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    std::map<std::string, std::string> opt;
    for (int i = 2; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
    auto get = [&](const std::string& k, const std::string& d) { return opt.count(k) ? opt[k] : d; };
    int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    if (cmd == "gen")   return cmd_gen(get("--out", "numbers.txt"), std::stoll(get("--count", "50000000")),
                                       std::stoull(get("--seed", "42")));
    if (cmd == "split") return cmd_split(get("--in", "numbers.txt"), get("--outdir", "parts"),
                                         std::stoi(get("--parts", "8")));
    if (cmd == "1a")    return cmd_1a(get("--in", "numbers.txt"), std::stoi(get("--threads", "1")));
    if (cmd == "1b")    return cmd_1b(get("--in", "numbers.txt"), std::stoi(get("--threads", std::to_string(hw))),
                                      get("--version", "all"), std::stoi(get("--runs1", "3")));
    if (cmd == "1c")    return cmd_1c(get("--in", "numbers.txt"), std::stoi(get("--threads", "1")),
                                      std::stoul(get("--batch", "10000")), std::stoul(get("--capacity", "100")));
    if (cmd == "2")     return cmd_2(get("--dir", "."), std::stoi(get("--threads", "1")));
    if (cmd == "info")  return cmd_info();
    usage();
    return 1;
}

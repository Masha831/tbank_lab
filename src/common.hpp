#pragma once
// ----------------------------------------------------------------------------
// Общие типы и утилиты: логирование, низкоуровневый файловый ввод/вывод,
// атомарное сложение double.
//
// Вся работа с диском идёт через pread/pwrite с явными смещениями — это
// упрощает контроль над последовательностью обращений и потокобезопасность.
// ----------------------------------------------------------------------------

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

using u8  = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

// Ребро ориентированного графа. Ровно 8 байт — в таком виде рёбра лежат
// во всех бинарных файлах (part_*.bin, row_*.bin, col_*.bin).
struct Edge {
    i32 src;
    i32 dst;
};
static_assert(sizeof(Edge) == 8, "Edge должен занимать 8 байт");

// ---------------------------------------------------------------- логирование

inline double nowSec() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

inline double& startTime() {
    static double t0 = nowSec();
    return t0;
}

// Печать в stderr с отметкой времени от старта программы.
inline void logMsg(const char* fmt, ...) {
    fprintf(stderr, "[%8.2fs] ", nowSec() - startTime());
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

[[noreturn]] inline void die(const std::string& msg) {
    fprintf(stderr, "ОШИБКА: %s\n", msg.c_str());
    exit(1);
}

// ------------------------------------------------------------------ файлы

inline int xopenRead(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) die("не удалось открыть для чтения: " + path);
    return fd;
}

inline int xopenWrite(const std::string& path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("не удалось открыть для записи: " + path);
    return fd;
}

inline u64 xfileSize(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) die("stat не удался: " + path);
    return u64(st.st_size);
}

// Полное чтение len байт со смещения off (pread может вернуть меньше).
inline void xpread(int fd, void* buf, u64 len, u64 off) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        ssize_t r = ::pread(fd, p, len, off_t(off));
        if (r < 0) { if (errno == EINTR) continue; die("pread не удался"); }
        if (r == 0) die("pread: неожиданный конец файла");
        p += r; off += u64(r); len -= u64(r);
    }
}

// Полная запись len байт по смещению off.
inline void xpwrite(int fd, const void* buf, u64 len, u64 off) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        ssize_t r = ::pwrite(fd, p, len, off_t(off));
        if (r < 0) { if (errno == EINTR) continue; die("pwrite не удался"); }
        p += r; off += u64(r); len -= u64(r);
    }
}

// Последовательный буферизованный писатель: накапливает данные в буфере
// фиксированного размера и сбрасывает их одним write(). Используется для
// part/row/col-файлов и итогового CSV.
class ChunkWriter {
public:
    ChunkWriter() = default;

    void open(const std::string& path, u64 bufBytes) {
        path_ = path;
        fd_ = xopenWrite(path);
        buf_.reserve(bufBytes);
        cap_ = bufBytes;
        written_ = 0;
    }

    void put(const void* data, u64 len) {
        const char* p = static_cast<const char*>(data);
        while (len > 0) {
            u64 space = cap_ - buf_.size();
            u64 take = len < space ? len : space;
            buf_.insert(buf_.end(), p, p + take);
            p += take; len -= take;
            if (buf_.size() == cap_) flush();
        }
    }

    void putEdge(const Edge& e) { put(&e, sizeof(Edge)); }

    void flush() {
        if (buf_.empty()) return;
        xpwrite(fd_, buf_.data(), buf_.size(), written_);
        written_ += buf_.size();
        buf_.clear();
    }

    void close() {
        if (fd_ < 0) return;
        flush();
        ::close(fd_);
        fd_ = -1;
    }

    u64 bytesWritten() const { return written_ + buf_.size(); }
    const std::string& path() const { return path_; }

    ~ChunkWriter() { close(); }

    ChunkWriter(const ChunkWriter&) = delete;
    ChunkWriter& operator=(const ChunkWriter&) = delete;
    ChunkWriter(ChunkWriter&&) = default;
    ChunkWriter& operator=(ChunkWriter&&) = default;

private:
    std::string path_;
    std::vector<char> buf_;
    u64 cap_ = 0;
    u64 written_ = 0;
    int fd_ = -1;
};

// ------------------------------------------------- атомарное сложение double

// std::atomic<double>::fetch_add появился только в C++20, поэтому реализуем
// сложение через CAS-цикл (переносимо в C++17). На x64 atomic<double>
// lock-free, так что это обычный цикл compare-exchange.
inline void atomicAddDouble(std::atomic<double>& a, double v) {
    double cur = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(cur, cur + v,
                                    std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
        // cur обновляется автоматически при неудаче
    }
}

// ----------------------------------------------------------- битовые операции

inline bool getBit(const u8* bits, u64 i) { return (bits[i >> 3] >> (i & 7)) & 1; }
inline void setBit(u8* bits, u64 i)       { bits[i >> 3] |= u8(1u << (i & 7)); }

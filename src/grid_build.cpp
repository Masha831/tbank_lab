// ----------------------------------------------------------------------------
// Фаза B: построение сеточного (grid) представления графа на диске.
//
// B1. Рёбра из part-файлов раскладываются по интервалу ИСТОЧНИКА в P
//     row-файлов (row_<i>.bin). Это поразрядная внешняя сортировка по
//     старшему ключу: один последовательный проход, O(E) чтения и записи.
//
// B2. Каждый row_<i>.bin обрабатывается по очереди:
//       * считается слайс исходящих степеней outdeg[i*S ... ) — влезает в
//         память, т.к. все источники строки лежат в одном интервале;
//       * ставятся биты существования для источников (тот же интервал);
//       * рёбра раскладываются по интервалу ПРИЁМНИКА в P col-файлов;
//         внутри col_<j>.bin рёбра оказываются сгруппированы по строкам
//         в порядке возрастания i — ровно то, что нужно итерациям.
//
// B3. Существование приёмников: колонки читаются последовательно, биты
//     ставятся в слайс (все приёмники колонки j лежат в одном интервале),
//     затем слайс OR-ится с битами источников в exist.bin.
//
// Разбиение сознательно однопоточное: это чистый memcpy-конвейер,
// ограниченный пропускной способностью диска, а не CPU (см. README).
// ----------------------------------------------------------------------------

#include "grid_build.hpp"

#include <memory>

namespace {

constexpr u64 READ_BUF = 8u << 20;

// Последовательное прогон файла рёбер: cb вызывается для порций Edge[].
template <typename CB>
void streamEdges(const std::string& path, std::vector<char>& buf, CB cb) {
    const u64 size = xfileSize(path);
    if (size % sizeof(Edge) != 0)
        die("повреждённый файл рёбер (размер не кратен 8): " + path);
    int fd = xopenRead(path);
    for (u64 off = 0; off < size;) {
        u64 want = std::min<u64>(buf.size(), size - off);
        xpread(fd, buf.data(), want, off);
        cb(reinterpret_cast<const Edge*>(buf.data()), want / sizeof(Edge));
        off += want;
    }
    ::close(fd);
}

std::string rowPath(const std::string& wd, u64 i) {
    return wd + "/row_" + std::to_string(i) + ".bin";
}

} // namespace

std::string colPath(const std::string& wd, u64 j) {
    return wd + "/col_" + std::to_string(j) + ".bin";
}

Grid buildGrid(const std::vector<std::string>& parts, const Plan& plan,
               const std::string& workdir) {
    const u64 P = plan.P, S = plan.S, V = plan.V;
    std::vector<char> readBuf(READ_BUF);

    // ---------------- B1: раскладка по интервалу источника ----------------
    logMsg("фаза B1: раскладка рёбер по %llu интервалам источников",
           (unsigned long long)P);
    {
        std::vector<ChunkWriter> rows(P);
        for (u64 i = 0; i < P; ++i)
            rows[i].open(rowPath(workdir, i), plan.writeBufBytes);

        for (const auto& part : parts) {
            streamEdges(part, readBuf, [&](const Edge* es, u64 n) {
                for (u64 k = 0; k < n; ++k)
                    rows[plan.sliceOf(es[k].src)].putEdge(es[k]);
            });
            ::unlink(part.c_str()); // part-файл больше не нужен
        }
        for (auto& w : rows) w.close();
    }

    // ------- B2: степени, существование источников, раскладка по колонкам -
    logMsg("фаза B2: подсчёт степеней и раскладка по колонкам");
    Grid grid;
    grid.colIndex.assign(P, {});
    {
        int outdegFd = xopenWrite(workdir + "/outdeg.bin");
        int existFd = xopenWrite(workdir + "/exist.bin");

        std::vector<ChunkWriter> cols(P);
        for (u64 j = 0; j < P; ++j)
            cols[j].open(colPath(workdir, j), plan.writeBufBytes);

        std::vector<u32> outdeg(S);
        std::vector<u8> exist(S / 8);
        std::vector<u64> rowCount(P); // рёбер строки i в колонку j

        for (u64 i = 0; i < P; ++i) {
            const u64 base = i * S, len = plan.sliceLen(i);
            std::fill(outdeg.begin(), outdeg.end(), 0);
            std::fill(exist.begin(), exist.end(), 0);
            std::fill(rowCount.begin(), rowCount.end(), 0);

            streamEdges(rowPath(workdir, i), readBuf,
                        [&](const Edge* es, u64 n) {
                for (u64 k = 0; k < n; ++k) {
                    const u64 local = u64(u32(es[k].src)) - base;
                    if (outdeg[local] == UINT32_MAX)
                        die("переполнение исходящей степени (u32)");
                    outdeg[local]++;
                    setBit(exist.data(), local);
                    const u64 j = plan.sliceOf(es[k].dst);
                    cols[j].putEdge(es[k]);
                    rowCount[j]++;
                }
            });
            ::unlink(rowPath(workdir, i).c_str());

            for (u64 j = 0; j < P; ++j)
                if (rowCount[j] > 0)
                    grid.colIndex[j].push_back(Segment{u32(i), rowCount[j]});

            xpwrite(outdegFd, outdeg.data(), len * 4, base * 4);
            xpwrite(existFd, exist.data(), (len + 7) / 8, base / 8);
        }
        for (auto& w : cols) w.close();
        ::close(outdegFd);
        ::close(existFd);
    }

    // ------------- B3: существование приёмников + подсчёт N ----------------
    logMsg("фаза B3: битовая карта существования вершин");
    {
        int existFd = ::open((workdir + "/exist.bin").c_str(), O_RDWR);
        if (existFd < 0) die("не удалось открыть exist.bin");

        std::vector<u8> mine(S / 8), disk(S / 8);
        for (u64 j = 0; j < P; ++j) {
            const u64 base = j * S, len = plan.sliceLen(j);
            const u64 bytes = (len + 7) / 8;
            std::fill(mine.begin(), mine.end(), 0);
            streamEdges(colPath(workdir, j), readBuf,
                        [&](const Edge* es, u64 n) {
                for (u64 k = 0; k < n; ++k)
                    setBit(mine.data(), u64(u32(es[k].dst)) - base);
            });
            xpread(existFd, disk.data(), bytes, base / 8);
            u64 real = 0;
            for (u64 b = 0; b < bytes; ++b) {
                disk[b] |= mine[b];
                real += u64(__builtin_popcount(disk[b]));
            }
            grid.numReal += real;
            xpwrite(existFd, disk.data(), bytes, base / 8);
        }
        ::close(existFd);
    }

    logMsg("фаза B: готово. V-пространство = %llu, реальных вершин N = %llu",
           (unsigned long long)V, (unsigned long long)grid.numReal);
    if (grid.numReal == 0) die("в графе нет вершин");
    return grid;
}

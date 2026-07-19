// ----------------------------------------------------------------------------
// Вычисление LeaderRank во внешней памяти.
//
// Модель LeaderRank (Lü, Zhang, Yeung, Zhou, 2011): к графу добавляется
// ground-вершина g, соединённая двунаправленными рёбрами со всеми N
// вершинами, и по расширенному графу запускается случайное блуждание:
//
//     s_{t+1}(v) = sum_{u -> v} s_t(u) / outdeg'(u),
//
// где outdeg'(u) = outdeg(u) + 1 (ребро в ground). После сходимости очки
// ground распределяются поровну: final(v) = s(v) + s(g)/N.
//
// Ground-вершина НЕ материализуется (это было бы +2N рёбер на диске),
// а учитывается аналитически:
//   * вклад g в каждую вершину: s(g)/N — добавляется при инициализации
//     next-слайса;
//   * приток в g: sum_v s(v)/outdeg'(v) = сумма всех contrib — считается
//     попутно на contrib-проходе.
//
// Одна итерация:
//   1) contrib-проход (последовательный, поэлементный, маленькие буферы):
//      contrib[v] = rank[v] / (outdeg[v] + 1);  g_next += contrib[v].
//   2) обход колонок j = 0..P-1:
//        next-слайс j инициализируется s(g)/N для существующих вершин;
//        файл col_<j>.bin читается строго последовательно; на границе
//        сегмента подгружается contrib-слайс очередной строки i;
//        рёбра обрабатываются параллельно: атомарное
//        next[dst] += contrib[src].
//        Чтение рёбер двойной буферизацией перекрывается с вычислением.
//      Финализация колонки: параллельно со сравнением со старым rank-слайсом
//      (для L1-дельты и контроля суммарной массы) next пишется в новый
//      rank-файл. Файлы rank_0/rank_1 чередуются (ping-pong).
//   3) s(g) обновляется, дельта и масса логируются; выход по tol.
//
// Инвариант для контроля корректности: матрица блуждания стохастическая,
// суммарная масса (sum_v rank[v]) + s(g) обязана оставаться равной 1
// с точностью до ошибок округления double (печатается каждую итерацию).
// ----------------------------------------------------------------------------

#include "leaderrank.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <future>
#include <memory>

namespace {

constexpr u64 PASS_CHUNK = 256u << 10; // вершин на порцию в поэлементных проходах
constexpr u64 EDGE_GRAIN = 32u << 10; // рёбер на задачу в parallelFor

std::string rankPath(const std::string& wd, int idx) {
    return wd + "/rank_" + std::to_string(idx) + ".bin";
}

// Поэлементный проход contrib[v] = rank[v] / (outdeg[v] + 1).
// Возвращает сумму всех contrib (приток ground-вершины).
// Работает порциями по PASS_CHUNK вершин: память O(мегабайты), I/O
// последовательный. Суммирование по порциям (по сути pairwise) удерживает
// ошибку накопления на уровне ~1e-12 даже для миллиардов слагаемых.
double contribPass(const std::string& wd, const Plan& plan,
                   const std::string& rankFile) {
    int rankFd = xopenRead(rankFile);
    int degFd = xopenRead(wd + "/outdeg.bin");
    int contribFd = xopenWrite(wd + "/contrib.bin");

    std::vector<double> r(PASS_CHUNK), c(PASS_CHUNK);
    std::vector<u32> deg(PASS_CHUNK);
    double total = 0;
    for (u64 v = 0; v < plan.V; v += PASS_CHUNK) {
        const u64 n = std::min(PASS_CHUNK, plan.V - v);
        xpread(rankFd, r.data(), n * 8, v * 8);
        xpread(degFd, deg.data(), n * 4, v * 4);
        double local = 0;
        for (u64 k = 0; k < n; ++k) {
            c[k] = r[k] / double(deg[k] + 1.0);
            local += c[k];
        }
        total += local;
        xpwrite(contribFd, c.data(), n * 8, v * 8);
    }
    ::close(rankFd);
    ::close(degFd);
    ::close(contribFd);
    return total;
}

} // namespace

LrResult computeLeaderRank(const Grid& grid, const Plan& plan,
                           const std::string& workdir, ThreadPool& pool,
                           const LrParams& params) {
    const u64 P = plan.P, S = plan.S;
    const double N = double(grid.numReal);

    if (!std::atomic<double>{}.is_lock_free())
        logMsg("предупреждение: atomic<double> не lock-free на этой платформе");

    // -- инициализация: s(v) = 1/(N+1) для реальных вершин, s(g) = 1/(N+1) --
    // Суммарная масса равна 1, поэтому итоговые очки нормированы: sum = 1.
    {
        int existFd = xopenRead(workdir + "/exist.bin");
        int rankFd = xopenWrite(rankPath(workdir, 0));
        const double init = 1.0 / (N + 1.0);
        std::vector<u8> ex(PASS_CHUNK / 8);
        std::vector<double> r(PASS_CHUNK);
        for (u64 v = 0; v < plan.V; v += PASS_CHUNK) {
            const u64 n = std::min(PASS_CHUNK, plan.V - v);
            xpread(existFd, ex.data(), (n + 7) / 8, v / 8);
            for (u64 k = 0; k < n; ++k)
                r[k] = getBit(ex.data(), k) ? init : 0.0;
            xpwrite(rankFd, r.data(), n * 8, v * 8);
        }
        ::close(existFd);
        ::close(rankFd);
    }
    double sg = 1.0 / (N + 1.0);

    // Крупные буферы аллоцируются один раз и переиспользуются между
    // колонками и итерациями — это и есть учитываемый бюджет памяти.
    auto next = std::make_unique<std::atomic<double>[]>(S);
    std::vector<double> contrib(S);
    std::vector<u8> existSlice(S / 8);
    std::vector<char> edgeBufA(plan.edgeBufBytes), edgeBufB(plan.edgeBufBytes);
    std::vector<double> oldChunk(PASS_CHUNK), newChunk(PASS_CHUNK);

    int contribFd = -1, existFd = xopenRead(workdir + "/exist.bin");

    LrResult res;
    int cur = 0; // индекс текущего rank-файла (ping-pong)

    for (u32 iter = 1; iter <= params.maxIters; ++iter) {
        const double t0 = nowSec();

        // ---- шаг 1: contrib-проход --------------------------------------
        const double gNext = contribPass(workdir, plan, rankPath(workdir, cur));
        if (contribFd >= 0) ::close(contribFd);
        contribFd = xopenRead(workdir + "/contrib.bin");

        const double addG = sg / N; // вклад ground в каждую вершину
        double delta = 0, mass = 0;

        int rankOldFd = xopenRead(rankPath(workdir, cur));
        int rankNewFd = xopenWrite(rankPath(workdir, 1 - cur));

        // ---- шаг 2: обход колонок ---------------------------------------
        for (u64 j = 0; j < P; ++j) {
            const u64 baseJ = j * S, lenJ = plan.sliceLen(j);

            // Инициализация next-слайса вкладом ground (только реальным).
            xpread(existFd, existSlice.data(), (lenJ + 7) / 8, baseJ / 8);
            parallelFor(pool, lenJ, PASS_CHUNK, [&](u64 b, u64 e) {
                for (u64 k = b; k < e; ++k)
                    next[k].store(getBit(existSlice.data(), k) ? addG : 0.0,
                                  std::memory_order_relaxed);
            });

            // Последовательный поток рёбер колонки с двойной буферизацией.
            int colFd = xopenRead(colPath(workdir, j));
            u64 fileOff = 0;
            for (const Segment& seg : grid.colIndex[j]) {
                const u64 baseI = u64(seg.row) * S;
                xpread(contribFd, contrib.data(), plan.sliceLen(seg.row) * 8,
                       baseI * 8);

                u64 remaining = seg.count * sizeof(Edge);
                auto readNext = [&](char* dst) -> u64 {
                    u64 want = std::min<u64>(plan.edgeBufBytes, remaining);
                    xpread(colFd, dst, want, fileOff);
                    fileOff += want;
                    remaining -= want;
                    return want;
                };

                char* curBuf = edgeBufA.data();
                char* nxtBuf = edgeBufB.data();
                u64 curLen = readNext(curBuf);
                while (curLen > 0) {
                    // Пока считаем текущий буфер, фоновым потоком читаем
                    // следующий (перекрытие I/O и CPU).
                    std::future<u64> pending;
                    if (remaining > 0)
                        pending = std::async(std::launch::async,
                                             [&readNext, nxtBuf] {
                                                 return readNext(nxtBuf);
                                             });

                    const Edge* es = reinterpret_cast<const Edge*>(curBuf);
                    const u64 n = curLen / sizeof(Edge);
                    parallelFor(pool, n, EDGE_GRAIN, [&](u64 b, u64 e) {
                        for (u64 k = b; k < e; ++k)
                            atomicAddDouble(next[u64(u32(es[k].dst)) - baseJ],
                                            contrib[u64(u32(es[k].src)) - baseI]);
                    });

                    if (pending.valid()) {
                        curLen = pending.get();
                        std::swap(curBuf, nxtBuf);
                    } else {
                        curLen = 0;
                    }
                }
            }
            ::close(colFd);

            // Финализация колонки: дельта, масса, запись нового rank-слайса.
            for (u64 v = 0; v < lenJ; v += PASS_CHUNK) {
                const u64 n = std::min(PASS_CHUNK, lenJ - v);
                xpread(rankOldFd, oldChunk.data(), n * 8, (baseJ + v) * 8);
                double dLocal = 0, mLocal = 0;
                for (u64 k = 0; k < n; ++k) {
                    const double nv = next[v + k].load(std::memory_order_relaxed);
                    newChunk[k] = nv;
                    dLocal += std::fabs(nv - oldChunk[k]);
                    mLocal += nv;
                }
                delta += dLocal;
                mass += mLocal;
                xpwrite(rankNewFd, newChunk.data(), n * 8, (baseJ + v) * 8);
            }
        }
        ::close(rankOldFd);
        ::close(rankNewFd);

        // ---- шаг 3: ground-вершина и критерий останова -------------------
        delta += std::fabs(gNext - sg);
        sg = gNext;
        mass += sg;
        cur = 1 - cur;

        logMsg("итерация %3u: L1-дельта = %.3e, масса = %.12f, %.2f с",
               iter, delta, mass, nowSec() - t0);

        res.iters = iter;
        res.delta = delta;
        if (delta < params.tol) break;
    }
    if (res.delta >= params.tol)
        logMsg("предупреждение: достигнут лимит итераций до сходимости к tol");

    ::close(contribFd);
    ::close(existFd);

    res.ground = sg;
    res.rankFile = rankPath(workdir, cur);
    return res;
}

void writeOutput(const Grid& grid, const Plan& plan, const LrResult& lr,
                 const std::string& workdir, const std::string& outPath) {
    logMsg("выгрузка результата в %s", outPath.c_str());

    const double share = lr.ground / double(grid.numReal);
    int rankFd = xopenRead(lr.rankFile);
    int existFd = xopenRead(workdir + "/exist.bin");

    ChunkWriter out;
    out.open(outPath, 8u << 20);
    out.put("vertex,rank\n", 12);

    // Попутно собираем топ-10 для отчёта в лог.
    std::vector<std::pair<double, i64>> top;

    std::vector<double> r(PASS_CHUNK);
    std::vector<u8> ex(PASS_CHUNK / 8);
    char line[64];
    for (u64 v = 0; v < plan.V; v += PASS_CHUNK) {
        const u64 n = std::min(PASS_CHUNK, plan.V - v);
        xpread(rankFd, r.data(), n * 8, v * 8);
        xpread(existFd, ex.data(), (n + 7) / 8, v / 8);
        for (u64 k = 0; k < n; ++k) {
            if (!getBit(ex.data(), k)) continue; // id-«дыры» не выводим
            const double score = r[k] + share;

            char* p = line;
            auto rc1 = std::to_chars(p, line + sizeof(line), i64(v + k));
            p = rc1.ptr;
            *p++ = ',';
            auto rc2 = std::to_chars(p, line + sizeof(line), score);
            p = rc2.ptr;
            *p++ = '\n';
            out.put(line, u64(p - line));

            top.emplace_back(score, i64(v + k));
            if (top.size() > 4096) { // периодически усечём до топ-10
                std::partial_sort(top.begin(), top.begin() + 10, top.end(),
                                  std::greater<>());
                top.resize(10);
            }
        }
    }
    out.close();
    ::close(rankFd);
    ::close(existFd);

    std::partial_sort(top.begin(),
                      top.begin() + std::min<size_t>(10, top.size()),
                      top.end(), std::greater<>());
    top.resize(std::min<size_t>(10, top.size()));
    logMsg("топ-10 вершин по LeaderRank:");
    for (auto& [score, v] : top)
        logMsg("    вершина %lld: %.10g", (long long)v, score);
}

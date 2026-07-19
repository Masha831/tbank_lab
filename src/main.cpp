// ----------------------------------------------------------------------------
// graphrank — вычисление LeaderRank для ориентированного графа, который
// не помещается в оперативную память.
//
// Конвейер: CSV -> бинарные рёбра -> сетка P x P на диске -> итерации
// LeaderRank со стримингом рёбер -> CSV "vertex,rank".
//
// Использование:
//   graphrank --input edges.csv --output leader_rank.csv
//             [--workdir DIR] [--memory-mb N] [--threads N]
//             [--tol X] [--max-iters N] [--slice-size N] [--keep-workdir]
// ----------------------------------------------------------------------------

#include <filesystem>
#include <string>
#include <thread>

#include <malloc.h>
#include <sys/resource.h>

#include "common.hpp"
#include "csv_import.hpp"
#include "grid_build.hpp"
#include "leaderrank.hpp"
#include "plan.hpp"
#include "thread_pool.hpp"

namespace fs = std::filesystem;

namespace {

struct Config {
    std::string input, output;
    std::string workdir = "./graphrank_tmp";
    u64 memoryMB = 1024;
    unsigned threads = std::thread::hardware_concurrency();
    double tol = 1e-9;
    u32 maxIters = 100;
    u64 sliceSize = 0; // 0 = автоматически из бюджета памяти
    bool keepWorkdir = false;
};

void usage() {
    fprintf(stderr,
        "graphrank — LeaderRank для графов, не помещающихся в память\n\n"
        "Обязательные аргументы:\n"
        "  --input PATH      входной CSV/TSV с рёбрами (from,to)\n"
        "  --output PATH     выходной CSV (vertex,rank)\n\n"
        "Необязательные:\n"
        "  --workdir DIR     каталог для временных файлов (по умолч. ./graphrank_tmp)\n"
        "  --memory-mb N     бюджет памяти в МБ, минимум 64 (по умолч. 1024)\n"
        "  --threads N       число потоков (по умолч. все CPU)\n"
        "  --tol X           порог сходимости, L1 (по умолч. 1e-9)\n"
        "  --max-iters N     максимум итераций (по умолч. 100)\n"
        "  --slice-size N    принудительный размер слайса в вершинах (для тестов)\n"
        "  --keep-workdir    не удалять временные файлы\n");
    exit(2);
}

Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) die("нет значения для аргумента " + a);
            return argv[++i];
        };
        if (a == "--input") c.input = val();
        else if (a == "--output") c.output = val();
        else if (a == "--workdir") c.workdir = val();
        else if (a == "--memory-mb") c.memoryMB = std::stoull(val());
        else if (a == "--threads") c.threads = unsigned(std::stoul(val()));
        else if (a == "--tol") c.tol = std::stod(val());
        else if (a == "--max-iters") c.maxIters = u32(std::stoul(val()));
        else if (a == "--slice-size") c.sliceSize = std::stoull(val());
        else if (a == "--keep-workdir") c.keepWorkdir = true;
        else if (a == "--help" || a == "-h") usage();
        else die("неизвестный аргумент: " + a);
    }
    if (c.input.empty() || c.output.empty()) usage();
    if (c.threads == 0) c.threads = 1;
    return c;
}

void raiseFileLimit() {
    // До P bucket-файлов открыто одновременно — поднимем мягкий лимит.
    rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 8192) {
        rl.rlim_cur = std::min<rlim_t>(8192, rl.rlim_max);
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

} // namespace

int main(int argc, char** argv) {
    startTime(); // фиксируем t0 для логов
    Config cfg = parseArgs(argc, argv);
    raiseFileLimit();

    logMsg("graphrank: input=%s, memory=%llu МБ, threads=%u",
           cfg.input.c_str(), (unsigned long long)cfg.memoryMB, cfg.threads);

    std::error_code ec;
    fs::create_directories(cfg.workdir, ec);
    if (ec) die("не удалось создать workdir: " + cfg.workdir);

    ThreadPool pool(cfg.threads);

    // Фаза A: CSV -> бинарные рёбра (параллельный разбор).
    ImportResult imp = importCsv(cfg.input, cfg.workdir, pool);
    malloc_trim(0); // вернуть ОС буферы разбора: они больше не нужны

    // Планирование памяти: размер слайса и число интервалов.
    Plan plan = makePlan(u64(imp.maxId) + 1, imp.edges, cfg.memoryMB,
                         cfg.sliceSize);
    logMsg("план: V = %llu, E = %llu, слайс S = %llu вершин, интервалов P = %llu%s",
           (unsigned long long)plan.V, (unsigned long long)plan.E,
           (unsigned long long)plan.S, (unsigned long long)plan.P,
           plan.P == 1 ? " (полу-внешний режим: вершинные массивы в RAM)" : "");

    // Фаза B: сетка P x P + степени + существование.
    Grid grid = buildGrid(imp.parts, plan, cfg.workdir);
    malloc_trim(0); // вернуть ОС буферы разбиения

    // Итерации LeaderRank.
    LrParams params{cfg.tol, cfg.maxIters};
    LrResult lr = computeLeaderRank(grid, plan, cfg.workdir, pool, params);
    logMsg("сходимость за %u итераций, финальная L1-дельта = %.3e",
           lr.iters, lr.delta);

    // Выгрузка результата.
    writeOutput(grid, plan, lr, cfg.workdir, cfg.output);

    if (!cfg.keepWorkdir) {
        fs::remove_all(cfg.workdir, ec);
        if (ec) logMsg("предупреждение: не удалось удалить workdir");
    }

    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    logMsg("готово. Пиковый RSS процесса: %.1f МБ (бюджет %llu МБ)",
           double(ru.ru_maxrss) / 1024.0, (unsigned long long)cfg.memoryMB);
    return 0;
}

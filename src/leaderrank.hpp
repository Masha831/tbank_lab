#pragma once
// Итеративное вычисление LeaderRank поверх сеточного представления графа.
// Подробности реализации — в leaderrank.cpp.

#include <string>

#include "grid_build.hpp"
#include "plan.hpp"
#include "thread_pool.hpp"

struct LrParams {
    double tol = 1e-9; // порог сходимости: L1-норма изменения всего состояния
    u32 maxIters = 100;
};

struct LrResult {
    u32 iters = 0;
    double delta = 0;    // L1-изменение на последней итерации
    double ground = 0;   // очки ground-вершины после сходимости
    std::string rankFile;// файл со сходившимися очками s(v), double[V]
};

LrResult computeLeaderRank(const Grid& grid, const Plan& plan,
                           const std::string& workdir, ThreadPool& pool,
                           const LrParams& params);

// Финализация LeaderRank (final = s(v) + s_g / N) и выгрузка в CSV
// "vertex,rank" по возрастанию id. Заодно печатает топ-10 вершин.
void writeOutput(const Grid& grid, const Plan& plan, const LrResult& lr,
                 const std::string& workdir, const std::string& outPath);

std::string colPath(const std::string& workdir, u64 j);

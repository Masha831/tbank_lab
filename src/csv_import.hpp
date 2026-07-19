#pragma once
// Фаза A: параллельный разбор входного CSV в бинарные файлы рёбер.
// Подробности реализации — в csv_import.cpp.

#include <string>
#include <vector>

#include "common.hpp"
#include "thread_pool.hpp"

struct ImportResult {
    std::vector<std::string> parts; // бинарные части (по одной на поток)
    u64 edges = 0;                  // всего рёбер
    i64 maxId = -1;                 // максимальный id вершины
};

// Разбирает CSV/TSV файл с рёбрами (первые две колонки: from, to) и пишет
// рёбра в бинарном виде (int32,int32) в part-файлы внутри workdir.
ImportResult importCsv(const std::string& csvPath, const std::string& workdir,
                       ThreadPool& pool);

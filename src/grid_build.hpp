#pragma once
// Фаза B: раскладка рёбер в сетку P x P на диске + подсчёт исходящих
// степеней и битовой карты существования вершин.
// Подробности реализации — в grid_build.cpp.

#include <string>
#include <vector>

#include "common.hpp"
#include "plan.hpp"

// Сегмент внутри файла колонки: непрерывный блок рёбер (row -> колонка j).
// Сегменты лежат в файле подряд в порядке возрастания row, поэтому смещения
// не храним — они восстанавливаются при последовательном чтении.
struct Segment {
    u32 row;   // индекс интервала источников
    u64 count; // число рёбер в сегменте
};

struct Grid {
    // colIndex[j] — список сегментов файла col_<j>.bin в порядке следования.
    std::vector<std::vector<Segment>> colIndex;
    u64 numReal = 0; // число реально существующих вершин (N)
};

// Строит на диске в workdir:
//   col_<j>.bin — рёбра колонки j, сгруппированные по интервалу источника;
//   outdeg.bin  — u32[V], исходящие степени;
//   exist.bin   — битовая карта V бит: вершина встречается в графе.
// Part-файлы и промежуточные row-файлы удаляются по мере использования.
Grid buildGrid(const std::vector<std::string>& parts, const Plan& plan,
               const std::string& workdir);

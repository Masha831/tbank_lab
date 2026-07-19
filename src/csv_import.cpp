// ----------------------------------------------------------------------------
// Фаза A: CSV -> бинарные рёбра.
//
// Разбор текста — самая CPU-ёмкая часть препроцессинга (для многогигабайтных
// CSV это минуты в один поток), поэтому именно она распараллелена:
//
//   * файл делится на T примерно равных байтовых диапазонов;
//   * поток обрабатывает все строки, НАЧИНАЮЩИЕСЯ в его диапазоне
//     (строка начинается в позиции p, если p == 0 или байт p-1 == '\n');
//   * последняя строка диапазона может выходить за его границу — поток
//     дочитывает её до конца; следующий поток эту строку пропускает,
//     т.к. её начало лежит вне его диапазона.
//
// Такое правило даёт разбиение всех строк между потоками без пересечений
// и пропусков, независимо от того, как границы диапазонов режут строки.
//
// Каждый поток пишет свой part_<t>.bin — никакой синхронизации на записи.
//
// Формат входа (либерален специально, чтобы напрямую читать датасеты SNAP):
//   * разделители: запятая, точка с запятой, табуляция, пробел;
//   * строки, начинающиеся с '#', и пустые строки игнорируются;
//   * первая строка файла может быть заголовком ("from,to") — определяется
//     автоматически;
//   * id вершин: целые в диапазоне [0, INT32_MAX];
//   * лишние колонки после первых двух игнорируются.
// ----------------------------------------------------------------------------

#include "csv_import.hpp"

#include <algorithm>
#include <memory>

namespace {

constexpr u64 READ_CHUNK = 4u << 20; // размер порции чтения текста

// Разбор беззнакового int32. Возвращает указатель за последней цифрой
// либо nullptr при ошибке.
const char* parseId(const char* p, const char* end, i32& out) {
    if (p == end || *p < '0' || *p > '9') return nullptr;
    u64 v = 0;
    while (p != end && *p >= '0' && *p <= '9') {
        v = v * 10 + u64(*p - '0');
        if (v > u64(INT32_MAX)) return nullptr;
        ++p;
    }
    out = i32(v);
    return p;
}

bool isSep(char c) { return c == ',' || c == ';' || c == '\t' || c == ' '; }

// Состояние одного потока-парсера.
struct LocalState {
    ChunkWriter out;
    u64 edges = 0;
    i64 maxId = -1;
    bool headerAllowed = false; // только поток 0 до первой успешной строки
    std::string error;          // непустая строка => фатальная ошибка разбора
};

// Разбор одной строки [ls, le) (без завершающего '\n').
// Возвращает false, если строка пропущена (комментарий/пустая/заголовок).
bool handleLine(const char* ls, const char* le, LocalState& st, u64 lineOffset) {
    if (le > ls && le[-1] == '\r') --le;              // CRLF
    while (ls != le && (*ls == ' ' || *ls == '\t')) ++ls;
    if (ls == le) return false;                       // пустая строка
    if (*ls == '#') return false;                     // комментарий (SNAP)

    i32 a, b;
    const char* p = parseId(ls, le, a);
    if (p) {
        const char* q = p;
        while (q != le && isSep(*q)) ++q;
        if (q != p && (q = parseId(q, le, b)) != nullptr) {
            st.headerAllowed = false;
            st.edges++;
            if (a > st.maxId) st.maxId = a;
            if (b > st.maxId) st.maxId = b;
            Edge e{a, b};
            st.out.putEdge(e);
            return true;
        }
    }
    if (st.headerAllowed) {       // первая содержательная строка файла —
        st.headerAllowed = false; // считаем её заголовком и пропускаем
        return false;
    }
    st.error = "не удалось разобрать строку по смещению " +
               std::to_string(lineOffset) + ": '" +
               std::string(ls, size_t(std::min<i64>(le - ls, 80))) + "'";
    return false;
}

// Обработка диапазона файла [begin, end): все строки, начинающиеся в нём.
void parseRegion(int fd, u64 fileSize, u64 begin, u64 end, LocalState& st) {
    if (begin >= fileSize) return;

    std::vector<char> buf;
    buf.reserve(READ_CHUNK * 2);
    // Строка «начинается» в позиции p, если p == 0 или байт p-1 == '\n'.
    // Чтобы не пропустить строку, начинающуюся ровно в begin, чтение
    // стартует с байта begin-1: если он '\n', первая строка — наша.
    u64 bufBase = (begin == 0) ? 0 : begin - 1; // позиция файла для buf[0]
    u64 cursor = 0;        // текущая позиция разбора внутри buf

    // Дочитываем данные в хвост буфера; возвращает false на конце файла.
    auto refill = [&]() -> bool {
        u64 fileOff = bufBase + buf.size();
        if (fileOff >= fileSize) return false;
        u64 want = std::min(READ_CHUNK, fileSize - fileOff);
        size_t old = buf.size();
        buf.resize(old + want);
        xpread(fd, buf.data() + old, want, fileOff);
        return true;
    };

    // Выравнивание на начало строки: находим первый '\n' начиная с
    // позиции begin-1; следующий за ним байт — первая строка потока.
    if (begin != 0) {
        for (;;) {
            while (cursor < buf.size() && buf[cursor] != '\n') ++cursor;
            if (cursor < buf.size()) { ++cursor; break; }
            // Выкидываем просмотренное и читаем дальше.
            bufBase += buf.size();
            buf.clear();
            cursor = 0;
            if (!refill()) return; // '\n' до конца файла не встретился
        }
    }

    // Основной цикл: строка принадлежит потоку, если её начало < end.
    for (;;) {
        u64 lineStartFile = bufBase + cursor;
        if (lineStartFile >= end || lineStartFile >= fileSize) return;

        // Ищем конец строки, при необходимости дочитывая файл.
        u64 nl = cursor;
        for (;;) {
            while (nl < buf.size() && buf[nl] != '\n') ++nl;
            if (nl < buf.size()) break;
            if (!refill()) break; // последняя строка файла без '\n'
        }

        handleLine(buf.data() + cursor, buf.data() + nl, st, lineStartFile);
        if (!st.error.empty()) return;

        cursor = std::min<u64>(nl + 1, buf.size());

        // Компактизация буфера, чтобы он не рос бесконечно.
        if (cursor >= READ_CHUNK) {
            buf.erase(buf.begin(), buf.begin() + cursor);
            bufBase += cursor;
            cursor = 0;
        }
        if (cursor >= buf.size()) {
            bufBase += buf.size();
            buf.clear();
            cursor = 0;
            if (!refill()) return;
        }
    }
}

} // namespace

ImportResult importCsv(const std::string& csvPath, const std::string& workdir,
                       ThreadPool& pool) {
    const u64 fileSize = xfileSize(csvPath);
    if (fileSize == 0) die("входной файл пуст: " + csvPath);
    const unsigned T = pool.size();

    logMsg("фаза A: разбор %s (%.1f МБ) в %u поток(ов)",
           csvPath.c_str(), double(fileSize) / (1 << 20), T);

    std::vector<LocalState> states(T);
    for (unsigned t = 0; t < T; ++t) {
        states[t].out.open(workdir + "/part_" + std::to_string(t) + ".bin",
                           8u << 20);
        states[t].headerAllowed = (t == 0);
    }

    int fd = xopenRead(csvPath);
    const u64 span = (fileSize + T - 1) / T;
    pool.run([&](unsigned t) {
        u64 begin = u64(t) * span;
        u64 end = std::min(fileSize, begin + span);
        parseRegion(fd, fileSize, begin, end, states[t]);
    });
    ::close(fd);

    ImportResult res;
    for (auto& st : states) {
        st.out.close();
        if (!st.error.empty()) die(st.error);
        res.edges += st.edges;
        res.maxId = std::max(res.maxId, st.maxId);
        res.parts.push_back(st.out.path());
    }
    if (res.edges == 0) die("во входном файле не найдено ни одного ребра");

    logMsg("фаза A: рёбер = %llu, maxId = %lld",
           (unsigned long long)res.edges, (long long)res.maxId);
    return res;
}

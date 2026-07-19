#!/usr/bin/env bash
# Сквозные тесты graphrank. Запуск из корня репозитория:
#   ./tests/run_tests.sh [путь_к_бинарнику]
set -euo pipefail

BIN=${1:-build/graphrank}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "=== Тест 1: пример из условия задачи против эталонной реализации ==="
"$BIN" --input "$ROOT/tests/sample/edges.csv" --output "$TMP/got.csv" \
       --workdir "$TMP/w1" --memory-mb 64 --threads 2 2>/dev/null
python3 "$ROOT/tools/reference_leaderrank.py" "$ROOT/tests/sample/edges.csv" \
       > "$TMP/expected.csv"
python3 "$ROOT/tools/compare_csv.py" "$TMP/got.csv" "$TMP/expected.csv" 1e-8

echo "=== Тест 2: случайный граф против эталонной реализации ==="
python3 "$ROOT/tools/gen_graph.py" --vertices 3000 --edges 30000 \
       --seed 7 --out "$TMP/rnd.csv" 2>/dev/null
"$BIN" --input "$TMP/rnd.csv" --output "$TMP/rnd_got.csv" \
       --workdir "$TMP/w2" --memory-mb 64 --threads 4 --tol 1e-12 2>/dev/null
python3 "$ROOT/tools/reference_leaderrank.py" "$TMP/rnd.csv" > "$TMP/rnd_exp.csv"
python3 "$ROOT/tools/compare_csv.py" "$TMP/rnd_got.csv" "$TMP/rnd_exp.csv" 1e-8

echo "=== Тест 3: инвариантность к разбиению (P=1 против P=6) ==="
# --slice-size принудительно дробит вершинные массивы на мелкие слайсы,
# заставляя пройти весь многослайсовый путь даже на маленьком графе.
"$BIN" --input "$TMP/rnd.csv" --output "$TMP/rnd_sliced.csv" \
       --workdir "$TMP/w3" --memory-mb 64 --threads 4 --tol 1e-12 \
       --slice-size 512 2>/dev/null
python3 "$ROOT/tools/compare_csv.py" "$TMP/rnd_got.csv" "$TMP/rnd_sliced.csv" 1e-10

echo "=== Тест 4: инвариантность к числу потоков (1 против 4) ==="
"$BIN" --input "$TMP/rnd.csv" --output "$TMP/rnd_t1.csv" \
       --workdir "$TMP/w4" --memory-mb 64 --threads 1 --tol 1e-12 2>/dev/null
python3 "$ROOT/tools/compare_csv.py" "$TMP/rnd_got.csv" "$TMP/rnd_t1.csv" 1e-10

echo
echo "Все тесты пройдены."

#!/usr/bin/env python3
"""Сравнение двух CSV вида "vertex,rank" с числовым допуском.

Использование: compare_csv.py a.csv b.csv [tol]
Код возврата 0 — файлы совпадают в пределах допуска.
"""
import sys


def load(path):
    result = {}
    with open(path) as f:
        header = f.readline()
        assert header.strip().lower().startswith("vertex"), f"нет заголовка в {path}"
        for line in f:
            line = line.strip()
            if not line:
                continue
            v, r = line.split(",")
            result[int(v)] = float(r)
    return result


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    tol = float(sys.argv[3]) if len(sys.argv) > 3 else 1e-8

    if a.keys() != b.keys():
        only_a = sorted(a.keys() - b.keys())[:5]
        only_b = sorted(b.keys() - a.keys())[:5]
        raise SystemExit(f"разные множества вершин: только в первом {only_a}, "
                         f"только во втором {only_b}")

    worst_v, worst = None, 0.0
    for v in a:
        d = abs(a[v] - b[v])
        if d > worst:
            worst_v, worst = v, d
    if worst > tol:
        raise SystemExit(f"расхождение {worst:.3e} на вершине {worst_v} "
                         f"(допуск {tol:.1e})")
    print(f"OK: {len(a)} вершин, максимальное расхождение {worst:.3e} "
          f"(допуск {tol:.1e})")


if __name__ == "__main__":
    main()

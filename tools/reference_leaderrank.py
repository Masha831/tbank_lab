#!/usr/bin/env python3
"""Эталонная реализация LeaderRank «в лоб» (весь граф в памяти).

Используется ТОЛЬКО для проверки корректности graphrank на маленьких графах:
независимая реализация на другом языке с другой структурой данных.

Использование: reference_leaderrank.py edges.csv > expected.csv
"""
import sys
from collections import defaultdict


def read_edges(path):
    edges = []
    header_allowed = True
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            for sep in (",", ";", "\t", " "):
                line = line.replace(sep, " ")
            parts = line.split()
            try:
                a, b = int(parts[0]), int(parts[1])
            except (ValueError, IndexError):
                if header_allowed:
                    header_allowed = False
                    continue
                raise SystemExit(f"не удалось разобрать строку: {line!r}")
            header_allowed = False
            edges.append((a, b))
    return edges


def leaderrank(edges, tol=1e-13, max_iters=100000):
    vertices = sorted({v for e in edges for v in e})
    n = len(vertices)
    outdeg = defaultdict(int)
    for a, _ in edges:
        outdeg[a] += 1

    # Инициализация: суммарная масса 1 (включая ground) — как в graphrank.
    s = {v: 1.0 / (n + 1) for v in vertices}
    sg = 1.0 / (n + 1)

    for _ in range(max_iters):
        contrib = {v: s[v] / (outdeg[v] + 1) for v in vertices}
        g_next = sum(contrib.values())
        nxt = {v: sg / n for v in vertices}  # вклад ground в каждую вершину
        for a, b in edges:
            nxt[b] += contrib[a]
        delta = sum(abs(nxt[v] - s[v]) for v in vertices) + abs(g_next - sg)
        s, sg = nxt, g_next
        if delta < tol:
            break

    share = sg / n
    return {v: s[v] + share for v in vertices}


def main():
    if len(sys.argv) != 2:
        raise SystemExit("использование: reference_leaderrank.py edges.csv")
    scores = leaderrank(read_edges(sys.argv[1]))
    print("vertex,rank")
    for v in sorted(scores):
        print(f"{v},{scores[v]:.15g}")


if __name__ == "__main__":
    main()

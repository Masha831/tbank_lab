#!/usr/bin/env python3
"""Генератор синтетического ориентированного графа со степенным перекосом.

Источник ребра выбирается по закону src = floor(V * u^skew), u ~ U(0,1]:
при skew > 1 маленькие id получают на порядки больше исходящих рёбер —
это моделирует гипер-узлы из условия задачи. Приёмник равномерный.

Генерация детерминированная (фиксированный seed) — важно для
воспроизводимости тестов. Если установлен numpy, используется быстрый
векторизованный путь; иначе — чистый stdlib.

Использование:
  gen_graph.py --vertices 2000000 --edges 20000000 --out big.csv \
               [--skew 3.0] [--seed 42]
"""
import argparse
import random
import sys


def gen_stdlib(v, e, skew, seed, out):
    rnd = random.Random(seed)
    buf = []
    for _ in range(e):
        src = int(v * (rnd.random() ** skew))
        dst = rnd.randrange(v)
        buf.append(f"{src},{dst}\n")
        if len(buf) >= 100000:
            out.writelines(buf)
            buf.clear()
    out.writelines(buf)


def gen_numpy(v, e, skew, seed, out, np):
    rng = np.random.default_rng(seed)
    chunk = 5_000_000
    done = 0
    while done < e:
        n = min(chunk, e - done)
        src = (v * rng.random(n) ** skew).astype(np.int64)
        dst = rng.integers(0, v, n)
        lines = np.char.add(np.char.add(src.astype(str), ","),
                            dst.astype(str))
        out.write("\n".join(lines))
        out.write("\n")
        done += n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vertices", type=int, required=True)
    ap.add_argument("--edges", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--skew", type=float, default=3.0)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    with open(args.out, "w") as out:
        out.write("from,to\n")
        try:
            import numpy as np
            gen_numpy(args.vertices, args.edges, args.skew, args.seed, out, np)
        except ImportError:
            print("numpy не найден, генерирую средствами stdlib (медленнее)",
                  file=sys.stderr)
            gen_stdlib(args.vertices, args.edges, args.skew, args.seed, out)
    print(f"записан {args.out}: V<={args.vertices}, E={args.edges}, "
          f"skew={args.skew}, seed={args.seed}", file=sys.stderr)


if __name__ == "__main__":
    main()

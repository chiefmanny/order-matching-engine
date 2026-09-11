"""OHLCV candles built from real executions.

Never from midprice sampling: the point of the harness is that price comes out of
actual matches. A midprice-derived candle can move while nothing traded, which would
make the candle series disagree with the trade tape it is supposed to summarise.
"""
from __future__ import annotations

import pandas as pd


def build_candles(trades: pd.DataFrame, interval_ms: int) -> pd.DataFrame:
    if trades is None or trades.empty:
        return pd.DataFrame()

    df = trades.copy()
    base = int(df["ts_ns"].iloc[0])
    bucket_ns = interval_ms * 1_000_000
    df["bucket"] = (df["ts_ns"] - base) // bucket_ns

    grouped = df.groupby("bucket").agg(
        open=("price", "first"),
        high=("price", "max"),
        low=("price", "min"),
        close=("price", "last"),
        volume=("size", "sum"),
        trades=("price", "count"),
    ).reset_index()

    grouped["t_seconds"] = grouped["bucket"] * interval_ms / 1000.0
    return grouped


def write_candles(run_path, trades: pd.DataFrame, intervals=(250, 1000, 5000)) -> list:
    from pathlib import Path
    written = []
    for ms in intervals:
        label = f"{ms}ms" if ms < 1000 else f"{ms // 1000}s"
        c = build_candles(trades, ms)
        path = Path(run_path) / f"candles_{label}.csv"
        c.to_csv(path, index=False)
        written.append(path)
    return written

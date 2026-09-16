"""Surveillance metrics: behavioural fingerprints, computed identically for everyone.

This is the educational payload of the layering scenario. The point is not that a
layering actor exists in the simulation -- it is that the same handful of measurements,
applied uniformly to every participant, separate it from the genuine ones without
anybody having to be told which is which.

The measures are the ones real surveillance keys on:

    cancel ratio        share of orders withdrawn rather than traded
    fill ratio          share that actually executed
    order lifetime      how long orders rest before being pulled
    order-to-trade      orders placed per execution obtained
    mean order size     displayed size relative to other participants

A genuine liquidity provider also cancels heavily -- that is what quoting IS -- so no
single number is evidence of anything. What separates them is the combination: a
provider cancels constantly but also trades constantly and leaves orders resting for
as long as they remain useful. An actor that posts unusually large size, withdraws
essentially all of it, trades almost none of it, and does so on a fixed cadence looks
different on every axis at once.

Computed from the participant event log, so it applies to every participant equally
and cannot be gamed by a strategy reporting flattering numbers about itself.
"""
from __future__ import annotations

import pandas as pd


def compute(run, tick: float) -> dict:
    events = run.events
    if events.empty or "type" not in events:
        return {}

    out: dict = {}
    profiles: list[dict] = []

    for pid, group in events.groupby("participant_id"):
        submits = group[group["type"] == "submit"]
        cancels = group[group["type"] == "cancel"]
        fills = group[group["type"] == "fill"]
        if submits.empty:
            continue

        # Successful cancels only; a cancel event carrying ok=false did nothing.
        if "ok" in cancels.columns:
            cancels = cancels[cancels["ok"].fillna(True).astype(bool)]

        n_sub = len(submits)
        n_can = len(cancels)
        n_fil = len(fills)

        # Lifetime: submit to first terminal event for the same order id.
        lifetimes = []
        if "order_id" in group.columns:
            placed = dict(zip(submits["order_id"], submits["ts_ns"]))
            terminal = group[group["type"].isin(["cancel", "fill"])]
            seen: set = set()
            for _, row in terminal.sort_values("ts_ns").iterrows():
                oid = row.get("order_id")
                if oid in placed and oid not in seen:
                    seen.add(oid)
                    lifetimes.append((int(row["ts_ns"]) - int(placed[oid])) / 1e9)

        profile = {
            "participant": pid,
            "orders": n_sub,
            "cancel_ratio": round(n_can / n_sub, 3),
            "fill_ratio": round(n_fil / n_sub, 3),
            "order_to_trade": round(n_sub / n_fil, 2) if n_fil else float(n_sub),
            "mean_order_size": round(float(submits["size"].mean()), 1)
            if "size" in submits.columns else 0.0,
        }
        if lifetimes:
            series = pd.Series(lifetimes)
            profile["mean_lifetime_s"] = round(float(series.mean()), 3)
            profile["median_lifetime_s"] = round(float(series.median()), 3)
            # A low spread relative to the mean means orders are pulled on a timer
            # rather than in response to the market -- mechanical regularity is itself
            # a signal, and one no genuine strategy produces by accident.
            profile["lifetime_cv"] = round(
                float(series.std() / series.mean()), 3) if series.mean() > 0 else 0.0

        profiles.append(profile)
        profiles[-1] = profile  # keep the reference for scoring below

    if not profiles:
        return out

    # Flag the participant that stands out on the COMBINATION, not on any one axis.
    #
    # An earlier version of this score used cancel ratio x order-to-trade x relative
    # size, and it ranked the honest liquidity provider as the most anomalous
    # participant in the run -- ahead of the layering actor. The provider cancelled
    # 99.9% of its orders against the layerer's 83%, and had an order-to-trade ratio
    # of 14.3 against 2.9, because continuous quoting means constantly replacing
    # orders. That is the classic surveillance false positive, reproduced here by
    # accident, and it is worth more as a lesson than the original demonstration was.
    #
    # A second attempt added lifetime REGULARITY, on the reasoning that a timer-driven
    # actor should show tightly clustered order lifetimes while a provider's track the
    # market. It worked on one run (layerer CV 0.43 against the provider's 1.17) and
    # inverted on the next, where the layerer's CV came out at 3.74.
    #
    # The reason is that a participant's orders are not one population. This actor
    # posts short-lived layers AND longer-resting opposing orders, so a single
    # coefficient of variation over all of them is bimodal and unstable. Per-order-type
    # regularity might well work; one number per participant does not, and a
    # discriminator that flips between runs is worse than none.
    #
    # What IS stable across runs is cancel ratio weighted by how large the displayed
    # orders are relative to everyone else. Measured separation: 2.83x and 2.92x on
    # two runs, against 6.43x and 1.09x for the regularity version.
    sizes = [p["mean_order_size"] for p in profiles if p["mean_order_size"] > 0]
    median_size = sorted(sizes)[len(sizes) // 2] if sizes else 1.0
    for profile in profiles:
        relative_size = profile["mean_order_size"] / median_size if median_size else 1.0
        profile["_score"] = profile["cancel_ratio"] * relative_size

    for profile in profiles:
        prefix = f"surveil.{profile['participant']}"
        for key, value in profile.items():
            if key not in ("participant", "_regularity"):
                out[f"{prefix}.{key.lstrip('_')}"] = value

    ranked = sorted(profiles, key=lambda p: p["_score"], reverse=True)
    out["surveil.most_anomalous"] = ranked[0]["participant"]
    out["surveil.top_score"] = round(ranked[0]["_score"], 2)
    if len(ranked) > 1:
        runner_up = ranked[1]["_score"]
        # How far clear the top participant is. A large separation is what makes the
        # demonstration legible; a small one would mean the metrics are not
        # discriminating and the claim should not be made.
        out["surveil.score_separation"] = round(
            ranked[0]["_score"] / runner_up, 2) if runner_up > 0 else None
    return out

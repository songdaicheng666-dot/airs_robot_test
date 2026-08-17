from __future__ import annotations

import math
from datetime import datetime, timezone
from typing import Any, Iterable


def utc_now() -> str:
    return datetime.now(tz=timezone.utc).isoformat().replace("+00:00", "Z")


def parse_utc(value: str | None) -> datetime | None:
    if not value:
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo is not None else parsed.replace(tzinfo=timezone.utc)


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return round(ordered[lower], 3)
    result = ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)
    return round(result, 3)


def summarize_rtt(samples: Iterable[dict[str, Any]]) -> dict[str, Any]:
    materialized = list(samples)
    values = [float(sample["rtt_ms"]) for sample in materialized if sample.get("success")]
    return {
        "sample_count": len(materialized),
        "success_count": len(values),
        "failure_count": len(materialized) - len(values),
        "min": round(min(values), 3) if values else None,
        "avg": round(sum(values) / len(values), 3) if values else None,
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "max": round(max(values), 3) if values else None,
    }


def corrected_latency_ms(
    start: str | None,
    end: str | None,
    *,
    start_offset_ms: float = 0.0,
    end_offset_ms: float = 0.0,
) -> float | None:
    start_time = parse_utc(start)
    end_time = parse_utc(end)
    if start_time is None or end_time is None:
        return None
    raw = (end_time - start_time).total_seconds() * 1000
    return round(raw + end_offset_ms - start_offset_ms, 3)


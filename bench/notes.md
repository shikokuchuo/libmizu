# Recorded benchmark notes

Dated performance records for libmizu's C bench suite (`make bench` —
`bench_channel`, `bench_pool` and `bench_arrow`). Report-only: nothing
here is asserted against — runner timing is too variable for thresholds.

Two parts: the best-known table below is the headline — swap a row's
value in only when a run beats it, with the date of that measurement
(lower is better on the latency rows, higher on the rate rows) — and the
dated run log at the bottom is the source of truth, carrying the bands
and each change's context. Append new dated outcomes at the bottom of
the log; update the table only on a new best.

Log entries follow one format (shared with the mizu and pymizu bench
notes):

```
## YYYY-MM-DD — <what changed or what was run> [(<commit>[, ...])]

<1-4 sentences: the change or the run's purpose, plus any caveat a
reader needs to interpret the numbers — host drift, a re-measure, a
definition change.>

Results: row v; row v; ...   (a short list, or a small table when the
                              entry is an A/B)
Status: <suite/lint state when recorded, e.g. "unit + integration +
        fuzz green"> — omit when nothing was run
```

The suite measures the transport floor: in-process peers staging through
the built-in bytes binding, so no language-runtime boundary is in the
numbers. The R package's records sit ~1 us higher on the same host —
that gap is the R-interpreter boundary, which is why a regression
showing in the R gate but not here bisects to the binding layer.

Latency figures are per-iteration medians (the mean follows the median
closely at these sizes). `pool_steal_w*` uses tiny echo tasks: the
single submitter thread is the bottleneck, so the number is the
transport's task-throughput ceiling at that worker count, not a
compute-scaling curve.

## Best-known results

| row | best | measured |
|---|---|---|
| `channel_roundtrip_NIL` | 0.37 us (n=50k) | 2026-08-22 |
| `channel_roundtrip_64B` | 0.33 us (n=50k) | 2026-08-22 |
| `channel_roundtrip_64KiB` | 4.04 us (n=10k; ARENA tier) | 2026-10-01 |
| `channel_roundtrip_1MiB` | 50.4 us (n=3k; SHM_RAW spill tier) | 2026-10-01 |
| `channel_batch_64B` | 52.8 Mmsg/s (1M msgs, batches of 32) | 2026-10-01 |
| `parker_wake` | 16.7 us (n=2k) | 2026-08-22 |
| `spill_fresh_1MiB` | 10.70 us per create+close (n=300) | 2026-08-23 |
| `spill_recycle_1MiB` | 0.03 us per free-list pop+insert (n=100k) | 2026-08-22 |
| `pool_submit_collect_NIL` | 0.37 us (n=20k) | 2026-08-22 |
| `pool_submit_collect_4KiB` | 1.04 us (n=10k; SHM_RAW both ways) | 2026-08-22 |
| `pool_steal_w1` | 10.3M tasks/s | 2026-10-01 |
| `pool_steal_w2` | 11.4M tasks/s | 2026-08-22 |
| `pool_steal_w4` | 7.7M tasks/s | 2026-10-01 |

## Run log

Dated outcome records, oldest first — the source of truth for the table
above. Append new dated outcomes at the bottom, with the commit SHA.

## 2026-08-22 — first records; the bench tier lands (9cbf72c + step-3 working tree)

Apple M4 Pro, macOS 26.6.1, arm64. R-package reference points on the
same host (2026-08-19/20): channel rt 1.50 us, pool 1.0 us — the C
floor is ~0.4 us for both, so the seam carve cost the hot path nothing
measurable through the R boundary either.

Results:

| row | value |
|---|---|
| `channel_roundtrip_NIL` | 0.37 us (n=50k) |
| `channel_roundtrip_64B` | 0.33 us (n=50k) |
| `channel_roundtrip_64KiB` | 4.83 us (n=10k; ARENA tier) |
| `channel_roundtrip_1MiB` | 56.46 us (n=3k; SHM_RAW spill tier) |
| `channel_batch_64B` | 47.1 Mmsg/s (1M msgs, batches of 32) |
| `parker_wake` | 16.7 us (unpark -> park-return ack, n=2k) |
| `spill_fresh_1MiB` | 10.95 us per create+close (n=300) |
| `spill_recycle_1MiB` | 0.03 us per free-list pop+insert (n=100k) |
| `pool_submit_collect_NIL` | 0.37 us (n=20k) |
| `pool_submit_collect_4KiB` | 1.04 us (n=10k; SHM_RAW both ways) |
| `pool_steal_w1` / `w2` / `w4` | 9.9M / 11.4M / 6.2M tasks/s (200k tiny echo tasks; submitter-bound — see the header note) |

## 2026-08-23 — no-regression re-run (753e619)

Same host. Covers the `cb845d5`..`753e619` changes (the attach message
alignment, `rei_pool_task_release`, the exit-time registry-log
teardown). All rows within the 2026-08-22 first-record bands.

Results:

| row | value |
|---|---|
| `channel_roundtrip_NIL` / `64B` | 0.42 / 0.42 us |
| `channel_roundtrip_64KiB` / `1MiB` | 4.33 / 50.46 us |
| `channel_batch_64B` | 43.6 Mmsg/s |
| `parker_wake` | 16.96 us |
| `spill_fresh_1MiB` / `spill_recycle_1MiB` | 10.70 / 0.03 us |
| `pool_submit_collect_NIL` / `4KiB` | 0.38 / 1.04 us |
| `pool_steal_w1` / `w2` / `w4` | 10.1M / 11.4M / 5.7M tasks/s |

Status: full re-run green — unit, integration, soak (10 s), fuzz (100k
runs x 3 harnesses, fuzzer+UBSan — the local Homebrew-ASan startup-hang
caveat stands), ASan+UBSan and TSan over unit + soak + bench, the
amalgamation smoke, rei.h under C++17. The shared lib exports 76
`rei_*` symbols (75 + `rei_pool_task_release`).

## 2026-10-01 — no-regression re-run; Phase 5 acceptance (c6f8cd2)

Same host, macOS 27. Phase 5 (the cross-language map) touched no core
code — DESIGN.md/CHANGELOG only — so this run also covers the
`753e619`..`c6f8cd2` record gap (Phases 0-4: the identity words, the
interop cursor, the morsel module, rng_jump, the validity sections).
bench_pool.c's `bench_exec` regained the current `mizu_exec_fn`
signature (`mizu_read_ctx *ctx`) — the bench tier had gone stale
against the exec-hook signature.

Results:

| row | value |
|---|---|
| `channel_roundtrip_NIL` / `64B` | 0.38-0.42 / 0.37-0.38 us |
| `channel_roundtrip_64KiB` / `1MiB` | 4.04-4.21 / 50.4-50.6 us |
| `channel_batch_64B` | 49.6-52.8 Mmsg/s |
| `parker_wake` | 16.8-16.9 us |
| `spill_fresh_1MiB` | 12.3-13.8 us — up from the 10.70 record; the create path carries no post-August code change (the THP collapse runs at free-list insert, recycle flat at 0.03); tracks the host OS move (26.6.1 -> 27), not the core |
| `spill_recycle_1MiB` | 0.03 us |
| `pool_submit_collect_NIL` / `4KiB` | 0.42 / 1.04 us |
| `pool_steal_w1` / `w2` / `w4` | 10.3M / 11.4M / 7.7M tasks/s |

Status: all other rows inside the 2026-08-22/23 bands — no core
regression.

## 2026-11-10 — Arrow kernel baseline (5d8734c)

Apple M4 Pro, macOS 27.2, arm64. New report-only `bench_arrow`: 8M rows,
one batch, one warmup and the best of seven; open, size and body are timed
separately. These are the pre-optimization records for the sparse-null,
dictionary, popcount, temporal and open-overhead work.

Results (ms):

| column | open, null-free / 1% | size, unknown | body, null-free | body, unknown | body, 1% | body, 50% | body, all null |
|---|---:|---:|---:|---:|---:|---:|---:|
| float64 copy | 0.000 / 0.000 | 2.385 | 1.176 | 1.272 | 13.862 | 12.540 | 2.027 |
| int64 copy | 0.000 / 0.000 | 1.913 | 1.068 | 1.282 | 18.689 | 20.022 | 2.020 |
| float32 widen | 0.000 / 0.000 | 2.051 | 1.404 | 1.560 | 9.717 | 8.863 | 2.015 |
| date32 | 0.000 / 0.000 | 1.920 | 2.716 | 4.812 | 5.569 | 4.546 | 4.159 |
| timestamp[us] | 0.000 / 0.000 | 1.941 | 3.454 | 5.494 | 6.294 | 4.952 | 4.058 |
| dictionary, int32 index | 5.694 / 8.331 | 2.018 | 6.038 | 7.644 | 8.299 | 5.952 | 4.133 |

## 2026-11-10 — Arrow kernel speedups (5d8734c + working tree)

Same host and shapes. Unknown-count resolution moved from the size pass to
open: it now costs 0.128-0.187 ms there (dictionary 1.336 ms including index
validation), while every unknown size row is below the clock precision. The
1%-null numeric body rows are within about 2x of their null-free rows.

Results (ms):

| column | open, null-free / 1% | body, null-free | body, unknown | body, 1% | body, 50% | body, all null |
|---|---:|---:|---:|---:|---:|---:|
| float64 copy | 0.000 / 0.000 | 1.158 | 1.071 | 1.643 | 7.174 | 2.016 |
| int64 copy | 0.000 / 0.000 | 1.065 | 1.064 | 1.837 | 8.026 | 2.018 |
| float32 widen | 0.000 / 0.000 | 1.342 | 1.363 | 1.674 | 7.473 | 2.020 |
| date32 | 0.000 / 0.000 | 1.351 | 1.444 | 1.708 | 4.238 | 2.015 |
| timestamp[us] | 0.000 / 0.000 | 1.960 | 1.871 | 2.478 | 5.209 | 2.036 |
| dictionary, int32 index | 1.194 / 4.716 | 1.093 | 1.107 | 1.635 | 7.507 | 2.016 |

Status: unit + ext + integration + fuzz (100k runs per harness, fuzzer+UBSan)
green; interop corpus unchanged; export surface unchanged.

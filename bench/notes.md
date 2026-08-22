# Recorded benchmark notes

Dated performance records for librei's C bench suite (`make bench` —
`bench_channel` and `bench_pool`). Report-only: nothing here is asserted
against — runner timing is too variable for thresholds. Append new dated
outcomes at the bottom with the commit SHA.

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

## 2026-08-22: first records (step 3: tests and benchmarks)

Apple M4 Pro, macOS 26.6.1, arm64. Code: `9cbf72c` + the step-3 working
tree (the bench harness itself is in this tree; pre-commit).

- `channel_roundtrip_NIL`: 0.37 us (n=50k)
- `channel_roundtrip_64B`: 0.33 us (n=50k)
- `channel_roundtrip_64KiB`: 4.83 us (n=10k; ARENA tier)
- `channel_roundtrip_1MiB`: 56.46 us (n=3k; SHM_RAW spill tier)
- `channel_batch_64B`: 47.1 Mmsg/s (1M msgs, batches of 32)
- `parker_wake`: 16.7 us (unpark -> park-return ack, n=2k)
- `spill_fresh_1MiB`: 10.95 us per create+close (n=300)
- `spill_recycle_1MiB`: 0.03 us per free-list pop+insert (n=100k)
- `pool_submit_collect_NIL`: 0.37 us (n=20k)
- `pool_submit_collect_4KiB`: 1.04 us (n=10k; SHM_RAW both ways)
- `pool_steal_w1`: 9.9M tasks/s; `w2`: 11.4M; `w4`: 6.2M (200k tiny
  echo tasks; submitter-bound — see the header note)

Reference points from the R package on the same host (2026-08-19/20
records): channel round trip 1.50 us, pool 1.0 us/task — the C floor is
~0.4 us for both, so the seam carve cost the hot path nothing measurable
through the R boundary either.

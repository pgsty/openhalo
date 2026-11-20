# OpenHalo 19.0.0 hotspot deadlocks and long waits

Date: 2026-11-20. Baseline: `3af5b6bd00ee5b2c50a4a679672d514ef4b1ba54`.
Product version **19.0.0**, PostgreSQL **19.0**, aux_mysql **2.0**.
This continues [the initial concurrency investigation](CONCURRENCY-19.0.0.md).

## Findings and fixes

### Detection delay amplified by a queue of hot-row writers

The baseline 16-thread, 100-row sysbench workload made almost every sampled
backend wait: **1,399 tuple-lock waits, 200 transaction-ID waits and one active
sample** out of 1,600 observations. Tuple-lock queue heads were themselves
waiting for transactions that held other hot rows. Recorded cycles contained
both lock kinds. Many waits concentrated on IDs 50 and 51: sysbench's default
`special` distribution concentrates 75% of draws in a 1% special range, so this
is much more contentious than uniform access to 100 rows.

PostgreSQL normally postpones the full deadlock check until
`deadlock_timeout`, which defaults to one second. Resolving one cycle lets
other queued writers proceed and form or encounter another cycle, accumulating
successive detection delays. The baseline requested 20 seconds of work but
took 39.23 seconds for read/write and 52.22 seconds for write-only to finish.
This delay policy is documented in
[PostgreSQL lock management](https://www.postgresql.org/docs/19/runtime-config-locks.html).

The optional protocol callback `early_deadlock_check` now requests **one full
check immediately after a waiter joins the lock queue**, before sleeping.
aux_mysql enables it for tuple and transaction-ID lock waits. The code calls
the existing PostgreSQL detector after releasing the lock partition lock and
arming error cleanup. It retains the detector's queue reordering for soft
cycles and normal error handling for hard cycles. A true deadlock reports
1213/40001 and rolls back the whole victim transaction, using the recovery
behavior fixed in the preceding change.

If no cycle exists, the backend waits normally. The ordinary deadlock timer
still starts, so a later waiter using another protocol can close a cycle and
the timed detector will find it. The early check does not cancel autovacuum,
run standby conflict processing, trigger the deadlock-timeout injection point,
or lower the threshold for lock-wait logging. Native PostgreSQL sessions, DDL
relation waits and advisory waits retain their existing timed behavior.

`mysql.early_deadlock_check` defaults to `on`. Administrators can disable it
with PostgreSQL SET or configuration reload to restore timed detection for
MySQL too. The existing `deadlock_timeout` value is unchanged. Checking a graph
on each contended wait costs more lock-manager work; the switch permits
workload-specific comparison. The detector still chooses its own transaction
as the victim of a hard cycle; this change does not add InnoDB's victim policy.

### Bitmap plans bypassed the current-row lookup

Longer runs exposed another source of stalls. The preceding point-write fix
handled direct SeqScan and IndexScan plans. Sustained updates can change the
cost-based choice to BitmapHeapScan, which bypassed that fix. A waiting DELETE
could again skip a deleted/reinserted row, causing the following INSERT to
fail with 1062.

This was confirmed in two ways:

- Forcing a bitmap plan reproduced DELETE returning **0** after another
  transaction deleted and reinserted the key; the expected result was **1**.
- Sampling EXPLAIN during a live run on the older module captured a natural
  change from `MySQL Current Read` to `Bitmap Heap Scan` roughly 29 seconds
  into the workload. That run subsequently logged duplicate-key failures.

The planner hook now also accepts a direct BitmapHeapScan when it proves
equality on the complete eligible unique key. It preserves the original
bitmap recheck predicates, replaces the bitmap producer with the unique-key
lookup, and rechecks all predicates against the locked current row. The
existing restrictions on RLS, joins, table/index types, range and gap locking
remain. Three deterministic tests force bitmap plans for DELETE/reinsert and
both outcomes of a changed UPDATE predicate.

### A fatal sysbench worker retained transaction locks

After 1062, a sysbench worker could stop issuing SQL while retaining its
connection and earlier transaction locks. Other workers then waited behind an
idle transaction. This is not a deadlock cycle, and an eager detector cannot
resolve it. The original stress process needed the external watchdog to stop
it, which released the connections and their locks.

The new `hotspot_stress.py` runner watches for fatal worker errors, records
them, terminates that failed sysbench process and enforces a bounded drain
time. It retries **only 1213**: duplicates, lock timeouts and any other SQL
error fail the run. A negative control against the old module, with bitmap
plans forced, preserved the 1062 failure, stopped the worker in 0.32 seconds
and removed its temporary schema. This is test-harness cleanup, not a server
policy of aborting legitimate idle transactions. The existing `sysbench.sh`
retains its stricter zero-retry gate.

## Validation

All final builds include the protocol callback and bitmap fix. The new callback
extends `ProtocolExtensionConfig`: **rebuild the kernel and aux_mysql together**.

| Build / test | Final result |
|---|---|
| Ubuntu 24.04 arm64, GCC 13, debug/assertions | **17/17 integration suites** |
| Debian 12 amd64, GCC 12, release, emulated | **17/17 integration suites** |
| macOS arm64, debug/assertions | **17/17 integration suites** |
| Ubuntu arm64, ASan + UBSan kernel and extension | **17/17 integration suites** |
| PGXS standalone build, staged install and separate fixture | **27/27 point-write cases + 13/13 hotspot groups** |
| Native PostgreSQL regression, changed kernel | **243/243** |
| Native isolation tests, changed kernel | **139/139** |

Each integration build includes the final **27 point-write cases** and **13
hotspot groups**. The latter cover 2/3/16-session hard cycles, disabling eager
checks, timed fallback, native and mixed protocols, ordinary noncyclic waits,
cancellation, lock timeout and soft cycles resolved by queue reordering.
The eager controls use a 5-second deadlock timer and 2-second lock timeout;
the closing waiter must instead report a deadlock promptly. A soft-cycle test
proves that all three transactions commit after reordering, without a false
deadlock error.

Each hotspot suite also runs two **16-thread, 1,600-commit transfer tests**:
all MySQL, then eight MySQL and eight native PostgreSQL workers. All 16 workers
must finish. Every balance must equal the net effect in the commit ledger,
and the total balance must remain zero. MySQL retries omit explicit ROLLBACK
to verify whole-transaction recovery. Mixed transfer tests use a 50 ms
deadlock timeout for native participants; they do not claim immediate
detection when a native session closes a cycle under the default 1 s timer.

ASan uses `detect_leaks=0:abort_on_error=1`; UBSan uses
`halt_on_error=1:print_stacktrace=1`. No final server assertion, crash, ASan or
UBSan diagnostic was found. This does not claim leak freedom. An initial
client invocation omitted the leak setting and failed on pg_dump/psql exit
allocations; the corrected client run and complete final suite passed. Initial
failure logs remain outside Git.

### Repeated 16-thread hotspot runs

One 100-row table, the default `special` distribution, server prepared
statements, three seeds, 30 seconds per workload. All six workloads completed,
with **115,193 committed transactions**, **70,834 deadlock retries**, **zero
duplicate-key errors** and **zero lock timeouts**.

| Seed | Workload | Commits | Deadlock retries | Mean ms | P99 ms | Maximum ms | Actual seconds |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 | Read/write | 16,395 | 9,435 | 29.29 | 118.92 | 261.03 | 30.0243 |
| 1 | Write only | 17,010 | 11,149 | 28.24 | 142.39 | 408.22 | 30.0556 |
| 2 | Read/write | 12,511 | 7,139 | 38.38 | 167.44 | 589.00 | 30.0242 |
| 2 | Write only | 21,790 | 13,731 | 22.03 | 99.33 | 357.24 | 30.0228 |
| 3 | Read/write | 21,360 | 12,190 | 22.48 | 89.16 | 209.16 | 30.0212 |
| 3 | Write only | 26,127 | 17,190 | 18.38 | 81.48 | 207.00 | 30.0243 |

The earlier baseline's mean transaction latencies were **16,408 ms** and
**31,927 ms**. The final repeated runs measured **18–38 ms**, with **81–167 ms
P99**, and only **21–56 ms** to finish beyond the requested work period. Shared
host load varied, so these are observed contention and latency results, not a
controlled performance benchmark or a cross-engine throughput claim.

Every workload checked `(COUNT(*), COUNT(DISTINCT id), MIN(id), MAX(id), SUM(id))`
and obtained `(100,100,1,100,5050)`. Deadlocks are expected in this multi-key
workload; the acceptance condition is correct full rollback and prompt retry,
not zero deadlocks. SQL errors other than 1213 fail immediately.

### Additional contention coverage

Each of the following runs completed both read/write and write-only workloads,
with zero duplicates or lock timeouts and successful integrity checks:

| Configuration | Seconds per workload | Combined commits | Combined deadlock retries | Largest P99 ms | Largest maximum ms |
|---|---:|---:|---:|---:|---:|
| 32 threads, 100 rows, special distribution | 30 | 29,874 | 26,691 | 325.98 | 848.83 |
| 64 threads, 100 rows, special distribution | 30 | 9,299 | 11,740 | 2,238.47 | 4,449.27 |
| 16 threads, 100 rows, uniform distribution | 20 | 93,692 | 322 | 28.67 | 101.45 |
| 16 threads, 100 rows, text protocol, special distribution | 20 | 27,077 | 16,645 | 137.35 | 304.72 |
| Debian amd64 release, 16 threads, 100 rows | 20 | 14,593 | 9,141 | 231.53 | 553.53 |
| ASan/UBSan, 16 threads, 100 rows | 20 | 1,222 | 706 | 2,680.11 | 4,273.89 |

The 64-thread run still has substantial contention and tail latency. Its
workloads completed in 30.42 and 30.37 seconds; the result does not imply a
low-latency guarantee at arbitrary concurrency. The uniform distribution's
much smaller retry count also illustrates how concentrated the default
100-row hotspot is. Sanitized/emulated builds are correctness checks, not
performance comparisons.

One earlier 64-thread run overlapped an integration test that deliberately
enabled global read-only mode. Its 1290 failure was recorded and excluded.
After that suite finished and read-only settings were verified restored, the
entire 64-thread pair above ran separately and passed. The failed run remains
in `stress64/`; accepted results are in `stress64-final/`.

The ordinary eight-thread control used four 20,000-row tables and 20 seconds
per workload. Point reads, read-only, read/write and write-only completed
1,077,388 / 50,207 / 33,006 / 109,697 transactions. Only write-only retried a
deadlock, once. Every table preserved `(20000,20000,1,20000,200010000)` after
every workload. This remains a completed run with a retry, rather than a pass
of `sysbench.sh`'s zero-retry gate.

A single-key control serialized 16 writers on one row for 20 seconds. With
eager checks off it completed **16,589 transactions** (19.30 ms mean); with
checks on it completed **15,033** (21.30 ms mean). Both had zero deadlocks and
passed integrity checks. In this short control the extra checks cost about
9% throughput, illustrating the tradeoff when contention has no cycles. It is
not a general overhead bound. The fixture's temporary role setting was
restored; the new default remains enabled for MySQL row/transaction waits.

Across the **24 accepted sysbench workloads**, there were **465,275 committed
write-workload transactions**, zero duplicate-key failures and zero lock
timeouts. Read workloads bring the total to 1,592,870 completed transactions;
136,080 retries were all deadlocks. The expected negative control, the old
bitmap failures and the read-only test interference are excluded.

## Reproduction and evidence

Use disposable fixtures and connection variables described in
[TESTING.md](TESTING.md):

```sh
python3 tests/point_write_concurrency.py
python3 tests/hotspot_deadlocks.py
python3 tests/hotspot_stress.py --output /tmp/openhalo-hotspot-16 \
  --threads 16 --size 100 --seconds 30 --repeat 3
```

Raw logs, exact runners, sampled waits and summaries remain outside Git at
`/tmp/openhalo-19.0.0-review-20261120/hotspot-fix`:

- `wait-samples-before.json` and `server-before.log`: baseline queues/cycles.
  Fresh baseline sysbench logs are in the sibling `sysbench-fix/hotspot-baseline/`.
- `bitmap-live-plans.json`, `bitmap-live-stress/` and `server-pgxs.log`: live
  plan change and old-module failure; `stress16/` and `server-eager.log` preserve
  the first longer run that exposed the bitmap gap.
- `stress-harness-negative/`: expected 1062 failure and prompt cleanup.
- `{u24,d12,asan}-final-integration/status.tsv` and
  `macos-final-integration/integration-status.tsv`: final suite status and logs.
  `pgxs-{point-write,deadlocks}-final.log` records standalone-module tests.
- `core-final.log`, `isolation-final.log`, `server-*-final.log`, build/install
  logs and `stress16-final/summary.json` with per-workload sysbench output.
- `stress32/`, `stress64-final/`, `uniform16/`, `text16/`, `stress-d12-16/`
  and `stress-asan16/` record the additional accepted stress runs.
- `normal8/`, `single-key-off/`, `single-key-on/` and
  `stress-matrix-final-status.tsv` record the controls and remaining runs.

Only source, tests and this report belong in Git. Test databases and raw logs
are retained as disposable task artifacts. Full InnoDB locking equivalence,
bounded latency behind an application holding a noncyclic lock, and production
performance at arbitrary connection counts are outside these results.

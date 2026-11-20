# OpenHalo 19.0.0 concurrent-write investigation

Date: 2026-11-20. Baseline: `4cb54cc0fbdbdcd03a5d84ddcd1c7a7c1496979a`.
Product version remains **19.0.0**, PostgreSQL kernel **19.0**, extension **2.0**.
This follows the failing eight-thread sysbench run recorded in
[REVIEW-19.0.0.md](REVIEW-19.0.0.md). The fixes and tests below concern the
development main branch; no release or deployment is implied.

The later [hotspot investigation](HOTSPOT-19.0.0.md) adds early deadlock detection
and fixes the BitmapHeapScan gap exposed by sustained high-contention writes.
The results below describe the earlier round and are retained as its baseline.

## Root cause: a deleted tuple is not a reinserted key

The failure is reproducible with two transactions and one existing primary key:

| Step | Transaction A | Transaction B |
|---|---|---|
| 1 | `BEGIN; DELETE FROM t WHERE id=1; INSERT INTO t VALUES(1,10);` | |
| 2 | | `BEGIN; DELETE FROM t WHERE id=1;` waits for A |
| 3 | `COMMIT;` | DELETE resumes |
| 4 | | `INSERT INTO t VALUES(1,20);` |

Before the fix, B's DELETE returned **0**, and its INSERT failed with **1062**.
The same schedule on MySQL 5.7.44 returned **1** and inserted successfully.
Native PostgreSQL reproduced the zero-row DELETE and unique violation.

PostgreSQL READ COMMITTED follows the version chain of a concurrent UPDATE,
but skips a tuple deleted by a committed transaction. DELETE followed by INSERT
creates a different tuple, even when the key is identical. The waiting DELETE
does not look up that new tuple. This matches
[PostgreSQL's documented isolation behavior](https://www.postgresql.org/docs/19/transaction-iso.html).
MySQL's UPDATE/DELETE use locking reads, with record locks for a unique-key
lookup; see [InnoDB statement locks](https://dev.mysql.com/doc/refman/8.4/en/innodb-locks-set.html).
The two-session reference test establishes the specific DELETE/reinsert behavior.

Sysbench's write transactions include exactly this DELETE/INSERT sequence.
Concurrent selection of the same key exposed a mismatch in write semantics.
The duplicate error was the consequence of the skipped DELETE.

## Implementation

`src/mys_current_read.c` installs a **MySQL Current Read** CustomScan through
the existing planner hook, only for MySQL sessions. It proves equality on every
key column of a valid, immediate, ordinary B-tree unique index, and replaces a
direct SeqScan/IndexScan under a single-target UPDATE/DELETE on a heap table.

The scan finds and locks the current tuple before evaluating predicates or new
column values. If waiting encounters a deletion or a concurrent UPDATE chain,
it repeats the unique-key lookup using a fresh committed snapshot. All original
predicates are checked against the selected row. Refreshing the snapshot keeps
the original command ID so a statement does not consume its own trigger writes.
Only the lookup repeats: statements and triggers are not replayed. Real heap
tuple locks also cooperate with native PostgreSQL writers.

Composite keys, matching text collations, constants and external prepared
parameters are supported. Runtime checks preserve transaction-snapshot isolation
even when a cached plan is executed after changing the isolation level. Dropping
the chosen index invalidates the cached plan. Actual duplicate inserts still
return 1062.

This path excludes RLS plans, modifying CTEs, joins, range scans, partitions,
partial/expression/deferrable unique indexes, indexes requiring an xmin check,
other table/index access methods and other plan shapes. There are no absent-key
gap locks or general InnoDB next-key locking semantics. Those paths retain their
existing behavior. Native PostgreSQL sessions retain their original execution.

## Two further transaction-recovery defects

1. **A deadlock rolled back only its statement.** The hidden savepoint retained
   writes made earlier in the victim transaction. A deterministic two-key
   deadlock observed the victim's earlier marker row still present. A client's
   subsequent retry BEGIN could implicitly commit that partial transaction.
   Client-visible deadlocks and serialization failures now abort the entire
   transaction and report **1213 / 40001**; PG deadlocks previously leaked
   SQLSTATE `40P01`. Ordinary errors, cancellation and lock timeouts retain
   statement rollback. Caught server-side errors and named advisory-lock
   deadlocks do not trigger this whole-transaction path. This follows
   [InnoDB's error rollback rules](https://dev.mysql.com/doc/refman/8.4/en/innodb-error-handling.html).
2. **Recovery ran after idle status and timers were selected.** Moving to whole
   transaction rollback exposed a stale idle-in-transaction timer: a victim with
   a 500 ms timeout disconnected after rollback. The optional protocol callback
   `prepare_for_query` now runs before PostgreSQL chooses idle state and starts
   timers. Recovery happens there instead of in the later wire-ready callback.
   A regression waits past the timeout and verifies that the connection remains
   usable and earlier writes are gone.

The protocol callback changes `ProtocolExtensionConfig`. **Rebuild the kernel
and aux_mysql together** with these headers. Every final build below includes
both changes.

## Final verification

`tests/point_write_concurrency.py` coordinates transactions by observing real
lock waits. Its 24 OpenHalo cases cover the original race, UPDATE values and
predicate changes, rollback/no replacement/key moves, secondary and composite
text keys, wire prepared statements, native writers, trigger execution and
command visibility, forced SeqScan plans, cached parameter plans and index
invalidation, transaction-snapshot conflicts, cancellation, lock timeouts,
independent keys, genuine duplicates and full deadlock rollback. The deadlock
cases include autocommit on/off, user savepoints and statement rollback disabled.
The portable MySQL 5.7 reference subset has 16 cases.

| Final build / test | Result |
|---|---|
| Ubuntu 24.04 arm64, GCC 13, debug/assertions | **16/16 integration suites**, including **24/24 concurrency cases** |
| Debian 12 amd64, GCC 12, release, emulated | **16/16 suites**, including **24/24 concurrency cases** |
| macOS arm64, debug/assertions | **16/16 suites**, including **24/24 concurrency cases** |
| Ubuntu arm64, ASan + UBSan kernel and extension | **16/16 suites**, including **24/24 concurrency cases**; no sanitizer diagnostics |
| PGXS standalone build and staged install, separate fixture | Build/install succeeded; **24/24 concurrency cases** |
| MySQL 5.7.44 reference | **16/16 portable cases** |
| Final Ubuntu native PostgreSQL regression | **243/243** |
| Final Ubuntu native isolation tests | **139/139** |

The integration suites also cover protocol/prepared statements, DML, binary
types, generated columns, foreign keys and privileges, collations/indexes,
transactions and PG19/release boundaries. Final server logs contain no assertion
trap, PANIC, sanitizer/runtime error, signal crash or stale idle-transaction
timeout. Expected negative tests deliberately generate SQL errors.
ASan ran with `detect_leaks=0:abort_on_error=1`; UBSan used
`halt_on_error=1:print_stacktrace=1`. This is not leak testing. Earlier exploratory
failures remain in the evidence directory and are not final acceptance results.

### Eight-thread sysbench

Four tables of 20,000 rows each, eight threads, 30 seconds per workload,
`--rand-seed=1`, server prepared statements (`--db-ps-mode=auto`). The only
retryable errors were **1213/1205**; 1062 or 1020 would fail the process.

| Workload | OpenHalo completed transactions | OpenHalo retries | MySQL 5.7 transactions | MySQL retries |
|---|---:|---:|---:|---:|
| Point select | 1,426,831 | 0 | 109,287 | 0 |
| Read only | 86,089 | 0 | 5,957 | 0 |
| Read/write | 37,898 | 1 | 5,019 | 0 |
| Write only | 108,654 | 0 | 20,082 | 0 |

The two OpenHalo write workloads completed **146,552 transactions with zero
duplicate-key errors**. The single retry was a real multi-key deadlock. After
each workload, every table passed `(COUNT(*), COUNT(DISTINCT id), MIN(id),
MAX(id), SUM(id)) = (20000,20000,1,20000,200010000)`. No sbtest duplicate-key
violation appears in the final server log.

The checked-in `tests/sysbench.sh` now retries only 1213/1205 and fails
immediately on 1062/1020. Its separate **zero-ignored-errors** exit gate remains
strict: the read/write result above would fail that gate because of its one
deadlock. These are four completed workloads, not four zero-error workloads.
The recorded stress driver runs the same sysbench workloads and adds table
integrity checks. MySQL runs use the same parameters as behavior references;
emulation and shared-host load preclude a cross-engine performance comparison.

### Hot-key contention

With 16 threads sharing one 100-row table, a 20-second work period produced:

| Engine | Workload | Completed transactions | Deadlock retries | Actual elapsed seconds |
|---|---|---:|---:|---:|
| OpenHalo, default `deadlock_timeout=1s` | Read/write | 39 | 96 | 47.18 |
| OpenHalo, default `deadlock_timeout=1s` | Write only | 29 | 59 | 36.19 |
| OpenHalo, diagnostic `deadlock_timeout=10ms` | Read/write | 1,799 | 2,904 | 20.06 |
| OpenHalo, diagnostic `deadlock_timeout=10ms` | Write only | 1,077 | 2,423 | 20.32 |
| MySQL 5.7 | Read/write | 6,011 | 3,329 | 20.04 |
| MySQL 5.7 | Write only | 25,356 | 18,191 | 20.01 |

Both engines had zero duplicate-key failures and preserved
`(100,100,1,100,5050)` after each workload. The work period does not cap the
time needed to finish in-flight transactions and their retries. OpenHalo's
severe contention caused long waits and drain time with the default one-second
deadlock detection delay. Correctness here does not imply acceptable hotspot
latency or InnoDB-equivalent performance.

A focused diagnostic repeated the same workload after temporarily setting only
the disposable `dbuser_mysql` role's `deadlock_timeout` to 10 ms. The improved
completion count and drain time show that the detection delay is a substantial
contributor. The diagnostic also had zero duplicates and passed both integrity
checks. The role override was removed and a fresh MySQL session verified the
original 1 s setting. No kernel default was changed. This short experiment does
not establish a recommended production setting or resolve general contention
performance.

## Reproduction and evidence

On disposable fixtures, set the usual PG*/MYSQL_* connection variables from
[TESTING.md](TESTING.md), then run:

```sh
python3 tests/point_write_concurrency.py
MYSQL_HOST=reference-host MYSQL_PORT=3306 python3 tests/point_write_concurrency.py --reference
MYSQL_PORT=3306 THREADS=8 TABLES=4 SIZE=20000 TIME=30 tests/sysbench.sh
```

Raw evidence is retained outside Git at
`/tmp/openhalo-19.0.0-review-20261120/sysbench-fix`:

- `reproduce-before.log` / `reproduce-after.log`: exact two-session behavior on
  OpenHalo MySQL, native PG and MySQL 5.7.
- `point-write-deadlock-before.log`, `point-write-idle-before.log` and
  `server-acceptance.log`: the two recovery defects before their fixes.
- `{u24,d12,asan}-integration/status.tsv` and
  `macos-debug-integration/integration-status.tsv`: final suite statuses and
  sibling individual logs; `point-write-reference.log` and `pgxs-point-write.log`.
- `core-final.log`, `isolation-final.log`, final `server-*.log`, kernel/extension
  build/install logs and `pgxs-{build,install}.log`.
- `{openhalo,mysql57}-{eight,hot16}/summary.json` and sibling workload logs;
  `openhalo-hot16-10ms/` contains the detection-delay diagnostic. `stress.py`
  records exact commands and verifies table integrity.

The source, deterministic test and this report belong in Git. Build products,
databases and raw logs are disposable task artifacts outside the repository.

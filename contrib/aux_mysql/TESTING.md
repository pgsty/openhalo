# aux_mysql 2.0 / PostgreSQL 19 integration checks

Use disposable instances with the matching kernel and extension. Build debug
(`--enable-debug --enable-cassert`) and release binaries separately; enable
`--enable-depend`, or clean all objects after changing node/protocol headers.
Keep build directories, data directories and logs outside this repository.

`tests/run.sh` recreates its `test` schema. The transaction and AB suites also
exercise global read-only flags: never point these suites at a shared production
instance. Configure SCRAM authentication and TLS before running driver/auth tests;
a trust HBA rule cannot verify password rejection. The existing smoke clients use
the documented fixture role `dbuser_mysql` and its test password.

Set connections for one isolated instance:

```sh
export PGBIN=/path/to/install/bin
export PGHOST=127.0.0.1 PGPORT=5792 PGDATABASE=postgres PGUSER=postgres
export PGPASSWORD='fixture-password'
export MYSQL_HOST=127.0.0.1 MYSQL_PORT=3792 MYSQL_USER=dbuser_mysql
export MYSQL_PASSWORD='fixture-password' MYSQL_PWD="$MYSQL_PASSWORD"
export LOG_DIR=/tmp/aux-mysql-acceptance-debug
cd contrib/aux_mysql
bash tests/run.sh
```

The smoke suite checks 26 groups, including 46 AB cases, prepared packets,
comments, authentication, Go and Connector/J. Missing dependencies are incomplete
verification, not a pass. `JAR` selects the Connector/J jar.
The SQL checks select UTF8MB4 explicitly and compare stdout and stderr
separately. Only MariaDB's failed-statement echo is omitted; result tables and
error numbers, SQLSTATEs, line numbers, messages and order still match exactly.

The following suites create unique application schemas and remove them on exit:

```sh
python3 tests/release_dml.py
python3 tests/release_fixes.py --port "$MYSQL_PORT" --reference-port 3771
python3 tests/binary_fix/mysql_check.py --port "$MYSQL_PORT"
python3 tests/review_fix_sql_generated.py --ports "$MYSQL_PORT" --password "$MYSQL_PASSWORD"
python3 tests/review_fix_generated_export.py --ports "$MYSQL_PORT" --password "$MYSQL_PASSWORD"
python3 tests/review_fix_generated_policy.py --ports "$MYSQL_PORT" --password "$MYSQL_PASSWORD"
python3 tests/review_fix_pending_fk.py --ports "$MYSQL_PORT" --password "$MYSQL_PASSWORD"
python3 tests/route_a_fk_privileges.py
python3 tests/review_fix_collation_mapping.py --mysql-port "$MYSQL_PORT" --pg-port "$PGPORT"
python3 tests/indexes_mysql.py
python3 tests/indexes_btree.py
python3 tests/release_txn.py --ports "$MYSQL_PORT" --pg-port "$PGPORT" \
  --psql "$PGBIN/psql" --allow-global-read-only
```

They cover all-key ODKU/REPLACE, FK/cascade/IGNORE behavior, repeated keys and
concurrent writers, trigger ordering, limited-column privileges, byte and NUL
preservation, generated values/ALTER/LIKE, missing-parent FK declarations,
MD5-collision keys, short-key ordered plans, prefix concurrency, queued EXECUTE,
RESET and runtime read-only enforcement. The collation test covers exact and
approximated names, warning lifetimes, expression `COLLATE` in DML/CTAS/views,
PREPARE/EXECUTE, multi-statement boundaries and COM_RESET_CONNECTION. The
index-plan and DML tests also need
a PostgreSQL connection; their PG role must be able to create fixture roles and
triggers. The transaction suite changes global flags only with its explicit flag
and restores them in cleanup.

The Route A FK privilege suite creates two non-superuser roles and verifies
schema USAGE, table/column REFERENCES, implicit composite primary keys,
revocation, parent recreation and privileged pg_dump/restore. Denied binding
must retain the declaration and old rows, warn 1142, and let parent DDL finish.
This enforces PG ownership; MySQL 5.7 does not check equivalent REFERENCES ACLs.

The generated-expression policy suite checks rejection of nondeterministic
expressions and preserves legitimate compatibility functions and sql_mode
conversions, including constant generators after repeated prepared executions.
It also creates legacy wrappers through a PG connection (default: MySQL port
plus 2000, or explicit `--pg-ports`) to check ALTER/LIKE and repair paths.
When changing an approved compatibility implementation, review the validator's
implementation identities/body fingerprints and rerun these checks.
VIRTUAL storage, virtual primary keys and references to
AUTO_INCREMENT follow the documented OpenHalo choices; do not make MySQL's
rejection of the latter two a reason to remove working PG capabilities.

For reference comparisons, `review_fix_sql_generated.py`,
`review_fix_generated_export.py` and `review_fix_pending_fk.py` accept multiple `--ports`. `indexes_mysql.py` works
against MySQL 5.7 directly. `release_dml.py` accepts `MYSQL_REFERENCE_PORT` to
repeat SQL/concurrency cases; `release_txn.py --ports 3771` runs its session-only
reference cases without changing global flags.

PG19-specific and recovery checks:

```sh
python3 tests/pg19_kernel.py
python3 tests/release_review.py
python3 tests/point_write_concurrency.py
python3 tests/hotspot_deadlocks.py
python3 tests/adversarial_fixes.py
python3 tests/tier1_optimizations.py
python3 tests/pg19_upgrade.py --old-prefix /path/to/openhalo18-aux2 \
  --new-prefix /path/to/openhalo19 --work-dir /tmp/openhalo-major-upgrade
python3 tests/release_roundtrip.py
```

`pg19_kernel.py` verifies serialized `ON CONFLICT DO SELECT FOR UPDATE`, actual
row-lock contention, native query cancellation, PostgreSQL direct TLS negotiation,
MySQL TLS, escaped literals and default-expression round trips. It needs `PGBIN`,
PG/MySQL connection settings, psycopg2 and PyMySQL, and a TLS-enabled fixture.
`release_roundtrip.py` also needs Oracle MySQL 5.7 dump/restore clients; select them
with `MYSQLDUMP` and `MYSQL_CLIENT`.

`release_review.py` checks the 19.0.0 product identity independently of PG19 and
aux_mysql 2.0; readonly version variables and SET GLOBAL privilege errors,
including their wire SQLSTATEs; RSA/TLS authentication, password rotation,
NOLOGIN and expiry; RLS on REPLACE/ODKU; cancellation and connection reuse;
malformed handshakes/commands and oversized preauthentication packets; binary
input/output spanning MySQL's 16 MiB frame boundary; and eight concurrent native
PG/MySQL writers with committed and rolled-back updates. It requires a superuser
PG connection and a TLS/SCRAM fixture, creates unique roles/schema, and removes
them on exit. RSA checks explicitly disable client TLS auto-upgrade.

`point_write_concurrency.py` observes lock waits to force DELETE/INSERT races;
checks UPDATE values and predicates, composite/text keys, binary parameters,
native writers, triggers, cached plans and index invalidation; and verifies
whole-transaction rollback and SQLSTATE 40001 after deadlocks. It also checks
that idle transaction timeouts do not disconnect an already-rolled-back victim.
Cancellation/lock-timeout cases must retain earlier writes. Use `--reference`
with `MYSQL_HOST`/`MYSQL_PORT` for the portable cases against MySQL 5.7;
OpenHalo-only cases require a superuser PG connection. Both modes use a unique
schema and remove it on exit.

`sysbench.sh` retries only 1213/1205. Errors 1062/1020 fail the workload immediately.
Its exit status still requires zero ignored errors, including genuine deadlocks;
report the completed workloads and retry counts separately. `MYSQL_HOST` selects
the target for reference comparisons.

`hotspot_deadlocks.py` checks default `mysql.deadlock_timeout` (`50ms`) detection in
2/3/16-session cycles, explicit session `deadlock_timeout` overrides, native/mixed
sessions, ordinary waits, cancellation, lock timeouts and soft cycles resolved by
queue reordering, plus 16-thread and 64-thread transfer runs against a commit ledger
(including retries without an explicit MySQL ROLLBACK). It requires administrative
PG/MySQL connections; `MYSQL_ADMIN_USER`/`MYSQL_ADMIN_PASSWORD` default to
`PGUSER`/`PGPASSWORD`.

`tier1_optimizations.py` covers `openhalo_version.h` relocation, stored
`INSERT IGNORE` catalog serialization in rules and `BEGIN ATOMIC` functions,
column ACL enforcement on spoofed `RETURNING` aliases, dynamic `log_lock_waits`
suppression and override precedence, unlogged `mysql_prefix` `INIT_FORKNUM`
metapages, binary `COM_STMT_EXECUTE` `CachedPlanSource` fast-path parity and
loopback QPS speedup, and concurrent `MysCurrentRead` point writes across `HASH`
and multilevel dropped/reordered partitioned tables.

For repeated sysbench hotspot stress, use a disposable fixture and an output
directory outside the source tree:

```sh
python3 tests/hotspot_stress.py --output /tmp/openhalo-hotspot-16 \
  --threads 16 --size 100 --seconds 30 --repeat 3
```

The stress runner creates/removes a unique schema, varies the seed between
repetitions, records mean/P99/maximum latency and retries, and verifies primary
key cardinality/range/sum after every workload. Only 1213 is retryable; 1062,
1205 and other errors fail. It stops failed sysbench workers promptly, records
the failure and enforces a bounded drain time. This does not change
`sysbench.sh`'s stricter zero-retry gate. `--protocol disable` exercises text
queries; `--distribution uniform` changes the key distribution.

For full upstream coverage, configure with `--enable-tap-tests
--enable-injection-points` and the libraries for the selected optional modules,
then run `make check-world` with `TEMP_CONFIG` disabling the MySQL listener.
`PG_TEST_EXTRA='ssl libpq_encryption'` enables the additional TLS/encryption
tests. ECPG's preprocessor TAP tests write generated C files beside their input;
give those tests a writable copy outside the repository when using read-only
source mounts. Do not run separate `make check` invocations concurrently in one
build tree: each recreates `tmp_install`.
For Docker, use container-native storage for test databases and an init process
(`--init`) that reaps orphaned children. Shared host filesystems can behave
differently for permissions and directory operations; unreaped postmaster
processes interfere with deliberate SIGKILL/restart tests. For an affected leaf
suite, `make check NO_TEMP_INSTALL=1` reuses the already prepared temporary
installation; preserve the Makefile's exported test environment when rerunning.

Historical 1.5 → 1.9 extension upgrade suites and PG14 import generators were
removed by the 2.0 source cleanup; they belong to the PG18 release checkout.
`tests/adversarial_fixes.py` covers `1.9 → 2.0` catalog/ACL parity, fast-path
WAL/lock invariants, `LIMIT 1` / `VARCHAR` parameter current reads, deferred
wire errors, `transaction_timeout` recovery, `mysql.deadlock_timeout` reload
precedence, handshake `program_name` sanitization, `utf8mb4_general_ci`
planner selectivity, and `SET` keyword variables.

Run PostgreSQL's own regression suite against the debug kernel with the cluster's
bootstrap superuser and a writable build tree:

```sh
PGHOST=/path/to/socket PGPORT=5792 PGUSER=bootstrap_user \
  make -C /path/to/build-debug/src/test/regress installcheck
```

## Route A compatibility checks

The standalone PG18 harnesses are historical aids; validate this port against the
matching OpenHalo 19 kernel and extension.

`python3 tests/route_a.py --port "$MYSQL_PORT" --reference-port 3771 --wp text`
asserts character functions, mixed expressions, value/HEX assignments, and the
intentional 1366 boundary for NUL in every character family and write mode.
The other work packages are selected with `--wp binary`, `char`, `collation`,
`collation_all`, `numeric_cast`, `uint64`, `prefix_nul`, and `hints`.
`collation_all` enumerates every character collation returned by MySQL 5.7
SHOW COLLATION (221 names), on VARCHAR/CHAR/TEXT, checking all substring-search
entry points and their UTF8 logical results. Binary-character-set columns are
covered by `binary`. Language/encoding approximations explicitly assert 1105.
The numeric CAST check also uses
`PSQL` (or `PGBIN/psql`, then psql on PATH) and honors `PGHOST`, `PGPORT`
and `PGUSER`. Without those settings it uses `/tmp`, the MySQL port plus 2000
and user `postgres`.

`release_fixes.py` covers the final release review corrections: FIND_IN_SET
trailing spaces in both directions without changing FIELD/STRCMP/LIKE/IN;
boolean string/aggregate values and HEX; TRUE/FALSE, predicate and IF
assignments to integer/BOOL/DECIMAL/FLOAT/DOUBLE/string/binary columns in both
sql_modes, unique-key ODKU/REPLACE against seeded rows, generated columns,
DEFAULT, prepared parameters and CAST/CONVERT;
character boolean defaults in CREATE/ALTER/LIKE, SHOW CREATE replay, inherited
defaults, native PG controls, wrong-row ODKU/REPLACE, rollback/commit and repeated
SQL/binary prepared executions;
general_ci/bin ORDER BY, GROUP BY, DISTINCT, equality joins and index lookups
over PAD SPACE, control characters below space, accents and supplementary
characters; CHAR/VARCHAR IFNULL/IF, NULL and
unknown literals in both PAD modes; connection initialization spellings and
session variables; text/prepared result charset metadata and raw binary rows;
and uint64 statement/trigger rollback inside an explicit transaction.
`route_a.py --wp uint64` also seeds an existing INT64_MAX key before above-uint64
INSERT, IGNORE, INSERT SELECT, UPDATE, ODKU and REPLACE. Never restore the old
INT64_MAX clipping expectation: it hid wrong-row updates and deletions.

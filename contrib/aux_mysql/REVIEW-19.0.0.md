# OpenHalo 19.0.0 review and acceptance

Review date: 2026-11-20. Input: `3741a0c6827013d7b7fba3e8b33145cef47399e9`.
This records a new verification round after the PostgreSQL 19.0 port. The earlier
port results in [PG19.md](PG19.md) are not counted as new test executions here.
The candidate is not a published release.

## Version identity

| Interface | Value |
|---|---|
| OpenHalo product | `19.0.0`, defined in `src/include/openhalo_version.h` |
| Native SQL | `SHOW openhalo.version` → `19.0.0` |
| MySQL SQL | `@@version_comment` / SHOW VARIABLES → `OpenHalo 19.0.0 (PostgreSQL 19.0)` |
| PostgreSQL kernel/API | `19.0`, `PG_VERSION_NUM=190000` |
| Extension installation SQL | `aux_mysql 2.0` |
| MySQL handshake / `@@version` | Configured compatibility value, default `5.7.32-log` |

Product identification does not change the PostgreSQL catalog version, extension
SQL version or the MySQL client compatibility setting. Native product identity
is an internal, read-only GUC. MySQL version identifiers reject SET.

## Findings fixed

1. `SET version_comment` could report success without changing the returned
   identity. Both version identifiers now reject assignment with MySQL error
   **1238 / HY000**, including global/session forms. The grammar also accepts the
   `VERSION` keyword as a SET variable name so rejection reaches the correct
   runtime check. MySQL 5.7.44 supplied the reference behavior.
2. SET GLOBAL correctly denied ordinary users but returned a generic table
   privilege error. It now reports **1227 / 42000**; ordinary table and auth-cache
   privilege errors keep their existing mapping. The new test reads raw ERR
   packets to verify SQLSTATE as well as errno.
3. A custom REPLACE no-op branch still passed `CMD_INSERT` to
   `ExecProcessReturning()`. PG19 expects a boolean `isDelete`; the call now passes
   `false`. Its old/new slots were identical in this branch, so this review did
   not reproduce a wrong result from the old argument.
4. The configuration sample retained tab formatting and redundant active
   settings that failed PG19's sample consistency test. Defaults are documented
   in the normal commented format. The test recognizes only the intentional
   active `shared_preload_libraries = 'aux_mysql'` setting and retains its other
   checks. The first-connection extension-installation comment was corrected.

The source review covered the kernel changes against pristine PG19: startup and
authentication routing, cancellation, protocol error/GUC responses, stored-node
serialization, privileges and RLS, REPLACE/ODKU/IGNORE execution, sequence state,
FK checks, collation callbacks and index method tables. Extension review also
covered prepared-value rendering and lifetimes, packet bounds, credential-cache
invalidation and generated-expression implementation validation. Runtime tests
support these checks; they are not a proof that every possible input is correct.

## New test executions

| Build / test family | Result |
|---|---|
| macOS arm64, debug/assertions and release | Both rebuilt and installed; core regression **243/243 on each** |
| macOS arm64, debug isolation | **139/139** |
| macOS MySQL integration | **13/13 suites on each build** |
| macOS PG19 kernel and release boundaries | Both builds passed the kernel checks and **7/7 boundary groups** |
| Client/driver smoke | **26/26 groups**, no skips; Oracle client, mysqlslap, PyMySQL, Go and Connector/J |
| MySQL 5.7.44 reference comparisons | **9/9 Route A groups**, including 221 collation names; release fixes **922 assertions** |
| Ubuntu 24.04 arm64, GCC 13, debug/assertions | Build/install and **15/15 integration suites**; final wire-error checks passed after the final rebuild |
| Debian 12 amd64, GCC 12, release, emulated on arm64 | Build/install and **15/15 integration suites** |
| PGXS, Ubuntu arm64 | Actual standalone build and staged installation; **15/15 suites** using the independently built library |
| ASan + UBSan, Ubuntu arm64 | Instrumented kernel and extension; **15/15 suites**, no sanitizer diagnostics in the server log |
| Ubuntu `check-world` | Complete configured coverage after the corrected leaf reruns described below; the initial full invocation exited 2 |
| Debian `check-world` | Complete configured run **exited 0**; core **243/243**, isolation **139/139**, recovery **58 scripts / 797 checks**, subscriptions **39 scripts / 619 checks** |
| SASLprep exhaustive codepoint-range test | Passed with the optional `saslprep` test enabled |
| PG18.6 / aux_mysql 2.0 → PG19.0 / aux_mysql 2.0 | `pg_upgrade --check`, copy upgrade and post-upgrade behavior checks passed |
| Logical backup | Native pg_dump/pg_restore, MySQL 5.7 dump/replay, and unchanged dump restored to MySQL 5.7 all passed |
| Physical replication | Native and MySQL writes replayed with exact binary/generated values; MySQL standby rejects writes; promotion restores MySQL writes and AUTO_INCREMENT behavior |

The seven new boundary groups verify immutable version identity; RSA and TLS
authentication, password rotation, NOLOGIN, expiry and privileged operations;
hidden-row RLS on REPLACE/ODKU; native cancellation of a MySQL query and session
reuse; malformed handshakes/commands and oversized preauthentication messages;
binary values crossing the 16 MiB frame boundary; and eight mixed-protocol
writers with **800 committed increments and 160 rolled-back updates**.

Linux builds enabled OpenSSL, ICU, LZ4, Zstd, XML/XSLT, Perl, Python, UUID, TAP
tests and injection points. The sanitizer build used `-O1 -g
-fsanitize=address,undefined -fno-omit-frame-pointer`, assertions,
`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. This checks address/undefined
behavior failures, not leak freedom. Builds and test databases are disposable.

## Concurrency result and remaining boundaries

The eight-thread sysbench run completed all four workloads, 30 seconds each,
with four 20,000-row tables and server-side prepared statements. Point reads and
read-only transactions had zero ignored errors. Read/write and write-only runs
reported **60 and 459** ignored errors respectively; the server log identifies
duplicate primary-key conflicts during concurrent DELETE/INSERT on the same
rows. The harness therefore returned failure for its strict zero-error gate
(`completed=4/4`, `error_free=2/4`). A separate one-thread run passed **4/4** with
zero errors. These results do not establish InnoDB-equivalent locking behavior;
applications using this pattern must handle transaction retries. No performance
baseline is claimed while other builds and tests share the host.

There is no aux_mysql **1.x → 2.0** update script. The major-upgrade rehearsal
starts with the preserved PG18.6/aux_mysql 2.0 source baseline; it does not prove
that an existing released 1.5/1.9 installation can be upgraded in place. See
[MIGRATION.md](MIGRATION.md). The local MySQL 26.7 dump client's repeated SESSION
assignment syntax remains outside the validated 5.7 dump path. Meson, Windows,
third-party binary extensions, and unconfigured optional authentication/build
features are not covered by this acceptance round. C extensions must be rebuilt
against matching OpenHalo headers.

The configured upstream runs include core/isolation, client tools, contrib,
procedural languages, authentication, TLS/libpq encryption, injection points,
backup verification, pg_upgrade, recovery and logical subscription tests.
Windows/SSPI tests are inapplicable; DNS load-balancing, OAuth, and the optional
ten-billion-transaction XID-wraparound stress tests were not enabled. The
optional exhaustive SASLprep check was run separately and passed.

The final Debian log contains 62 passing TAP suite summaries reporting **27,374
checks** in total, plus four `NOTESTS` summaries for skipped optional or
platform-specific suites. These are separate from the regression-test counts
above. No failing test or make error was reported in that final invocation.

The reviewed changes are suitable for the development main branch within this
documented scope. The sysbench zero-error gate did not pass, and this review
does not qualify a direct aux_mysql 1.x upgrade or constitute a production
release certification.

## Evidence and reproduction

Evidence root: `/tmp/openhalo-19.0.0-review-20261120` (outside Git).

- `macos-*-core.log`, `macos-debug-isolation.log`, `macos-*-integration/`,
  `release-review-macos-*.log`, `pg19-kernel-macos-*.log`.
- `smoke-macos-release/`, `reference-status.tsv`, `reference-*.log`,
  `release-fixes.log`, `major-upgrade/`, `roundtrip.log`.
- `u24-check-world-final.log`, `d12-check-world-final.log`, build trees' TAP logs
  and regression diffs; `u24-test-decoding-*.log`, `u24-test-misc-*.log` and
  `u24-misc-final.log`, `u24-verifybackup-final.log`, `u24-recovery-native.log`,
  `u24-saslprep-extra.log`.
- `u24-integration/`, `d12-integration/`, `pgxs-integration/`, `asan-integration/`,
  `*-runtime-server.log`, `pgxs-*-final.log`, `replication.log`.
- `u24-sysbench/`, `u24-sysbench-single/` and their summary logs.

The evidence root also contains the build, fixture, integration and replication
commands used in this round. [TESTING.md](TESTING.md) documents the maintained
tests, including `tests/release_review.py`.

Initial harness failures are retained separately. ECPG needs writable test
inputs; its fixture was copied outside Git. Concurrent make checks recreate
`tmp_install`, so checks sharing a build tree were serialized. Docker's shared
filesystem also caused install permission and replication-slot directory errors;
the shell installer and container-native test storage were used for the affected
checks. Logical decoding then passed **21 regression, 16 isolation and 10 TAP
checks**. The corrected test_misc suite passed **13 scripts / 199 checks**, and
the backup corruption/permission script passed **92 checks** on native storage.
The container's original PID 1 did not reap deliberately killed postmasters;
using a child subreaper fixed the shared-memory restart test. The first 15
recovery scripts had passed 237 checks; the remaining **43 scripts / 560 checks**
passed with native storage and child reaping, covering all 58 recovery scripts.
Logical subscriptions passed **39 scripts / 619 checks**. The complete Ubuntu
invocation's four failing leaf families are thus accounted for; its exit code is
retained rather than rewritten as success. Debian uses the corrected fixture.
Early sanitizer client attempts before fixture initialization completed are
excluded; the complete post-initialization run passed.

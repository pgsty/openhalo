# Contributing

OpenHalo 19 is developed at [`pgsty/openHalo`](https://github.com/pgsty/openHalo). Bug
reports and pull requests are welcome. For a compatibility bug, the most useful report
is the statement, what MySQL returns and what OpenHalo returns.

## Branches

| Branch | Content |
| :--- | :--- |
| `codex/pg19.0` | OpenHalo 19: PostgreSQL 19.0 with protocol and execution hooks, plus `contrib/aux_mysql` |
| `pg14` | openHalo 1.0 on PostgreSQL 14 (tag `v1.0-pg14`); historical source of the imported MySQL grammar and SQL layer |

Releases are tagged `v<PostgreSQL version>.<revision>` on the release branch; see [`CHANGELOG.md`](CHANGELOG.md).

## Build

Build with GNU make (Meson is not supported). A development build with assertions:

```sh
mkdir -p /path/to/build-openhalo
cd /path/to/build-openhalo
/path/to/source/configure --prefix=$HOME/openhalo --with-openssl --enable-debug --enable-cassert --enable-depend
make -j$(nproc) && make install             # kernel and contrib/aux_mysql
```

The kernel and `aux_mysql` share node and callback layouts: always build and install them
together, with `--enable-depend` or from a clean tree after changing a header. Keep build
trees, data directories and logs outside the repository.

## Test

```sh
printf '%s\n' 'mysql.listener_on = off' > /tmp/openhalo-regression.conf
make -C /path/to/build-openhalo check TEMP_CONFIG=/tmp/openhalo-regression.conf
cd /path/to/source
contrib/aux_mysql/tests/kernel_instance.sh <prefix> <pgdata> <pgport> <mysqlport>
PGBIN=<prefix>/bin PGPORT=<pgport> MYSQL_PORT=<mysqlport> contrib/aux_mysql/tests/run.sh
```

`kernel_instance.sh` creates a disposable instance with the fixture account the suites
use; never point the suites at an instance holding real data.
[`contrib/aux_mysql/TESTING.md`](contrib/aux_mysql/TESTING.md) lists the integration,
upgrade and round-trip suites and what each one needs.

Compare behavior with a real MySQL 5.7 server rather than with the manual alone: most
suites take a reference port and run the same statements against both servers.

## Changing `aux_mysql`

- **Parser and SQL sources.** Maintain `src/parser/mys_gram.y`, `mys_scan.l` and
  `sql/aux_mysql--2.0.sql` directly. GNU make generates the C parser, scanner and
  keyword headers in the build directory. The PG14 import generators belonged
  to the earlier port; [`UPSTREAM.md`](contrib/aux_mysql/UPSTREAM.md) is its historical log.
- **SQL objects.** A change to the extension's SQL objects needs a new extension version
  with an update script from the last released one, so that existing installations can
  run `ALTER EXTENSION aux_mysql UPDATE`. The `Makefile` shows how the install and update
  scripts are assembled.
- **Compatibility choices.** The target is legacy MySQL 5.7 applications, including
  permissive SQL: accept what MySQL accepts and use the nearest semantics, with a
  warning where exact behavior is unavailable. Raise an error only where MySQL does under
  the same `sql_mode`, or to prevent a crash or silent data loss. See the end of
  "What works" in [`contrib/aux_mysql/README.md`](contrib/aux_mysql/README.md), and list
  a deliberate difference under "Known gaps" there.
- **Tests.** Add a case that fails without the change, and check its expected values on
  MySQL 5.7.

## Debugging a MySQL session

A MySQL connection is served by a regular PostgreSQL backend. Find its process and
attach a debugger to it:

```
mysql> SELECT pg_backend_pid();
```

```sh
gdb -p <pid>
```

Server messages go to the PostgreSQL log; `SHOW WARNINGS` shows what the session received.

## Licenses

Kernel changes are under the PostgreSQL License and `contrib/aux_mysql` is under the
GPL v3, as described in the [README](README.md#license). Keep the existing copyright
headers of the files you touch.

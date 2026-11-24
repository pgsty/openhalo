#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
"""PG19 port regressions on a disposable instance (PG* and MYSQL_* settings).

Checks stored ON CONFLICT DO SELECT locking, native cancellation and TLS,
MySQL string escaping and execution of extension expressions with LLVM JIT.
Requires psycopg2, PyMySQL, a TLS-enabled server and matching PGBIN.
"""
import concurrent.futures
import os
import pathlib
import subprocess
import time
import uuid

import psycopg2
import pymysql


SCHEMA = "pg19_port_" + uuid.uuid4().hex[:10]


def pg():
    connection = psycopg2.connect(connect_timeout=5)
    connection.autocommit = True
    return connection


def sql(connection, statement):
    with connection.cursor() as cursor:
        cursor.execute(statement)
        return cursor.fetchall() if cursor.description else None


def main():
    admin = pg()
    mysql = None
    try:
        assert sql(admin, "SHOW server_version_num") == [("190000",)]
        assert sql(admin, "SELECT extversion FROM pg_extension WHERE extname='aux_mysql'") == [("2.0",)]
        sql(admin, f"CREATE SCHEMA {SCHEMA}")
        sql(admin, f"CREATE TABLE {SCHEMA}.native(id int PRIMARY KEY, value text)")
        sql(admin, f"INSERT INTO {SCHEMA}.native VALUES(1,'original')")
        sql(admin, f"""CREATE FUNCTION {SCHEMA}.get_existing()
            RETURNS SETOF {SCHEMA}.native LANGUAGE SQL BEGIN ATOMIC
            INSERT INTO {SCHEMA}.native VALUES(1,'replacement')
            ON CONFLICT(id) DO SELECT FOR UPDATE RETURNING *;
            END""")

        # Reload the serialized function body in another backend, then prove
        # the FOR UPDATE clause survives by testing its actual row lock.
        locker = pg()
        try:
            definition = sql(locker, f"SELECT pg_get_functiondef('{SCHEMA}.get_existing()'::regprocedure)")[0][0]
            assert "FOR UPDATE" in definition.upper(), definition
            locker.autocommit = False
            assert sql(locker, f"SELECT * FROM {SCHEMA}.get_existing()") == [(1, "original")]
            sql(admin, "SET lock_timeout='200ms'")
            try:
                sql(admin, f"UPDATE {SCHEMA}.native SET value='unlocked' WHERE id=1")
            except psycopg2.errors.LockNotAvailable:
                pass
            else:
                raise AssertionError("stored DO SELECT lost its FOR UPDATE lock")
        finally:
            locker.rollback()
            locker.close()
            sql(admin, "RESET lock_timeout")
        print("PASS stored ON CONFLICT DO SELECT retains row locking", flush=True)

        sleeper = pg()
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                future = pool.submit(sql, sleeper, "SELECT pg_sleep(20)")
                for _ in range(100):
                    state = sql(admin, "SELECT wait_event FROM pg_stat_activity WHERE pid=" + str(sleeper.get_backend_pid()))
                    if state == [("PgSleep",)]:
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError("sleep query did not become active")
                sleeper.cancel()
                try:
                    future.result(timeout=5)
                except psycopg2.errors.QueryCanceled:
                    pass
                else:
                    raise AssertionError("native cancel request was ignored")
        finally:
            sleeper.close()
        print("PASS native PostgreSQL cancellation", flush=True)

        psql = pathlib.Path(os.environ["PGBIN"]) / "psql"
        result = subprocess.run([str(psql), "sslmode=require sslnegotiation=direct",
                                 "-XAt", "-v", "ON_ERROR_STOP=1", "-c", "SELECT 19"],
                                check=True, capture_output=True, text=True)
        assert result.stdout.strip() == "19", result.stdout
        print("PASS PostgreSQL direct TLS negotiation", flush=True)

        mysql = pymysql.connect(host=os.getenv("MYSQL_HOST", "127.0.0.1"),
            port=int(os.environ["MYSQL_PORT"]), user=os.getenv("MYSQL_USER", "dbuser_mysql"),
            password=os.getenv("MYSQL_PASSWORD", "Mysql.123"), autocommit=True,
            read_timeout=10, write_timeout=10, ssl={"check_hostname": False})
        assert mysql._secure, "MySQL TLS was not negotiated"
        with mysql.cursor() as cursor:
            cursor.execute(r"SELECT HEX('a\\b'), HEX('a\nb')")
            assert cursor.fetchone() == ("615C62", "610A62")
            cursor.execute(f"USE {SCHEMA}")
            cursor.execute(r"CREATE TABLE strings(v varchar(40) DEFAULT 'a\\b')")
            cursor.execute("INSERT INTO strings VALUES(DEFAULT)")
            cursor.execute("SELECT HEX(v) FROM strings")
            assert cursor.fetchone() == ("615C62",)
            cursor.execute("SHOW CREATE TABLE strings")
            definition = cursor.fetchone()[1]
            cursor.execute("DROP TABLE strings")
            cursor.execute(definition)
            cursor.execute("INSERT INTO strings VALUES(DEFAULT)")
            cursor.execute("SELECT HEX(v) FROM strings")
            assert cursor.fetchone() == ("615C62",)
        print("PASS MySQL TLS, escaped literals and default DDL round trip", flush=True)

        sql(admin, "SET jit=on")
        if sql(admin, "SELECT pg_jit_available()") == [(True,)]:
            for option in ("jit_above_cost", "jit_inline_above_cost", "jit_optimize_above_cost"):
                sql(admin, f"SET {option}=0")
            plan = sql(admin, """EXPLAIN (ANALYZE, FORMAT JSON, TIMING OFF)
                SELECT sum(length(mysql.part_list_columns(i))) FROM generate_series(1,20) AS g(i)""")[0][0][0]
            assert plan["JIT"]["Functions"] > 0 and plan["JIT"]["Options"]["Inlining"], plan
            print("PASS LLVM JIT compiled and executed an extension expression", flush=True)
    finally:
        if mysql:
            mysql.close()
        sql(admin, f"DROP SCHEMA IF EXISTS {SCHEMA} CASCADE")
        admin.close()


if __name__ == "__main__":
    main()

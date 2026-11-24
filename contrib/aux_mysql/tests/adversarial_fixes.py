#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
"""Deterministic regression suite for OpenHalo 19.0.0 review & hardening fixes.

Uses PG*/MYSQL_* fixture variables (PGBIN, PGHOST, PGPORT, PGUSER, PGPASSWORD,
MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWORD). Creates disposable objects
in a unique schema / temporary database and cleans them up on exit.
"""
from concurrent.futures import ThreadPoolExecutor
import os
import re
import shutil
import socket
import subprocess
import tempfile
import time
import uuid

import psycopg2
import pymysql

from point_write_concurrency import WireError, query, wire_dml


def pg_conn(dbname=None, user=None, password=None):
    c = psycopg2.connect(
        host=os.getenv('PGHOST', '127.0.0.1'),
        port=int(os.getenv('PGPORT', '5432')),
        dbname=dbname or os.getenv('PGDATABASE', 'postgres'),
        user=user or os.getenv('PGUSER', 'postgres'),
        password=password if password is not None else os.getenv('PGPASSWORD', ''),
        connect_timeout=5,
    )
    c.autocommit = True
    return c


def my_conn(user=None, password=None, program_name=None, duplicate_prog=False):
    kwargs = dict(
        host=os.getenv('MYSQL_HOST', '127.0.0.1'),
        port=int(os.getenv('MYSQL_PORT', '3306')),
        user=user or os.getenv('MYSQL_ADMIN_USER', os.getenv('PGUSER', 'postgres')),
        password=password if password is not None else os.getenv(
            'MYSQL_ADMIN_PASSWORD', os.getenv('PGPASSWORD', '')),
        autocommit=True,
        connect_timeout=5,
        read_timeout=15,
        write_timeout=15,
    )
    if program_name is not None:
        kwargs['program_name'] = program_name
    c = pymysql.connect(**kwargs)
    return c


def wait_blocked(pg, pid, pending, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if query(pg, 'SELECT cardinality(pg_blocking_pids(%s))', (pid,))[0][0]:
            return
        if pending.done():
            raise AssertionError(('statement did not block', pending.result()))
        time.sleep(0.01)
    raise AssertionError(f'pid {pid} never entered lock wait')


def test_wal_and_fastpath_locks(pg, my, schema, pgbin):
    query(pg, 'CREATE EXTENSION IF NOT EXISTS pg_walinspect')
    query(my, f'USE {schema}')
    query(my, 'CREATE TABLE plain_t(id INT PRIMARY KEY, v INT)')
    query(my, 'CREATE TABLE auto_t(id INT AUTO_INCREMENT PRIMARY KEY, v INT)')
    query(my, 'INSERT INTO plain_t VALUES (1, 10), (2, 20)')
    query(my, 'INSERT INTO auto_t(v) VALUES (10), (20)')

    # 1a. Plain table uncontended UPDATE & DELETE + AUTO_INCREMENT DELETE emit 0 LOCK WAL records.
    lsn0 = query(pg, 'SELECT pg_current_wal_insert_lsn()::text')[0][0]
    assert query(my, 'UPDATE plain_t SET v = 11 WHERE id = 1') == 1
    assert query(my, 'DELETE FROM plain_t WHERE id = 2') == 1
    assert query(my, 'DELETE FROM auto_t WHERE id = 2') == 1
    lsn1 = query(pg, 'SELECT pg_current_wal_insert_lsn()::text')[0][0]
    lock_recs = query(pg, """
        SELECT record_type FROM pg_get_wal_records_info(%s::pg_lsn, %s::pg_lsn)
        WHERE resource_manager = 'Heap' AND record_type LIKE 'LOCK%%'
    """, (lsn0, lsn1))
    assert lock_recs == [], f'unexpected XLOG_HEAP_LOCK records on fast path: {lock_recs}'

    # 1b. AUTO_INCREMENT UPDATE emits no extra LOCK WAL beyond native PG's BEFORE trigger lock (1 vs old 2).
    lsn_pg0 = query(pg, 'SELECT pg_current_wal_insert_lsn()::text')[0][0]
    query(pg, f'UPDATE {schema}.auto_t SET v = 98 WHERE id = 1')
    lsn_pg1 = query(pg, 'SELECT pg_current_wal_insert_lsn()::text')[0][0]
    native_locks = query(pg, """
        SELECT record_type FROM pg_get_wal_records_info(%s::pg_lsn, %s::pg_lsn)
        WHERE resource_manager = 'Heap' AND record_type LIKE 'LOCK%%'
    """, (lsn_pg0, lsn_pg1))

    lsn2 = query(pg, 'SELECT pg_current_wal_insert_lsn()::text')[0][0]
    assert query(my, 'UPDATE auto_t SET v = 99 WHERE id = 1') == 1
    lsn3 = query(pg, 'SELECT pg_current_wal_insert_lsn()::text')[0][0]
    lock_recs_auto = query(pg, """
        SELECT record_type FROM pg_get_wal_records_info(%s::pg_lsn, %s::pg_lsn)
        WHERE resource_manager = 'Heap' AND record_type LIKE 'LOCK%%'
    """, (lsn2, lsn3))
    assert len(lock_recs_auto) <= len(native_locks), (lock_recs_auto, native_locks)
    print('PASS decoded WAL confirms zero XLOG_HEAP_LOCK on fast-path point writes', flush=True)


def test_fk_keyshare_and_no_phantom_locks(pg, schema):
    a, b, check = my_conn(), my_conn(), pg_conn()
    try:
        for c in (a, b):
            query(c, f'USE {schema}')
        query(a, 'CREATE TABLE p_fk(id INT PRIMARY KEY, v INT)')
        query(a, 'CREATE TABLE c_fk(cid INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p_fk(id))')
        query(a, 'INSERT INTO p_fk VALUES (1, 10)')

        # 2a. Parent non-key point UPDATE holds LockTupleNoKeyExclusive -> child INSERT does not block!
        query(a, 'BEGIN')
        assert query(a, 'UPDATE p_fk SET v = 20 WHERE id = 1') == 1
        assert query(b, 'INSERT INTO c_fk VALUES (100, 1)') == 1
        query(a, 'COMMIT')

        # 2b. Even after child FK placed KEY_SHARE on parent, parent UPDATE still emits 0 LOCK WAL.
        lsn0 = query(pg, 'SELECT pg_current_wal_insert_lsn()::text')[0][0]
        assert query(a, 'UPDATE p_fk SET v = 30 WHERE id = 1') == 1
        lsn1 = query(pg, 'SELECT pg_current_wal_insert_lsn()::text')[0][0]
        locks = query(pg, """
            SELECT record_type FROM pg_get_wal_records_info(%s::pg_lsn, %s::pg_lsn)
            WHERE resource_manager = 'Heap' AND record_type LIKE 'LOCK%%'
        """, (lsn0, lsn1))
        assert locks == [], f'FK KEY_SHARE parent lost fast path: {locks}'

        # 3. Contended key-moving UPDATE (id=1 -> id=2) must NOT leave phantom lock on id=2!
        query(a, 'CREATE TABLE km(id INT PRIMARY KEY, v INT)')
        query(a, 'INSERT INTO km VALUES (1, 10)')
        query(a, 'BEGIN')
        query(b, 'BEGIN')
        assert query(a, 'UPDATE km SET id = 2 WHERE id = 1') == 1
        with ThreadPoolExecutor(max_workers=1) as pool:
            f = pool.submit(query, b, 'UPDATE km SET v = 99 WHERE id = 1')
            wait_blocked(pg, b.thread_id(), f)
            query(a, 'COMMIT')
            assert f.result(timeout=5) == 0
        # Session b is STILL inside BEGIN! Competing FOR UPDATE NOWAIT on id=2 must succeed immediately!
        query(check, f'SET search_path = {schema}')
        query(check, 'BEGIN')
        assert query(check, 'SELECT id FROM km WHERE id = 2 FOR UPDATE NOWAIT') == [(2,)]
        query(check, 'ROLLBACK')
        query(b, 'ROLLBACK')

        # 4. Non-matching residual WHERE leaves row unlocked both uncontended and after replacement!
        query(b, 'BEGIN')
        assert query(b, 'UPDATE km SET v = 88 WHERE id = 2 AND v = 999') == 0
        query(check, 'BEGIN')
        assert query(check, 'SELECT id FROM km WHERE id = 2 FOR UPDATE NOWAIT') == [(2,)]
        query(check, 'ROLLBACK')
        query(b, 'ROLLBACK')

        query(a, 'BEGIN')
        query(b, 'BEGIN')
        query(a, 'DELETE FROM km WHERE id = 2')
        query(a, 'INSERT INTO km VALUES (2, 50)')
        with ThreadPoolExecutor(max_workers=1) as pool:
            f = pool.submit(query, b, 'UPDATE km SET v = 88 WHERE id = 2 AND v = 999')
            wait_blocked(pg, b.thread_id(), f)
            query(a, 'COMMIT')
            assert f.result(timeout=5) == 0
        query(check, 'BEGIN')
        assert query(check, 'SELECT id FROM km WHERE id = 2 FOR UPDATE NOWAIT') == [(2,)]
        query(check, 'ROLLBACK')
        query(b, 'ROLLBACK')

        # 4b. Volatile targetlist expression must not take unlocked fast path and leave phantom EPQ lock!
        query(pg, f"""
            CREATE FUNCTION {schema}.slow_val() RETURNS int LANGUAGE plpgsql VOLATILE AS $$
            BEGIN
                PERFORM pg_sleep(0.25);
                RETURN 777;
            END $$
        """)
        query(b, 'BEGIN')
        with ThreadPoolExecutor(max_workers=1) as pool:
            f = pool.submit(query, b, f'UPDATE km SET v = {schema}.slow_val() WHERE id = 2')
            time.sleep(0.08)
            # b already locked id=2 before evaluating volatile slow_val(), closing the EPQ key-move window!
            query(check, 'BEGIN')
            try:
                query(check, 'SELECT id FROM km WHERE id = 2 FOR UPDATE NOWAIT')
            except psycopg2.Error as e:
                assert e.pgcode == '55P03', e
            else:
                raise AssertionError('volatile targetlist took unlocked fast path before slow_val()!')
            query(check, 'ROLLBACK')
            assert f.result(timeout=5) == 1
        query(b, 'ROLLBACK')
        print('PASS FK KEY SHARE concurrency, no phantom key-move lock, and unlocked false-WHERE rows', flush=True)
    finally:
        for c in (a, b, check):
            c.close()


def test_spoofed_trigger_name(pg, schema):
    my = my_conn()
    try:
        query(my, f'USE {schema}')
        query(pg, f'CREATE TABLE {schema}.trig_spoof(id INT PRIMARY KEY, v INT)')
        query(pg, f'CREATE TABLE {schema}.trig_calls(n INT)')
        query(pg, f'INSERT INTO {schema}.trig_spoof VALUES (1, 10)')
        query(pg, f"""
            CREATE FUNCTION {schema}.fake_autoinc() RETURNS trigger LANGUAGE plpgsql AS $$
            BEGIN
                INSERT INTO {schema}.trig_calls VALUES (1);
                RETURN NULL;
            END $$
        """)
        # Deliberately spoof the internal trigger name 'aux_mysql_auto_increment'!
        query(pg, f"""
            CREATE TRIGGER aux_mysql_auto_increment
            BEFORE UPDATE ON {schema}.trig_spoof
            FOR EACH ROW EXECUTE FUNCTION {schema}.fake_autoinc()
        """)
        assert query(my, 'UPDATE trig_spoof SET v = 20 WHERE id = 1') == 0
        calls = query(pg, f'SELECT count(*) FROM {schema}.trig_calls')[0][0]
        assert calls == 1, f'spoofed trigger fired {calls} times instead of 1!'
        print('PASS spoofed BEFORE UPDATE trigger name fires once (tgfoid verified)', flush=True)
    finally:
        my.close()


def test_varchar_param_and_limit_unwrap(pg, schema):
    a, b = my_conn(), my_conn()
    try:
        for c in (a, b):
            query(c, f'USE {schema}')
        query(a, 'CREATE TABLE vc(k VARCHAR(32) PRIMARY KEY, v INT)')
        query(a, "INSERT INTO vc VALUES ('k1', 1)")

        # 6a. RelabelType(Param) on VARCHAR prepared plan under DELETE+INSERT race
        query(b, "SELECT set_config('mysql.parser', 'postgres', false)")
        query(b, "SET plan_cache_mode = 'force_generic_plan'")
        query(b, f'PREPARE vcupd(varchar) AS UPDATE {schema}.vc SET v = v + 1 WHERE k = $1')
        plan_txt = '\n'.join(r[0] for r in query(b, "EXPLAIN EXECUTE vcupd('k1')"))
        assert 'MySQL Current Read' in plan_txt, plan_txt

        query(a, 'BEGIN')
        query(a, "DELETE FROM vc WHERE k = 'k1'")
        query(a, "INSERT INTO vc VALUES ('k1', 10)")
        query(b, 'BEGIN')
        with ThreadPoolExecutor(max_workers=1) as pool:
            f = pool.submit(query, b, "EXECUTE vcupd('k1')")
            wait_blocked(pg, b.thread_id(), f)
            query(a, 'COMMIT')
            assert f.result(timeout=5) == 1
        query(b, 'COMMIT')
        assert query(a, "SELECT v FROM vc WHERE k = 'k1'") == ((11,),)
        query(b, "SELECT set_config('mysql.parser', 'mysql', false)")

        # 6b. ORM DELETE/UPDATE ... WHERE id = 1 LIMIT 1 unwraps to MySQL Current Read
        query(a, 'CREATE TABLE lim(id INT PRIMARY KEY, v INT)')
        query(a, 'INSERT INTO lim VALUES (1, 2), (2, 0)')
        plan_lim = '\n'.join(r[0] for r in query(a, 'EXPLAIN DELETE FROM lim WHERE id = 1 LIMIT 1'))
        assert 'MySQL Current Read' in plan_lim and 'SubPlan' not in plan_lim, plan_lim

        query(a, 'BEGIN')
        query(a, 'DELETE FROM lim WHERE id = 1')
        query(a, 'INSERT INTO lim VALUES (1, 50)')
        query(b, 'BEGIN')
        with ThreadPoolExecutor(max_workers=1) as pool:
            f = pool.submit(query, b, 'DELETE FROM lim WHERE id = 1 LIMIT 1')
            wait_blocked(pg, b.thread_id(), f)
            query(a, 'COMMIT')
            assert f.result(timeout=5) == 1
        query(b, 'INSERT INTO lim VALUES (1, 60)')
        query(b, 'COMMIT')
        assert query(a, 'SELECT id, v FROM lim ORDER BY id') == ((1, 60), (2, 0))

        # 6c. Adversarial user IN-subquery with LIMIT 1 MUST NOT unwrap to wrong row!
        query(a, 'TRUNCATE TABLE lim')
        query(a, 'INSERT INTO lim VALUES (1, 2), (2, 0)')
        assert query(a, 'UPDATE lim SET v = 99 WHERE id IN (SELECT v FROM lim WHERE id = 1 LIMIT 1)') == 1
        assert query(a, 'SELECT id, v FROM lim ORDER BY id') == ((1, 2), (2, 99))

        # 6d. Column-level SELECT ACL preserved when LIMIT 1 wrapper is unwrapped!
        role = 'lim_acl_' + uuid.uuid4().hex[:8]
        query(pg, f"CREATE ROLE {role} LOGIN PASSWORD 'LimAcl.123'")
        try:
            query(pg, f'GRANT USAGE ON SCHEMA {schema} TO {role}')
            query(pg, f'GRANT UPDATE (v) ON {schema}.lim TO {role}')
            uconn = my_conn(user=role, password='LimAcl.123')
            try:
                query(uconn, f'USE {schema}')
                try:
                    query(uconn, 'UPDATE lim SET v = 77 WHERE id = 1 LIMIT 1')
                except pymysql.MySQLError as e:
                    assert e.args[0] == 1142 or 'permission denied' in str(e).lower(), e
                else:
                    raise AssertionError('unwrapped LIMIT 1 bypassed SELECT(id) column ACL!')
                query(pg, f'GRANT SELECT (id) ON {schema}.lim TO {role}')
                assert query(uconn, 'UPDATE lim SET v = 77 WHERE id = 1 LIMIT 1') == 1
                # Scalar subquery in SET with LIMIT 1 must not hit cassert in pull_varattnos
                assert query(uconn, 'UPDATE lim SET v = (SELECT 88) WHERE id = 1 LIMIT 1') == 1
                assert query(a, 'SELECT v FROM lim WHERE id = 1') == ((88,),)
                # Correlated ctid inside scalar subquery must STILL require SELECT(ctid) privilege!
                try:
                    query(uconn, 'UPDATE lim SET v = (SELECT CASE WHEN lim.ctid IS NOT NULL THEN 99 ELSE 0 END) WHERE id = 1 LIMIT 1')
                except pymysql.MySQLError as e:
                    assert e.args[0] == 1142 or 'permission denied' in str(e).lower(), e
                else:
                    raise AssertionError('correlated ctid inside subquery bypassed SELECT(ctid) ACL!')
            finally:
                uconn.close()
        finally:
            query(pg, f'REVOKE ALL ON {schema}.lim FROM {role}')
            query(pg, f'REVOKE ALL ON SCHEMA {schema} FROM {role}')
            query(pg, f'DROP ROLE {role}')

        # 6e. Runtime index key context survives retry with detoasted short-varlena text key!
        query(pg, f'CREATE TABLE {schema}.keysrc(k TEXT)')
        query(pg, f"INSERT INTO {schema}.keysrc VALUES (repeat('k', 40))")
        query(pg, f"""
            CREATE FUNCTION {schema}.fetch_key() RETURNS text LANGUAGE sql STABLE AS $$
                SELECT k FROM {schema}.keysrc LIMIT 1
            $$
        """)
        query(pg, f'CREATE TABLE {schema}.rtk(k TEXT PRIMARY KEY, v INT)')
        query(pg, f"INSERT INTO {schema}.rtk VALUES (repeat('k', 40), 1)")
        query(b, "SELECT set_config('mysql.parser', 'postgres', false)")
        query(b, "SET plan_cache_mode = 'force_generic_plan'")
        query(b, f'PREPARE rtk_upd(int, text) AS UPDATE {schema}.rtk SET v = $1 WHERE k = $2')
        query(a, 'BEGIN')
        query(a, f"DELETE FROM rtk WHERE k = repeat('k', 40)")
        query(a, f"INSERT INTO rtk VALUES (repeat('k', 40), 10)")
        query(b, 'BEGIN')
        with ThreadPoolExecutor(max_workers=1) as pool:
            f = pool.submit(query, b, f'EXECUTE rtk_upd(42, {schema}.fetch_key())')
            wait_blocked(pg, b.thread_id(), f)
            query(a, 'COMMIT')
            f.result(timeout=5)
        query(b, 'COMMIT')
        query(b, 'DEALLOCATE rtk_upd')
        query(b, "RESET plan_cache_mode")
        query(b, "SELECT set_config('mysql.parser', 'mysql', false)")
        assert query(a, f"SELECT v FROM rtk WHERE k = repeat('k', 40)") == ((42,),)
        print('PASS VARCHAR RelabelType Param, LIMIT 1 current read, runtime key retry, and column ACL', flush=True)
    finally:
        a.close(); b.close()


def test_deferred_error_wire_ordering(pg, schema):
    a, b, retry_conn = my_conn(), my_conn(), pg_conn()
    try:
        for c in (a, b):
            query(c, f'USE {schema}')
        query(a, 'CREATE TABLE dw(id INT PRIMARY KEY, pad TEXT)')
        big = 'x' * 70000  # Exceeds 64KB outbuf auto-flush threshold!
        query(a, 'INSERT INTO dw VALUES (1, %s), (2, %s)', (big, big))
        query(retry_conn, f'SET search_path = {schema}')

        query(a, 'BEGIN')
        query(b, 'BEGIN')
        # Victim b also locks an independent row id=99 earlier in its transaction
        query(a, 'INSERT INTO dw VALUES (98, %s)', ('a',))
        query(b, 'INSERT INTO dw VALUES (99, %s)', ('b',))
        query(a, 'UPDATE dw SET pad = %s WHERE id = 1', (big,))
        query(b, 'UPDATE dw SET pad = %s WHERE id = 2', (big,))

        def run_dml(conn, other):
            try:
                wire_dml(conn, f"UPDATE dw SET pad = '{big}' WHERE id = {other}")
                return 'ok'
            except WireError as e:
                assert e.args[0] == 1213, e
                # The instant 1213 arrives on the wire, victim's outer-xact locks
                # MUST ALREADY be released!
                rem = query(retry_conn, 'SELECT count(*) FROM pg_locks WHERE pid = %s AND granted',
                            (conn.thread_id(),))[0][0]
                assert rem == 0, f'victim still held {rem} granted locks after 1213 arrived on wire!'
                return 'victim'

        with ThreadPoolExecutor(max_workers=2) as pool:
            fa = pool.submit(run_dml, a, 2)
            wait_blocked(pg, a.thread_id(), fa)
            fb = pool.submit(run_dml, b, 1)
            res = {fa.result(timeout=8), fb.result(timeout=8)}
        assert res == {'ok', 'victim'}, res
        query(a, 'ROLLBACK'); query(b, 'ROLLBACK')
        print('PASS >64KB deferred ERROR flushed strictly after transaction lock cleanup', flush=True)
    finally:
        for c in (a, b, retry_conn):
            c.close()


def test_transaction_timeout_survival(schema):
    # 8a. With statement_rollback=on, recoverable 1062 error must NOT disarm transaction_timeout!
    c = my_conn()
    try:
        query(c, f'USE {schema}')
        query(c, 'CREATE TABLE tt(id INT PRIMARY KEY)')
        query(c, 'INSERT INTO tt VALUES (1)')
        query(c, "SELECT set_config('transaction_timeout', '350ms', false)")
        query(c, "SELECT set_config('idle_in_transaction_session_timeout', '5s', false)")
        query(c, 'BEGIN')
        query(c, 'INSERT INTO tt VALUES (2)')
        try:
            query(c, 'INSERT INTO tt VALUES (1)')
        except pymysql.MySQLError as e:
            assert e.args[0] == 1062, e
        time.sleep(0.55)
        try:
            query(c, 'SELECT 1')
        except pymysql.MySQLError:
            pass
        else:
            raise AssertionError('transaction_timeout was disarmed by statement rollback!')
    finally:
        try:
            c.close()
        except Exception:
            pass

    # 8b. With statement_rollback=off, aborted block keeps native behavior (does not rearm transaction_timeout)
    c2 = my_conn()
    try:
        query(c2, f'USE {schema}')
        query(c2, "SELECT set_config('mysql.statement_rollback', 'off', false)")
        query(c2, "SELECT set_config('transaction_timeout', '300ms', false)")
        query(c2, "SELECT set_config('idle_in_transaction_session_timeout', '0', false)")
        query(c2, 'BEGIN')
        try:
            query(c2, 'INSERT INTO tt VALUES (1)')
        except pymysql.MySQLError as e:
            assert e.args[0] == 1062, e
        time.sleep(0.45)
        # Connection remains alive in aborted block until ROLLBACK
        query(c2, 'ROLLBACK')
        assert query(c2, 'SELECT count(*) FROM tt') == ((1,),)
    finally:
        c2.close()

    # 8c. Subtransaction changing transaction_timeout to 60s before failing must restore outer 350ms deadline!
    pg_adm = pg_conn()
    c3 = my_conn()
    try:
        query(pg_adm, f"""
            CREATE FUNCTION {schema}.spoof_timeout() RETURNS int LANGUAGE plpgsql AS $$
            BEGIN
                PERFORM set_config('transaction_timeout', '0', true);
                PERFORM set_config('transaction_timeout', '60s', true);
                RAISE EXCEPTION 'boom after timeout spoof';
            END $$
        """)
        query(c3, f'USE {schema}')
        query(c3, "SELECT set_config('transaction_timeout', '350ms', false)")
        query(c3, 'BEGIN')
        query(c3, 'INSERT INTO tt VALUES (3)')
        try:
            query(c3, f'SELECT {schema}.spoof_timeout()')
        except pymysql.MySQLError:
            pass
        time.sleep(0.55)
        try:
            query(c3, 'SELECT 1')
        except pymysql.MySQLError:
            pass
        else:
            raise AssertionError('failed subtransaction replaced outer transaction_timeout with 60s!')
        print('PASS transaction_timeout survives statement-rollback error, spoofed subxact timer, and statement_rollback=off', flush=True)
    finally:
        pg_adm.close()
        try:
            c3.close()
        except Exception:
            pass


def test_deadlock_timeout_guc_lifecycle(pg):
    m = my_conn()
    try:
        # Default in MySQL session is 50ms, native PG session stays 1s
        assert query(pg, 'SHOW deadlock_timeout')[0][0] == '1s'
        assert query(m, "SELECT current_setting('deadlock_timeout')")[0][0] == '50ms'

        # Session SET overrides dynamic default; RESET restores 50ms!
        query(m, "SELECT set_config('deadlock_timeout', '250ms', false)")
        assert query(m, "SELECT current_setting('deadlock_timeout')")[0][0] == '250ms'
        query(m, "SELECT set_config('mysql.parser', 'postgres', false)")
        query(m, 'RESET ALL')
        assert query(m, "SELECT current_setting('deadlock_timeout')")[0][0] == '50ms'

        # ALTER SYSTEM deadlock_timeout = '200ms' (PGC_S_GLOBAL > PGC_S_DYNAMIC_DEFAULT) wins on reload;
        # RESET via ALTER SYSTEM restores MySQL's 50ms dynamic default;
        # mysql.deadlock_timeout = 0 withdraws override back to 1s!
        try:
            query(pg, "ALTER SYSTEM SET deadlock_timeout = '200ms'")
            query(pg, 'SELECT pg_reload_conf()')
            time.sleep(0.1)
            assert query(m, "SELECT current_setting('deadlock_timeout')")[0][0] == '200ms'

            query(pg, 'ALTER SYSTEM RESET deadlock_timeout')
            query(pg, 'SELECT pg_reload_conf()')
            time.sleep(0.1)
            assert query(m, "SELECT current_setting('deadlock_timeout')")[0][0] == '50ms'

            # Explicit deadlock_timeout = '1s' (matching boot_val) removed on reload must still reconcile existing session to 50ms!
            query(pg, "ALTER SYSTEM SET deadlock_timeout = '1s'")
            query(pg, 'SELECT pg_reload_conf()')
            time.sleep(0.1)
            m_boot = my_conn()
            try:
                assert query(m_boot, "SELECT current_setting('deadlock_timeout')")[0][0] == '1s'
                query(pg, 'ALTER SYSTEM RESET deadlock_timeout')
                query(pg, 'SELECT pg_reload_conf()')
                time.sleep(0.1)
                assert query(m_boot, "SELECT current_setting('deadlock_timeout')")[0][0] == '50ms'
            finally:
                m_boot.close()

            query(pg, 'ALTER SYSTEM SET "mysql.deadlock_timeout" = 0')
            query(pg, 'SELECT pg_reload_conf()')
            time.sleep(0.1)
            assert query(m, "SELECT current_setting('deadlock_timeout')")[0][0] == '1s'
        finally:
            query(pg, 'ALTER SYSTEM RESET deadlock_timeout')
            query(pg, 'ALTER SYSTEM RESET "mysql.deadlock_timeout"')
            query(pg, 'SELECT pg_reload_conf()')
            time.sleep(0.1)
        assert query(m, "SELECT current_setting('deadlock_timeout')")[0][0] == '50ms'
        print('PASS mysql.deadlock_timeout precedence, reload reconciliation, and 0-withdrawal', flush=True)
    finally:
        m.close()


def test_program_name_sanitization(pg):
    dirty = 'evil\nLOG: forged\r\x1b[31mapp\x80'
    m = my_conn(program_name=dirty)
    try:
        app = query(pg, 'SELECT application_name FROM pg_stat_activity WHERE pid = %s',
                    (m.thread_id(),))[0][0]
        assert '\n' not in app and '\r' not in app and '\x1b' not in app, repr(app)
        assert all(32 <= ord(ch) <= 126 for ch in app), repr(app)
        assert 'evil' in app and 'forged' in app, repr(app)
        print('PASS handshake program_name sanitized via pg_clean_ascii:', repr(app), flush=True)
    finally:
        m.close()


def test_collation_selectivity_and_hash_compat(pg, schema):
    m = my_conn()
    try:
        query(m, f'USE {schema}')
        # 11a. Persistent hash values under utf8mb4_general_ci match BE16 PAD SPACE representation
        # and equal strings ('abc' vs 'ABC   ') hash identically!
        h = query(pg, """
            SELECT hashtext('abc' COLLATE mysql.utf8mb4_general_ci),
                   hashtext('ABC   ' COLLATE mysql.utf8mb4_general_ci),
                   hashtext('abd' COLLATE mysql.utf8mb4_general_ci)
        """)[0]
        assert h[0] == h[1] and h[0] != h[2], h

        # 11b. Within-histogram-bin range selectivity under utf8mb4_general_ci must interpolate monotonically!
        # Use a low statistics target (2 bins over 1000 distinct ASCII keys 'k0000'..'k0999') so
        # narrow vs wide sub-ranges inside a single histogram bin rely on convert_string_datum(pg_strxfrm).
        query(m, 'CREATE TABLE sel_t(s VARCHAR(32))')
        query(pg, f'ALTER TABLE {schema}.sel_t ALTER COLUMN s SET STATISTICS 2')
        query(pg, f"""
            INSERT INTO {schema}.sel_t
            SELECT 'k' || lpad(i::text, 4, '0') FROM generate_series(0, 999) i
        """)
        query(pg, f'ANALYZE {schema}.sel_t')

        def est_rows(cond):
            plan = query(pg, f"EXPLAIN SELECT * FROM {schema}.sel_t WHERE {cond}")[0][0]
            m_rows = re.search(r'rows=(\d+)', plan)
            assert m_rows, plan
            return int(m_rows.group(1))

        narrow = est_rows("s >= 'k0100' AND s <= 'k0120'")
        medium = est_rows("s >= 'k0100' AND s <= 'k0300'")
        wide = est_rows("s >= 'k0100' AND s <= 'k0480'")
        assert narrow < medium < wide, (narrow, medium, wide)

        # 11b2. PAD SPACE selectivity with control bytes < 0x20 ('A\x01' < 'A' < 'A!')
        query(m, 'CREATE TABLE pad_ctrl(s VARCHAR(32))')
        query(pg, f'ALTER TABLE {schema}.pad_ctrl ALTER COLUMN s SET STATISTICS 2')
        query(pg, f"""
            INSERT INTO {schema}.pad_ctrl
            SELECT CASE
                WHEN i < 300 THEN 'A' || chr(1) || lpad(i::text, 3, '0')
                WHEN i < 600 THEN 'A'
                ELSE 'A!' || lpad(i::text, 3, '0')
            END
            FROM generate_series(0, 999) i
        """)
        query(pg, f'ANALYZE {schema}.pad_ctrl')
        p_ctrl = query(pg, f"EXPLAIN SELECT * FROM {schema}.pad_ctrl WHERE s >= ('A' || chr(1)) AND s <= 'A'")[0][0]
        assert 'rows=' in p_ctrl, p_ctrl

        # 11c. Exercise utf8mb4_bin abbreviated keys including empty strings
        query(m, 'CREATE TABLE bin_t(s VARCHAR(32) COLLATE utf8mb4_bin)')
        query(pg, f"""
            INSERT INTO {schema}.bin_t
            SELECT CASE WHEN i % 10 = 0 THEN '' ELSE md5(i::text) END
            FROM generate_series(1, 500) i
        """)
        cnt = query(m, 'SELECT count(*) FROM (SELECT s FROM bin_t ORDER BY s) sub')[0][0]
        assert cnt == 500
        print(f'PASS utf8mb4_general_ci hash stability & monotonic range selectivity ({narrow} < {medium} < {wide})', flush=True)
    finally:
        m.close()


def test_set_keyword_variables():
    m = my_conn()
    try:
        # Unknown variable in SESSION / LOCAL must report 1193 with actual variable name (not 'SESSION')!
        for scope in ('', 'SESSION ', 'LOCAL '):
            try:
                query(m, f'SET {scope}completely_bogus_var = 1')
            except pymysql.MySQLError as e:
                assert e.args[0] == 1193 and 'completely_bogus_var' in e.args[1], (scope, e)
            else:
                raise AssertionError(f'SET {scope}completely_bogus_var succeeded unexpectedly')

        # Keyword variable names parse cleanly without 1064 syntax error:
        try:
            query(m, 'SET SESSION timestamp = DEFAULT')
        except pymysql.MySQLError as e:
            assert e.args[0] == 1193 and 'timestamp' in e.args[1], e
        try:
            query(m, 'SET SESSION version = "8.0"')
        except pymysql.MySQLError as e:
            assert e.args[0] == 1238, e
        query(m, "SET GLOBAL delay_key_write = 'ON'")
        query(m, "SET GLOBAL sql_buffer_result = 0")
        print('PASS SET keyword variable names and 1193 unknown-variable diagnostics', flush=True)
    finally:
        m.close()


def test_dead_end_backend_and_abi_symbol(pgbin):
    # 12 & 13. Spin up a tiny max_connections=2 disposable instance to verify:
    #  (a) RegisterProtocolExtensionSized symbol is exported and 1-arg symbol is gone;
    #  (b) CAC_TOOMANY over MySQL handshake exits cleanly as B_DEAD_END_BACKEND with cassert enabled!
    nm_out = subprocess.check_output(['nm', '-g', os.path.join(pgbin, 'postgres')], text=True)
    assert 'RegisterProtocolExtensionSized' in nm_out, 'RegisterProtocolExtensionSized missing'
    assert not re.search(r'\b_?RegisterProtocolExtension$', nm_out, re.M), 'legacy 1-arg symbol still exported!'

    tmp = tempfile.mkdtemp(prefix='openhalo_cac_')
    pgport = 15892
    myport = 13892
    try:
        subprocess.check_call(
            [os.path.join(pgbin, 'initdb'), '-U', 'postgres', '-D', tmp, '-A', 'trust', '-E', 'UTF8', '--no-locale'],
            stdout=subprocess.DEVNULL,
        )
        with open(os.path.join(tmp, 'postgresql.conf'), 'a') as f:
            f.write(f"\nport = {pgport}\nunix_socket_directories = '{tmp}'\n"
                    f"mysql.port = {myport}\nmax_connections = 2\nsuperuser_reserved_connections = 0\n")
        subprocess.check_call(
            [os.path.join(pgbin, 'pg_ctl'), '-D', tmp, '-l', os.path.join(tmp, 'server.log'), 'start'],
            stdout=subprocess.DEVNULL,
        )
        try:
            # Create non-superuser role and fill both connection slots
            adm = psycopg2.connect(host=tmp, port=pgport, user='postgres', dbname='postgres')
            adm.autocommit = True
            query(adm, 'CREATE EXTENSION IF NOT EXISTS aux_mysql')
            query(adm, "CREATE ROLE u_cac LOGIN PASSWORD 'Cac.123'")
            adm.close()

            c1 = psycopg2.connect(host=tmp, port=pgport, user='u_cac', dbname='postgres')
            c2 = psycopg2.connect(host=tmp, port=pgport, user='u_cac', dbname='postgres')
            try:
                try:
                    c3 = pymysql.connect(host='127.0.0.1', port=myport, user='u_cac',
                                         password='Cac.123', connect_timeout=5)
                    c3.close()
                except pymysql.MySQLError as e:
                    assert 'too many clients' in str(e).lower() or e.args[0] in (1040, 2013, 1105), e
                else:
                    raise AssertionError('3rd connection succeeded despite max_connections=2')
            finally:
                c1.close(); c2.close()

            # Server must still be healthy (no assertion crash in dead-end backend!)
            adm2 = psycopg2.connect(host=tmp, port=pgport, user='postgres', dbname='postgres')
            assert query(adm2, 'SELECT 1') == [(1,)]
            adm2.close()
            with open(os.path.join(tmp, 'server.log')) as lf:
                logtxt = lf.read()
            assert 'TRAP:' not in logtxt and 'FailedAssertion' not in logtxt, logtxt
        finally:
            subprocess.call([os.path.join(pgbin, 'pg_ctl'), '-D', tmp, 'stop', '-m', 'immediate'],
                            stdout=subprocess.DEVNULL)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('PASS CAC_TOOMANY dead-end MySQL handshake and RegisterProtocolExtensionSized ABI guard', flush=True)


def test_extension_upgrade_parity(pg, pgbin):
    prefix = os.path.abspath(os.path.join(pgbin, '..'))
    extdir = os.path.join(prefix, 'share', 'postgresql', 'extension')
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
    parts = [
        'aux_mysql.sql', 'aux_mysql_additions.sql', 'aux_mysql_binary.sql',
        'aux_mysql_fix_binary.sql', 'aux_mysql_fix_indexes.sql', 'aux_mysql_fix_compat.sql',
    ]
    sql19 = ''.join(
        subprocess.check_output(['git', '-C', repo, 'show', f'v18.6.1:contrib/aux_mysql/sql/{p}'], text=True)
        for p in parts
    )
    sql19_path = os.path.join(extdir, 'aux_mysql--1.9.sql')
    prev_sql19 = open(sql19_path, 'rb').read() if os.path.exists(sql19_path) else None
    db_up = 'up19_' + uuid.uuid4().hex[:8]
    db_fr = 'fr20_' + uuid.uuid4().hex[:8]
    with open(sql19_path, 'w') as f:
        f.write(sql19)
    try:
        query(pg, f'CREATE DATABASE {db_up}')
        query(pg, f'CREATE DATABASE {db_fr}')
        cup = pg_conn(dbname=db_up)
        cfr = pg_conn(dbname=db_fr)
        try:
            query(cup, "CREATE EXTENSION aux_mysql VERSION '1.9'")
            query(cup, 'CREATE VIEW public.dep_v AS SELECT table_name FROM mys_informa_schema.tables')
            # Private non-extension table inside mysql schema must NOT receive PUBLIC SELECT on upgrade!
            query(cup, 'CREATE TABLE mysql.private_tokens(secret text)')
            query(cup, 'REVOKE ALL ON TABLE mysql.private_tokens FROM PUBLIC')
            # Temp table shadowing pg_class must NOT break or hijack extension upgrade!
            query(cup, 'CREATE TEMP TABLE pg_class AS SELECT * FROM pg_catalog.pg_class LIMIT 0')
            query(cup, "ALTER EXTENSION aux_mysql UPDATE TO '2.0'")
            has_pub = query(cup, "SELECT has_table_privilege('public', 'mysql.private_tokens', 'SELECT')")[0][0]
            assert not has_pub, 'upgrade granted PUBLIC SELECT to private non-extension table!'
            query(cup, 'DROP TABLE mysql.private_tokens')
            query(cup, 'DROP TABLE pg_temp.pg_class')
            query(cup, 'DROP VIEW public.dep_v')
            query(cfr, "CREATE EXTENSION aux_mysql VERSION '2.0'")

            def snap(c):
                p = query(c, """
                    SELECT n.nspname || '.' || p.proname || '(' || pg_get_function_identity_arguments(p.oid) || ')',
                           p.prokind, p.prolang::regproc::text, p.provolatile, p.proisstrict, p.prosecdef, p.proparallel,
                           pg_get_function_result(p.oid), trim(p.prosrc), coalesce(p.probin, ''), coalesce(p.proacl::text, '')
                    FROM pg_depend d
                    JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'aux_mysql'
                    JOIN pg_proc p ON p.oid = d.objid AND d.classid = 'pg_proc'::regclass
                    JOIN pg_namespace n ON n.oid = p.pronamespace
                    ORDER BY 1
                """)
                r = query(c, """
                    SELECT n.nspname || '.' || c.relname, c.relkind,
                           CASE WHEN c.relkind = 'v' THEN pg_get_viewdef(c.oid, true) ELSE '' END,
                           coalesce(c.relacl::text, '')
                    FROM pg_depend d
                    JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'aux_mysql'
                    JOIN pg_class c ON c.oid = d.objid AND d.classid = 'pg_class'::regclass
                    JOIN pg_namespace n ON n.oid = c.relnamespace
                    ORDER BY 1
                """)
                acl = query(c, """
                    SELECT p.oid::regprocedure::text, coalesce(p.proacl::text, '')
                    FROM pg_proc p WHERE proname IN ('pg_read_file', 'pg_read_binary_file') ORDER BY 1
                """)
                casts = query(c, """
                    SELECT castsource, casttarget, castcontext
                    FROM pg_cast
                    WHERE (castsource = 23 AND casttarget = 16)
                       OR (castsource = 16 AND casttarget = 23)
                    ORDER BY 1, 2
                """)
                return p, r, acl, casts

            assert snap(cup) == snap(cfr), 'catalog/ACL mismatch after 1.9 -> 2.0 update!'
        finally:
            cup.close(); cfr.close()
    finally:
        if prev_sql19 is not None:
            with open(sql19_path, 'wb') as f:
                f.write(prev_sql19)
        elif os.path.exists(sql19_path):
            os.remove(sql19_path)
        query(pg, f'DROP DATABASE IF EXISTS {db_up}')
        query(pg, f'DROP DATABASE IF EXISTS {db_fr}')
    print('PASS aux_mysql 1.9 -> 2.0 extension upgrade achieves 100% catalog & ACL parity', flush=True)


def main():
    pgbin = os.getenv('PGBIN', '/usr/local/pgsql/bin')
    pg = pg_conn()
    my = my_conn()
    schema = 'adv_fix_' + uuid.uuid4().hex[:8]
    try:
        query(my, f'CREATE SCHEMA {schema}')
        test_wal_and_fastpath_locks(pg, my, schema, pgbin)
        test_fk_keyshare_and_no_phantom_locks(pg, schema)
        test_spoofed_trigger_name(pg, schema)
        test_varchar_param_and_limit_unwrap(pg, schema)
        test_deferred_error_wire_ordering(pg, schema)
        test_transaction_timeout_survival(schema)
        test_deadlock_timeout_guc_lifecycle(pg)
        test_program_name_sanitization(pg)
        test_collation_selectivity_and_hash_compat(pg, schema)
        test_set_keyword_variables()
        test_dead_end_backend_and_abi_symbol(pgbin)
        test_extension_upgrade_parity(pg, pgbin)
        print('ALL 12 ADVERSARIAL FIX GROUPS PASSED!', flush=True)
    finally:
        try:
            query(pg, f'DROP SCHEMA IF EXISTS {schema} CASCADE')
        finally:
            my.close(); pg.close()


if __name__ == '__main__':
    main()

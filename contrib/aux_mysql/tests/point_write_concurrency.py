#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
"""Deterministic unique-key write races; PyMySQL and psycopg2.

Use disposable OpenHalo fixtures with PG*/MYSQL_* connection variables.
--reference runs the portable cases against MySQL 5.7 (INNODB_LOCK_WAITS).
Waits are observed in the lock catalog, not inferred from a fixed sleep.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import struct
import time
import uuid

import pymysql


class WireError(pymysql.MySQLError):
    def __init__(self, packet):
        super().__init__(int.from_bytes(packet[1:3], 'little'), packet[9:].decode())
        self.sqlstate = packet[4:9].decode('ascii')


def wire_dml(conn, sql):
    # PyMySQL's normal exception discards SQLSTATE; inspect the ERR packet.
    conn._execute_command(0x03, sql)
    header = conn._read_bytes(4)
    assert header[3] == 1
    data = conn._read_bytes(int.from_bytes(header[:3], 'little'))
    conn._next_seq_id = 2
    if data[0] == 0xff:
        raise WireError(data)
    return pymysql.protocol.OKPacketWrapper(
        pymysql.protocol.MysqlPacket(data, conn.encoding)).affected_rows


def query(conn, sql, args=None):
    with conn.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchall() if cur.description else cur.rowcount


class Prepared:
    def __init__(self, conn, sql):
        self.conn = conn
        conn._execute_command(0x16, sql)
        packet = conn._read_packet().get_all_data()
        self.sid, ncols, self.nparams = struct.unpack('<IHH', packet[1:9])
        assert ncols == 0 and self.nparams == 1
        for _ in range(self.nparams):
            conn._read_packet()
        if not conn.client_flag & (1 << 24):
            conn._read_packet()

    def execute(self, value):
        payload = (struct.pack('<IBI', self.sid, 0, 1) + b'\0\1\x08\0' +
                   struct.pack('<q', value))
        self.conn._execute_command(0x17, payload)
        return self.conn._read_ok_packet().affected_rows

    def close(self):
        self.conn._execute_command(0x19, struct.pack('<I', self.sid))


class Suite:
    def __init__(self, reference):
        self.reference = reference
        self.schema = 'point_write_' + uuid.uuid4().hex[:10]
        self.admin = self.connect()
        self.pg = None
        if not reference:
            import psycopg2
            self.pg = psycopg2.connect(connect_timeout=5)
            self.pg.autocommit = True
        query(self.admin, 'CREATE SCHEMA ' + self.schema)
        query(self.admin, 'USE ' + self.schema)
        query(self.admin, 'CREATE TABLE t(id INT PRIMARY KEY, v INT)')

    def connect(self):
        return pymysql.connect(host=os.getenv('MYSQL_HOST', '127.0.0.1'),
            port=int(os.getenv('MYSQL_PORT', '3306')),
            user=os.getenv('MYSQL_USER', 'dbuser_mysql'),
            password=os.getenv('MYSQL_PASSWORD', 'Mysql.123'),
            autocommit=True, connect_timeout=5, read_timeout=15, write_timeout=15)

    def wait_for_lock(self, pending, conn):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.reference:
                waiting = query(self.admin, '''SELECT COUNT(*) FROM
                    information_schema.innodb_lock_waits w JOIN
                    information_schema.innodb_trx t ON t.trx_id=w.requesting_trx_id
                    WHERE t.trx_mysql_thread_id=%s''', (conn.thread_id(),))[0][0]
            else:
                waiting = query(self.pg, 'SELECT cardinality(pg_blocking_pids(%s))',
                                (conn.thread_id(),))[0][0]
            if waiting:
                return
            if pending.done():
                raise AssertionError(('statement did not wait', pending.result()))
            time.sleep(.2 if self.reference else .01)
        raise AssertionError('lock wait not observed')

    def race(self, label, statements, sql, rows, final, *, rollback=False,
             prepared=False, native_first=False, isolation='REPEATABLE READ',
             after=None, setup=None, table='t', initial='(1,0)'):
        query(self.admin, 'TRUNCATE TABLE ' + table)
        query(self.admin, f'INSERT INTO {table} VALUES{initial}')
        a = self.connect() if not native_first else self.pg
        b = self.connect()
        stmt = None
        try:
            if native_first:
                query(a, 'SET search_path=' + self.schema)
            else:
                query(a, 'USE ' + self.schema)
            query(b, 'USE ' + self.schema)
            query(b, 'SET SESSION TRANSACTION ISOLATION LEVEL ' + isolation)
            if setup:
                setup(b)
            if prepared:
                stmt = Prepared(b, sql)
            query(a, 'BEGIN')
            query(b, 'BEGIN')
            for command in statements:
                query(a, command)
            with ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(stmt.execute, 1) if stmt else pool.submit(query, b, sql)
                try:
                    self.wait_for_lock(pending, b)
                finally:
                    query(a, 'ROLLBACK' if rollback else 'COMMIT')
                actual = pending.result(timeout=5)
                assert actual == rows, (label, actual, rows)
            if after:
                query(b, after)
            query(b, 'COMMIT')
            actual = query(self.admin, f'SELECT * FROM {table} ORDER BY 1')
            assert actual == final, (label, actual, final)
            print('PASS', label, flush=True)
        finally:
            for conn in (a, b):
                query(conn, 'ROLLBACK')
            if stmt:
                stmt.close()
            b.close()
            if not native_first:
                a.close()

    def run(self):
        replace = ['DELETE FROM t WHERE id=1', 'INSERT INTO t VALUES(1,10)']
        for level in ('READ COMMITTED', 'REPEATABLE READ'):
            self.race('delete/reinsert ' + level, replace, 'DELETE FROM t WHERE id=1',
                      1, ((1, 20),), isolation=level, after='INSERT INTO t VALUES(1,20)')
        self.race('binary prepared delete/reinsert', replace, 'DELETE FROM t WHERE id=?',
                  1, ((1, 20),), prepared=True, after='INSERT INTO t VALUES(1,20)')
        self.race('update replacement value', replace, 'UPDATE t SET v=v+1 WHERE id=1',
                  1, ((1, 11),))
        self.race('binary prepared update replacement', replace,
                  'UPDATE t SET v=v+1 WHERE id=?', 1, ((1, 11),), prepared=True)
        self.race('new row fails extra predicate', replace,
                  'UPDATE t SET v=v+1 WHERE id=1 AND v=0', 0, ((1, 10),))
        self.race('new row now matches extra predicate', replace,
                  'UPDATE t SET v=v+1 WHERE id=1 AND v=10', 1, ((1, 11),))
        self.race('concurrent deletion without replacement', ['DELETE FROM t WHERE id=1'],
                  'DELETE FROM t WHERE id=1', 0, ((1, 20),), after='INSERT INTO t VALUES(1,20)')
        self.race('rollback of deleter', replace, 'UPDATE t SET v=v+1 WHERE id=1',
                  1, ((1, 1),), rollback=True)
        self.race('ordinary concurrent update', ['UPDATE t SET v=10 WHERE id=1'],
                  'UPDATE t SET v=v+1 WHERE id=1', 1, ((1, 11),))
        self.race('key move plus reinsert', ['UPDATE t SET id=2 WHERE id=1',
                  'INSERT INTO t VALUES(1,10)'], 'DELETE FROM t WHERE 1=id',
                  1, ((1, 20), (2, 0)), after='INSERT INTO t VALUES(1,20)')
        query(self.admin, 'CREATE TABLE uk(k INT UNIQUE, v INT)')
        self.race('secondary unique index', ['DELETE FROM uk WHERE k=1',
                  'INSERT INTO uk VALUES(1,10)'], 'UPDATE uk SET v=v+1 WHERE k=1',
                  1, ((1, 11),), table='uk')
        query(self.admin, 'CREATE TABLE ck(a INT,b VARCHAR(16),v INT,PRIMARY KEY(a,b))')
        self.race('composite key and case-insensitive comparison',
                  ["DELETE FROM ck WHERE a=1 AND b='key'", "INSERT INTO ck VALUES(1,'KEY',10)"],
                  "UPDATE ck SET v=v+1 WHERE a=1 AND b='key'", 1, ((1, 'KEY', 11),),
                  table='ck', initial="(1,'Key',0)")
        if not self.reference:
            self.race('native writer, MySQL waiter', replace,
                      'DELETE FROM t WHERE id=1', 1, ((1, 20),), native_first=True,
                      after='INSERT INTO t VALUES(1,20)')
            self.pg_cases(replace)
            self.cached_plan()
            self.cancelled_wait()
        self.independent_keys_and_duplicates()
        self.deadlock_rollback()
        self.deadlock_rollback(autocommit=False)
        if not self.reference:
            self.deadlock_rollback(statement_rollback=False)

    def deadlock_rollback(self, autocommit=True, statement_rollback=True):
        query(self.admin, 'CREATE TABLE IF NOT EXISTS marks(id INT PRIMARY KEY)')
        query(self.admin, 'TRUNCATE TABLE marks')
        query(self.admin, 'TRUNCATE TABLE t')
        query(self.admin, 'INSERT INTO t VALUES(1,0),(2,0)')
        a, b = self.connect(), self.connect()
        try:
            for i, conn in enumerate((a, b), 1):
                query(conn, 'USE ' + self.schema)
                if not autocommit:
                    query(conn, 'SET autocommit=0')
                if not statement_rollback:
                    query(conn, "SELECT set_config('mysql.statement_rollback','off',false)")
                if not self.reference and autocommit and statement_rollback:
                    query(conn, "SELECT set_config('idle_in_transaction_session_timeout','500ms',false)")
                query(conn, 'BEGIN')
                query(conn, f'INSERT INTO marks VALUES({i})')
                query(conn, 'SAVEPOINT user_savepoint')
                query(conn, f'UPDATE t SET v=v+1 WHERE id={i}')

            def second_write(conn, own, other):
                try:
                    wire_dml(conn, f'UPDATE t SET v=v+1 WHERE id={other}')
                except WireError as error:
                    assert error.args[0] == 1213 and error.sqlstate == '40001', error
                    if not self.reference and autocommit and statement_rollback:
                        # Recovery must precede the core's idle-state/timer
                        # decision; there is no transaction left to time out.
                        time.sleep(.65)
                    # Inspect before issuing ROLLBACK or another BEGIN: the
                    # server must have discarded all earlier writes already.
                    retained = query(conn, f'SELECT COUNT(*) FROM marks WHERE id={own}')[0][0]
                    query(conn, 'ROLLBACK')
                    return own, retained
                query(conn, 'COMMIT')
                return None

            with ThreadPoolExecutor(max_workers=2) as pool:
                first = pool.submit(second_write, a, 1, 2)
                self.wait_for_lock(first, a)
                second = pool.submit(second_write, b, 2, 1)
                results = [first.result(timeout=8), second.result(timeout=8)]
            victims = [result for result in results if result is not None]
            assert len(victims) == 1 and victims[0][1] == 0, victims
            assert query(self.admin, 'SELECT * FROM t ORDER BY id') == ((1, 1), (2, 1))
            print(f'PASS deadlock 1213/40001 rolls back all writes '
                  f'(autocommit={autocommit}, statement_rollback={statement_rollback})', flush=True)
        finally:
            a.close();b.close()

    def cached_plan(self):
        query(self.pg, f'CREATE TABLE {self.schema}.cached(k int UNIQUE,v int)')
        query(self.pg, f'INSERT INTO {self.schema}.cached VALUES(1,0)')
        a, b = self.connect(), self.connect()
        try:
            for c in (a, b): query(c, 'USE ' + self.schema)
            # Native SQL PREPARE through the MySQL connection exercises a real
            # cached PostgreSQL plan and runtime Param keys (COM_STMT binds are
            # rendered as typed literals by the protocol adapter).
            query(b, "SELECT set_config('mysql.parser','postgres',false)")
            query(b, "SET plan_cache_mode='force_generic_plan'")
            query(b, f'PREPARE wr(int) AS UPDATE {self.schema}.cached SET v=v+1 WHERE k=$1')
            assert query(b, 'EXECUTE wr(999)') == 0
            query(a, 'BEGIN')
            query(a, 'DELETE FROM cached WHERE k=1')
            query(a, 'INSERT INTO cached VALUES(1,10)')
            query(b, 'BEGIN')
            with ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(query, b, 'EXECUTE wr(1)')
                try:
                    self.wait_for_lock(pending, b)
                finally:
                    query(a, 'COMMIT')
                assert pending.result(timeout=5) == 1
            query(b, 'COMMIT')
            assert query(a, 'SELECT * FROM cached') == ((1, 11),)
            query(b, 'BEGIN ISOLATION LEVEL REPEATABLE READ')
            query(b, f'INSERT INTO {self.schema}.cached VALUES(9,9)')
            query(b, f'SELECT * FROM {self.schema}.cached WHERE k=1')
            query(a, 'UPDATE cached SET v=v+1 WHERE k=1')
            try:
                wire_dml(b, 'EXECUTE wr(1)')
            except WireError as error:
                assert error.args[0] == 1213 and error.sqlstate == '40001', error
            else:
                raise AssertionError('cached current read bypassed transaction snapshot')
            assert query(b, f'SELECT COUNT(*) FROM {self.schema}.cached WHERE k=9') == ((0,),)
            # The index chosen by our plan must participate in invalidation.
            query(self.pg, f'ALTER TABLE {self.schema}.cached DROP CONSTRAINT cached_k_key')
            query(a, 'INSERT INTO cached VALUES(1,20)')
            assert query(b, 'EXECUTE wr(1)') == 2
            assert query(a, 'SELECT v FROM cached ORDER BY v') == ((13,), (21,))
            print('PASS cached parameter plan, snapshot isolation and index invalidation', flush=True)
        finally:
            query(b, 'ROLLBACK');a.close();b.close()

    def cancelled_wait(self):
        query(self.admin, 'TRUNCATE TABLE t')
        query(self.admin, 'INSERT INTO t VALUES(1,0),(2,0)')
        a, b = self.connect(), self.connect()
        try:
            for c in (a, b):
                query(c, 'USE ' + self.schema)
                query(c, 'BEGIN')
            query(a, 'DELETE FROM t WHERE id=1')
            query(b, 'UPDATE t SET v=10 WHERE id=2')
            with ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(query, b, 'DELETE FROM t WHERE id=1')
                self.wait_for_lock(pending, b)
                query(self.pg, 'SELECT pg_cancel_backend(%s)', (b.thread_id(),))
                try:
                    pending.result(timeout=5)
                except pymysql.MySQLError as error:
                    assert error.args[0] == 1317, error
                else:
                    raise AssertionError('cancelled lock wait succeeded')
            query(b, "SELECT set_config('lock_timeout','100ms',false)")
            try:
                query(b, 'DELETE FROM t WHERE id=1')
            except pymysql.MySQLError as error:
                assert error.args[0] == 1205, error
            else:
                raise AssertionError('lock timeout did not fire')
            query(b, 'COMMIT')
            query(a, 'ROLLBACK')
            assert query(self.admin, 'SELECT * FROM t ORDER BY id') == ((1, 0), (2, 10))
            print('PASS cancellation/timeout roll back only the waiting statement', flush=True)
        finally:
            query(a, 'ROLLBACK');query(b, 'ROLLBACK');a.close();b.close()

    def pg_cases(self, replace):
        # Row triggers must run once, after the current row has been chosen.
        query(self.pg, f'CREATE TABLE {self.schema}.audit(id int, v int)')
        query(self.pg, f'''CREATE FUNCTION {self.schema}.audit_write() RETURNS trigger
            LANGUAGE plpgsql AS $$ BEGIN INSERT INTO {self.schema}.audit VALUES(OLD.id,OLD.v);
            RETURN OLD; END $$''')
        query(self.pg, f'''CREATE TRIGGER audit AFTER DELETE OR UPDATE ON {self.schema}.t
            FOR EACH ROW EXECUTE FUNCTION {self.schema}.audit_write()''')
        self.race('triggers run once per affected row', replace,
                  'UPDATE t SET v=v+1 WHERE id=1', 1, ((1, 11),))
        assert query(self.pg, f'SELECT * FROM {self.schema}.audit ORDER BY v') == [(1, 0), (1, 10)]
        query(self.pg, f'DROP TRIGGER audit ON {self.schema}.t')
        query(self.pg, f'CREATE TABLE {self.schema}.own_command(id int PRIMARY KEY,v int)')
        query(self.pg, f'''CREATE FUNCTION {self.schema}.insert_before_statement() RETURNS trigger
            LANGUAGE plpgsql AS $$ BEGIN INSERT INTO {self.schema}.own_command VALUES(1,7);
            RETURN NULL; END $$''')
        query(self.pg, f'''CREATE TRIGGER insert_before BEFORE UPDATE ON {self.schema}.own_command
            FOR EACH STATEMENT EXECUTE FUNCTION {self.schema}.insert_before_statement()''')
        assert query(self.admin, 'UPDATE own_command SET v=9 WHERE id=1') == 0
        assert query(self.admin, 'SELECT * FROM own_command') == ((1, 7),)
        print('PASS refreshed snapshot preserves own-command visibility', flush=True)
        # Force a SeqScan candidate: current reads cannot depend on the planner's
        # cost choice for a tiny table. EXPLAIN itself must not acquire row locks.
        self.race('point write with index scans disabled', replace,
                  'UPDATE t SET v=v+1 WHERE id=1', 1, ((1, 11),),
                  setup=lambda c: query(c, "SELECT set_config('enable_indexscan','off',false),"
                                           "set_config('enable_bitmapscan','off',false)"))
        # Repeated writes grow the heap enough to make bitmap plans attractive.
        # Point-write semantics must survive that cost-driven plan change.
        def bitmap(c):
            query(c, "SELECT set_config('enable_indexscan','off',false),"
                     "set_config('enable_seqscan','off',false)")
            plan = query(c, 'EXPLAIN DELETE FROM t WHERE id=1')
            assert any('MySQL Current Read' in row[0] for row in plan), plan
        self.race('bitmap point delete/reinsert', replace, 'DELETE FROM t WHERE id=1',
                  1, ((1, 20),), after='INSERT INTO t VALUES(1,20)', setup=bitmap)
        self.race('bitmap point update rechecks predicate', replace,
                  'UPDATE t SET v=v+1 WHERE id=1 AND v=10', 1, ((1, 11),), setup=bitmap)
        self.race('bitmap point update rejects changed predicate', replace,
                  'UPDATE t SET v=v+1 WHERE id=1 AND v=0', 0, ((1, 10),), setup=bitmap)
        # A true transaction snapshot must continue to reject a concurrent change.
        a, b = self.connect(), self.connect()
        try:
            for c in (a, b): query(c, 'USE ' + self.schema)
            query(b, 'SET TRANSACTION ISOLATION LEVEL SERIALIZABLE')
            query(b, 'BEGIN')
            query(b, 'SELECT * FROM t WHERE id=1')
            query(a, 'UPDATE t SET v=v+1 WHERE id=1')
            try:
                query(b, 'UPDATE t SET v=v+1 WHERE id=1')
            except pymysql.MySQLError as error:
                assert error.args[0] == 1213, error
            else:
                raise AssertionError('SERIALIZABLE change was silently accepted')
            print('PASS SERIALIZABLE still reports a retryable conflict', flush=True)
        finally:
            query(b, 'ROLLBACK');a.close();b.close()

    def independent_keys_and_duplicates(self):
        query(self.admin, 'TRUNCATE TABLE t')
        query(self.admin, 'INSERT INTO t VALUES(1,0),(2,0)')
        a, b = self.connect(), self.connect()
        try:
            for c in (a, b): query(c, 'USE ' + self.schema)
            query(a, 'BEGIN');query(b, 'BEGIN')
            query(a, 'DELETE FROM t WHERE id=1')
            with ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(query, b, 'UPDATE t SET v=v+1 WHERE id=2')
                try:
                    assert pending.result(timeout=3) == 1
                finally:
                    query(a, 'ROLLBACK')
            query(b, 'COMMIT')
            try:
                query(b, 'INSERT INTO t VALUES(2,99)')
            except pymysql.MySQLError as error:
                assert error.args[0] == 1062, error
            else:
                raise AssertionError('real duplicate was accepted')
            assert query(self.admin, 'SELECT * FROM t ORDER BY id') == ((1, 0), (2, 1))
            print('PASS independent keys remain concurrent; real duplicates still fail', flush=True)
        finally:
            a.close();b.close()

    def close(self):
        query(self.admin, 'DROP SCHEMA ' + self.schema)
        self.admin.close()
        if self.pg:
            self.pg.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', action='store_true')
    args = parser.parse_args()
    suite = Suite(args.reference)
    try:
        suite.run()
    finally:
        suite.close()

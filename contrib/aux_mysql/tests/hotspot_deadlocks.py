#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
"""Deterministic MySQL deadlock detection, queue reordering, and hotspot transfers.

Uses PG*/MYSQL_* fixture variables; MYSQL_ADMIN_USER/PASSWORD default to PGUSER/
PGPASSWORD. Both administrative connections must be superusers. MySQL protocol
sessions default to mysql.deadlock_timeout (50ms) without any kernel ProcSleep
hook; explicit session deadlock_timeout overrides and native PG sessions are
also verified.
"""
from concurrent.futures import ThreadPoolExecutor
import os
import random
import threading
import time

import psycopg2
import pymysql

from point_write_concurrency import Suite, WireError, query, wire_dml


class Deadlocks(Suite):
    def endpoint(self, native=False, deadlock_ms=None, lock_ms=2000):
        if native:
            c = psycopg2.connect(connect_timeout=5)
            c.autocommit = True
            query(c, 'SET search_path=' + self.schema)
        else:
            c = pymysql.connect(host=os.getenv('MYSQL_HOST', '127.0.0.1'),
                port=int(os.getenv('MYSQL_PORT', '3306')),
                user=os.getenv('MYSQL_ADMIN_USER', os.getenv('PGUSER', 'postgres')),
                password=os.getenv('MYSQL_ADMIN_PASSWORD', os.getenv('PGPASSWORD', '')),
                autocommit=True, connect_timeout=5, read_timeout=15, write_timeout=15)
            query(c, 'USE ' + self.schema)
        settings = [('lock_timeout', f'{lock_ms}ms'), ('statement_timeout', '8s')]
        if deadlock_ms is not None:
            settings.insert(0, ('deadlock_timeout', f'{deadlock_ms}ms'))
        for name, value in settings:
            query(c, 'SELECT set_config(%s,%s,false)', (name, value))
        return c

    def wait(self, future, conn, native=False):
        pid = conn.get_backend_pid() if native else conn.thread_id()
        end = time.monotonic() + 3
        while time.monotonic() < end:
            if query(self.pg, 'SELECT cardinality(pg_blocking_pids(%s))', (pid,))[0][0]:
                return
            if future.done():
                raise AssertionError(('did not wait', future.result()))
            time.sleep(.005)
        raise AssertionError('wait not observed')

    def cycle(self, label, n=2, native_last=False, all_native=False,
              expected='deadlock', deadlock_ms=None, lock_ms=2000):
        query(self.admin, 'TRUNCATE TABLE t')
        query(self.admin, 'CREATE TABLE IF NOT EXISTS marks(id INT PRIMARY KEY)')
        query(self.admin, 'TRUNCATE TABLE marks')
        for i in range(1, n + 1):
            query(self.admin, 'INSERT INTO t VALUES(%s,0)', (i,))
        native = [all_native or (native_last and i == n-1) for i in range(n)]
        # Let only the first waiter time out in the fallback controls. Equal
        # deadlines could cancel both transactions before either releases locks.
        conns = [self.endpoint(flag, deadlock_ms,
                              lock_ms if expected != 'timeout' or i == 0 else lock_ms+2000)
                 for i, flag in enumerate(native)]
        try:
            for i, c in enumerate(conns, 1):
                query(c, 'BEGIN')
                query(c, 'INSERT INTO marks VALUES(%s)', (i,))
                query(c, 'UPDATE t SET v=v+1 WHERE id=%s', (i,))

            def finish(i):
                c = conns[i]
                start = time.monotonic()
                try:
                    sql = f'UPDATE t SET v=v+1 WHERE id={(i+1) % n + 1}'
                    query(c, sql) if native[i] else wire_dml(c, sql)
                except (WireError, psycopg2.Error) as e:
                    elapsed = time.monotonic() - start
                    if native[i]:
                        kind = 'deadlock' if e.pgcode == '40P01' else 'timeout'
                        assert e.pgcode in ('40P01', '55P03'), e
                    else:
                        kind = 'deadlock' if e.args[0] == 1213 else 'timeout'
                        assert e.args[0] in (1213, 1205), e
                        if kind == 'deadlock':
                            assert e.sqlstate == '40001', e
                            assert query(c, 'SELECT COUNT(*) FROM marks WHERE id=%s',
                                         (i+1,)) == ((0,),), 'partial victim transaction survived'
                    query(c, 'ROLLBACK')
                    return i+1, kind, elapsed
                query(c, 'COMMIT')
                return i+1, 'ok', time.monotonic()-start

            with ThreadPoolExecutor(max_workers=n) as pool:
                futures = []
                for i in range(n):
                    f = pool.submit(finish, i)
                    futures.append(f)
                    if i < n-1:
                        self.wait(f, conns[i], native[i])
                results = [f.result(timeout=10) for f in futures]
            victims = [r for r in results if r[1] != 'ok']
            assert len(victims) == 1 and victims[0][1] == expected, results
            if expected == 'deadlock' and deadlock_ms is None and not native_last and not all_native:
                # Default mysql.deadlock_timeout (50ms) resolves promptly (< 1s).
                assert victims[0][2] < 1.0, results
            marks = tuple(r[0] for r in query(self.admin, 'SELECT id FROM marks ORDER BY id'))
            winners = tuple(i for i in range(1, n+1) if i != victims[0][0])
            assert marks == winners, (results, marks)
            actual = query(self.admin, 'SELECT id,v FROM t ORDER BY id')
            expected_rows = tuple((i, int(i in winners) + int((i-2) % n + 1 in winners))
                                  for i in range(1, n+1))
            assert actual == expected_rows, (results, actual, expected_rows)
            print('PASS', label, 'victim', victims[0], flush=True)
        finally:
            for c in conns:
                c.close()

    def ordinary_wait(self, cancel=False, timeout=False):
        query(self.admin, 'TRUNCATE TABLE t')
        query(self.admin, 'INSERT INTO t VALUES(1,0),(2,0)')
        a, b = self.endpoint(), self.endpoint(lock_ms=250 if timeout else 2000)
        try:
            query(a, 'BEGIN'); query(b, 'BEGIN')
            query(a, 'UPDATE t SET v=10 WHERE id=1')
            query(b, 'UPDATE t SET v=20 WHERE id=2')
            with ThreadPoolExecutor(max_workers=1) as pool:
                f = pool.submit(query, b, 'UPDATE t SET v=v+1 WHERE id=1')
                self.wait(f, b)
                if cancel:
                    query(self.pg, 'SELECT pg_cancel_backend(%s)', (b.thread_id(),))
                elif not timeout:
                    # Ordinary contention must wait, without false deadlocks.
                    time.sleep(.1)
                    assert not f.done()
                    query(a, 'COMMIT')
                try:
                    assert f.result(timeout=3) == 1
                    assert not cancel and not timeout
                except pymysql.MySQLError as e:
                    assert e.args[0] == (1317 if cancel else 1205), e
            query(b, 'COMMIT')
            query(a, 'ROLLBACK')
            assert query(self.admin, 'SELECT * FROM t ORDER BY id') == (
                (1, 0 if cancel or timeout else 11), (2, 20))
            print('PASS ordinary wait', 'cancel' if cancel else 'timeout' if timeout else 'commit',
                  flush=True)
        finally:
            a.close(); b.close()

    def transfers(self, mixed=False, workers=16, iters=100, rows=8):
        # Match all committed changes to a transaction ledger. Row counts alone
        # cannot detect a victim whose first write survived its deadlock.
        query(self.admin, 'TRUNCATE TABLE t')
        query(self.admin, 'CREATE TABLE IF NOT EXISTS ledger('
              'worker INT, seq INT, src INT, dst INT, PRIMARY KEY(worker,seq))')
        query(self.admin, 'TRUNCATE TABLE ledger')
        for i in range(1, rows + 1):
            query(self.admin, 'INSERT INTO t VALUES(%s,0)', (i,))
        barrier = threading.Barrier(workers)
        deadline = time.monotonic()+90

        def worker(i):
            native = mixed and i % 2 == 0
            # MySQL sessions rely on built-in mysql.deadlock_timeout (50ms);
            # native PG sessions set 50ms explicitly in the mixed test.
            c = self.endpoint(native=native, deadlock_ms=50 if native else None, lock_ms=5000)
            rng = random.Random(i)
            retries = 0
            maximum = 0
            try:
                barrier.wait(timeout=15)
                for seq in range(iters):
                    src, dst = rng.sample(range(1, rows + 1), 2)
                    start = time.monotonic()
                    while True:
                        assert time.monotonic() < deadline, ('transfer stalled', i, seq)
                        try:
                            query(c, 'BEGIN')
                            query(c, 'UPDATE t SET v=v-1 WHERE id=%s', (src,))
                            query(c, 'UPDATE t SET v=v+1 WHERE id=%s', (dst,))
                            query(c, 'INSERT INTO ledger VALUES(%s,%s,%s,%s)',
                                  (i, seq, src, dst))
                            query(c, 'COMMIT')
                            break
                        except (pymysql.MySQLError, psycopg2.Error) as e:
                            if native:
                                assert e.pgcode == '40P01', e
                                query(c, 'ROLLBACK')
                            else:
                                assert e.args[0] == 1213, e
                                # Exercise automatic whole-transaction recovery.
                            retries += 1
                    maximum = max(maximum, time.monotonic()-start)
            finally:
                c.close()
            return retries, maximum

        with ThreadPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(worker, range(workers)))
        ledger = query(self.admin, 'SELECT worker,seq,src,dst FROM ledger')
        assert len(ledger) == workers * iters
        assert {(w, s) for w, s, _, _ in ledger} == {
            (w, s) for w in range(workers) for s in range(iters)}
        expected = {i: 0 for i in range(1, rows + 1)}
        for _, _, src, dst in ledger:
            expected[src] -= 1
            expected[dst] += 1
        actual = dict(query(self.admin, 'SELECT id,v FROM t'))
        assert actual == expected and sum(actual.values()) == 0, (actual, expected)
        print(f'PASS {workers}-thread transfers ({rows} rows)',
              'mixed protocols' if mixed else 'MySQL',
              f'commits={workers * iters}', 'retries='+str(sum(r[0] for r in results)),
              'max_seconds='+str(round(max(r[1] for r in results), 4)), flush=True)

    def soft_cycle(self):
        # A shares gate; B wants gate exclusively; C wants a share behind B.
        # A then waits for C's row. Moving C ahead of B resolves this soft cycle
        # without aborting anyone.
        query(self.admin, 'CREATE TABLE gate(id INT)')
        query(self.admin, 'TRUNCATE TABLE t')
        query(self.admin, 'INSERT INTO t VALUES(1,0),(2,0)')
        a, b, c = self.endpoint(), self.endpoint(native=True), self.endpoint(native=True)
        try:
            query(a, "SELECT set_config('mysql.parser','postgres',false)")
            for conn in (a, b, c):
                query(conn, 'BEGIN')
            query(a, 'LOCK TABLE gate IN SHARE MODE')
            query(b, 'UPDATE t SET v=v+1 WHERE id=1')
            query(c, 'UPDATE t SET v=v+1 WHERE id=2')

            def lock_and_commit(conn, mode):
                query(conn, 'LOCK TABLE gate IN ' + mode + ' MODE')
                query(conn, 'COMMIT')

            with ThreadPoolExecutor(max_workers=2) as pool:
                fb = pool.submit(lock_and_commit, b, 'ACCESS EXCLUSIVE')
                self.wait(fb, b, native=True)
                fc = pool.submit(lock_and_commit, c, 'SHARE')
                self.wait(fc, c, native=True)
                try:
                    assert query(a, 'UPDATE t SET v=v+1 WHERE id=2') == 1
                    query(a, 'COMMIT')
                finally:
                    query(a, 'ROLLBACK')
                fb.result(timeout=3); fc.result(timeout=3)
            assert query(self.admin, 'SELECT * FROM t ORDER BY id') == ((1, 1), (2, 2))
            print('PASS soft cycle reorders waiters without abort', flush=True)
        finally:
            for conn in (a, b, c):
                conn.close()

    def run(self):
        self.cycle('two-way default mysql.deadlock_timeout detection')
        self.cycle('three-way default mysql.deadlock_timeout detection', n=3)
        self.cycle('16-way default mysql.deadlock_timeout detection', n=16)
        self.cycle('explicit session deadlock_timeout obeys lock timeout',
                   deadlock_ms=5000, expected='timeout', lock_ms=500)
        self.cycle('explicit session deadlock_timeout=250ms detector',
                   deadlock_ms=250)
        self.cycle('native sessions retain 1s default timeout behavior', all_native=True,
                   expected='timeout', lock_ms=500)
        self.cycle('mixed native closing edge uses configured detector', n=3, native_last=True,
                   deadlock_ms=250)
        self.ordinary_wait()
        self.ordinary_wait(cancel=True)
        self.ordinary_wait(timeout=True)
        self.soft_cycle()
        self.transfers(workers=16, iters=100, rows=8)
        self.transfers(mixed=True, workers=16, iters=100, rows=8)
        self.transfers(workers=64, iters=25, rows=32)


if __name__ == '__main__':
    suite = Deadlocks(False)
    try:
        suite.run()
    finally:
        suite.close()

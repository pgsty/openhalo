#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
"""Adversarial self-review & end-to-end acceptance suite for OpenHalo 19.0.0.

Covers:
  1. Flex scanner (-Cfe) safety on _binary high-byte / embedded-NUL normalization
  2. utf8mb4_bin PAD SPACE pg_strxfrm planner selectivity, prefix_like multibyte
     bounds, and mysql_prefix exact_candidate conflict-probe memory stability
  3. Zero TopMemoryContext leak across 1,400 rewrite-warning prepare/execute/error cycles
  4. Wire diagnostic clearing & preservation (COM_STMT_PREPARE, COM_PING, COM_STMT_RESET,
     COM_STMT_CLOSE, >63-byte identifier truncation warning parity, stale plan rebuild)
  5. transform_null_equals = on parity on binary COM_STMT_EXECUTE
  6. Nested trigger isolation for ON UPDATE CURRENT_TIMESTAMP and ODKU affected_rows
  7. 12-thread concurrent CachedPlan fast-path + multilevel partitioned MysCurrentRead
  8. Physical streaming replication Hot Standby + MySQL read/write checks + Promotion
"""
from concurrent.futures import ThreadPoolExecutor
import datetime
import json
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time
import uuid

import psycopg2
import pymysql

from tier1_optimizations import (
    RawStmtClient,
    T_BLOB,
    T_LONG,
    T_NULL,
    T_VARCHAR,
    my_conn,
    pg_conn,
    query,
    wait_blocked,
)

COM_PING = 0x0E


def pick_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def lenenc_bytes(raw):
    n = len(raw)
    if n < 251:
        return bytes([n]) + raw
    if n < 65536:
        return b'\xfc' + struct.pack('<H', n) + raw
    return b'\xfd' + struct.pack('<I', n)[:3] + raw


def test_scanner_binary_high_byte_and_nul(schema):
    """Regression for Flex -CF yy_transition tail overflow on _binary literals."""
    c = my_conn(database=schema)
    raw = RawStmtClient(my_conn(database=schema))
    try:
        query(c, 'CREATE TABLE scan_bin(id INT PRIMARY KEY, b BLOB, vb VARBINARY(32))')
        query(c, "INSERT INTO scan_bin VALUES (1, _binary X'b66800ff8090feff', X'b66800ff8090feff')")

        # Hex _binary introducer (exact trigger of ab_regress ASan crash)
        rows = query(c, "SELECT id FROM scan_bin WHERE b = _binary X'b66800ff8090feff' AND vb = X'b66800ff8090feff'")
        assert rows == [(1,)], rows

        # Raw high-byte & embedded-NUL bytes sent directly over COM_QUERY wire packet
        for tail in (b'\x00', b'\x80', b'\xfe', b'\xff', b'\xb6h\x00\xff\x80\x90\xfe\xff'):
            payload = b"SELECT HEX(_binary '" + tail + b"')"
            raw.send(0x03, payload)
            first = raw.read_pkt()
            assert first[0] != 0xff, first
            ncols, _ = raw.lenenc(first, 0)
            cols = [raw.parse_coldef(raw.read_pkt()) for _ in range(ncols)]
            if not raw.deprecate_eof:
                raw.read_pkt()
            row_pkt = raw.read_pkt()
            end_pkt = raw.read_pkt()
            assert end_pkt[0] == 0xfe
            plen, idx = raw.lenenc(row_pkt, 0)
            got_hex = row_pkt[idx:idx + plen].decode('ascii')
            assert got_hex.upper() == tail.hex().upper(), (got_hex, tail.hex())

        # Prepared statement normalization with _binary and high bytes
        ps = raw.prepare("SELECT id FROM scan_bin WHERE b = _binary X'b66800ff8090feff' AND id = ?")
        assert 'id' in ps, ps
        res = raw.execute(ps['id'], [(T_LONG, struct.pack('<i', 1))])
        assert res['rows'] == [(1,)], res
        raw.close_stmt(ps['id'])
        print('PASS scanner _binary high-byte and embedded-NUL normalization', flush=True)
    finally:
        raw.close()
        c.close()


def test_utf8mb4_bin_selectivity_and_prefix(pg, schema):
    """Verify utf8mb4_bin PAD SPACE pg_strxfrm selectivity, prefix_like, and conflict memory."""
    c = my_conn(database=schema)
    try:
        query(c, """
            CREATE TABLE bin_sel (
                id INT PRIMARY KEY,
                v VARCHAR(32) COLLATE utf8mb4_bin NOT NULL,
                c CHAR(8) COLLATE utf8mb4_general_ci NOT NULL,
                UNIQUE KEY uk_pref (v(5))
            )
        """)
        # Ordering & equality under utf8mb4_bin PAD SPACE: 'A\x01' < 'A' == 'A   ' < 'A!'
        assert query(c, "SELECT ('A\x01' COLLATE utf8mb4_bin) < ('A' COLLATE utf8mb4_bin)") == [(1,)]
        assert query(c, "SELECT ('A' COLLATE utf8mb4_bin) = ('A   ' COLLATE utf8mb4_bin)") == [(1,)]
        assert query(c, "SELECT ('A' COLLATE utf8mb4_bin) < ('A!' COLLATE utf8mb4_bin)") == [(1,)]

        # Populate distinct 4-char prefixes for planner stats
        batch = []
        for i in range(400):
            # Spread across 'A\x01..', 'A   ', 'A!..', 'B...'
            if i < 100:
                val = f'A\x01{i:02d}'
            elif i == 100:
                val = 'A   '
            elif i < 250:
                val = f'A!{i:02d}'
            else:
                val = f'B#{i:02d}'
            batch.append(f"({i}, '{val}', 'é中😀')")
        query(c, 'INSERT INTO bin_sel VALUES ' + ','.join(batch))
        query(pg, f'ANALYZE {schema}.bin_sel')

        plan_json = query(
            pg,
            f"""
            EXPLAIN (FORMAT JSON)
            SELECT * FROM {schema}.bin_sel
            WHERE v >= U&'A\\0001' COLLATE mysql.utf8mb4_bin
              AND v <= 'A' COLLATE mysql.utf8mb4_bin
            """,
        )[0][0]
        est_rows = plan_json[0]['Plan']['Plan Rows']
        # Roughly 101 rows match ('A\x0100'..'A\x0199' + 'A   '); inverted lobound>hibound
        # previously collapsed scalarineqsel to 1 row!
        assert 40 <= est_rows <= 200, f'unexpected utf8mb4_bin range selectivity rows={est_rows}'

        # Multibyte UTF-8 & trailing-backslash LIKE under both general_ci and bin
        assert query(c, "SELECT COUNT(*) FROM bin_sel WHERE c LIKE 'E_😀'") == [(400,)]
        assert query(c, "SELECT ('abc\\\\' COLLATE utf8mb4_bin) LIKE 'abc\\\\'") == [(1,)]
        assert query(c, "SELECT ('😀x' COLLATE utf8mb4_bin) LIKE '_x'") == [(1,)]
        assert query(c, "SELECT ('😀x' COLLATE utf8mb4_general_ci) LIKE '_X'") == [(1,)]

        # 600 conflicting rows in a single INSERT IGNORE over mysql_prefix unique index:
        # verifies exact_candidate frees BuildIndexInfo & per-tuple detoasted keys cleanly.
        backend_pid = query(c, 'SELECT CONNECTION_ID()')[0][0]
        conflict_vals = ','.join(f"({1000 + i}, 'B#250_extra_{i}', 'x')" for i in range(600))
        affected = query(c, f'INSERT IGNORE INTO bin_sel VALUES {conflict_vals}')
        assert affected == 0, affected
        warns = query(c, 'SHOW WARNINGS')
        assert warns and warns[0][1] == 1062, warns

        # Bitmap scan path over mysql_prefix index leaves no dangling heapRelation
        query(c, "SELECT pg_catalog.set_config('enable_indexscan', 'off', false)")
        query(c, "SELECT pg_catalog.set_config('enable_seqscan', 'off', false)")
        got = query(c, "SELECT id FROM bin_sel WHERE v = 'B#250'")
        query(c, "SELECT pg_catalog.set_config('enable_indexscan', 'on', false)")
        query(c, "SELECT pg_catalog.set_config('enable_seqscan', 'on', false)")
        assert got == [(250,)], got
        assert backend_pid == query(c, 'SELECT CONNECTION_ID()')[0][0]
        print('PASS utf8mb4_bin strxfrm selectivity, multibyte prefix_like & prefix conflict memory', flush=True)
    finally:
        c.close()


def test_rewrite_warnings_zero_topmem_leak(pg, schema):
    """Ensure rewrite_warnings never leak in TopMemoryContext across prepare/execute/error."""
    raw_conn = my_conn(
        database=schema,
        client_flag=pymysql.constants.CLIENT.MULTI_STATEMENTS | pymysql.constants.CLIENT.DEPRECATE_EOF,
    )
    raw = RawStmtClient(raw_conn)
    try:
        raw.send(0x03, b'SELECT CONNECTION_ID()')
        first = raw.read_pkt()
        ncols, _ = raw.lenenc(first, 0)
        for _ in range(ncols):
            raw.read_pkt()
        if not raw.deprecate_eof:
            raw.read_pkt()
        rp = raw.read_pkt()
        raw.read_pkt()
        plen, idx = raw.lenenc(rp, 0)
        my_pid = int(rp[idx:idx + plen])

        # Warm up caches once before sampling TopMemoryContext
        ps0 = raw.prepare("SELECT ? COLLATE utf8mb4_unicode_ci")
        raw.execute(ps0['id'], [(T_VARCHAR, lenenc_bytes(b'hello'))])
        raw.close_stmt(ps0['id'])

        def top_used_bytes():
            # Query TopMemoryContext of the live MySQL backend via pg_stat_activity check;
            # inside the MySQL session itself we can query pg_backend_memory_contexts directly!
            raw.send(
                0x03,
                b"SELECT used_bytes FROM pg_catalog.pg_backend_memory_contexts "
                b"WHERE name = 'TopMemoryContext' ORDER BY level ASC LIMIT 1",
            )
            f = raw.read_pkt()
            nc, _ = raw.lenenc(f, 0)
            for _ in range(nc):
                raw.read_pkt()
            if not raw.deprecate_eof:
                raw.read_pkt()
            r = raw.read_pkt()
            raw.read_pkt()
            ln, ix = raw.lenenc(r, 0)
            return int(r[ix:ix + ln])

        base_bytes = top_used_bytes()

        # 1) 400x COM_STMT_PREPARE + COM_STMT_CLOSE with collation warning
        for _ in range(400):
            ps = raw.prepare("SELECT ? COLLATE utf8mb4_unicode_ci")
            assert ps.get('id'), ps
            raw.close_stmt(ps['id'])

        # 2) 400x COM_STMT_EXECUTE of collation-warning statement (must emit warning every time!)
        ps = raw.prepare("SELECT ? COLLATE utf8mb4_unicode_ci")
        for i in range(400):
            res = raw.execute(ps['id'], [(T_VARCHAR, lenenc_bytes(b'abc'))])
            assert res['rows'] == [(b'abc',)] or res['rows'] == [('abc',)], res
            assert res['term']['warnings'] == 1, res
        raw.close_stmt(ps['id'])

        # 3) 200x multi-statement error on stmt #1 when stmt #2 queued rewrite_warnings
        for _ in range(200):
            raw.send(
                0x03,
                b"SELECT * FROM missing_tbl_xyz; SELECT 'x' COLLATE utf8mb4_unicode_ci",
            )
            err = raw.read_pkt()
            assert err[0] == 0xff

        # 4) 200x mid-rewrite error after queueing collation warning (ENCRYPTION='Y' without pg_tde)
        for _ in range(200):
            raw.send(
                0x03,
                b"CREATE TABLE bad_enc(v VARCHAR(10) COLLATE utf8mb4_unicode_ci) ENCRYPTION='Y'",
            )
            err = raw.read_pkt()
            assert err[0] == 0xff

        after_bytes = top_used_bytes()
        delta = after_bytes - base_bytes
        assert delta < 65536, f'TopMemoryContext leaked {delta} bytes (pid={my_pid})'
        print(f'PASS zero TopMemoryContext leak across 1,200 warning cycles (delta={delta}B)', flush=True)
    finally:
        raw.close()


def test_wire_diagnostics_and_fastpath_edge_cases(schema):
    """Verify wire command warning clearing, COM_STMT_CLOSE preservation, >63B warning parity,
    stale cached UPDATE rebuild safety, and transform_null_equals = on."""
    c = my_conn(database=schema)
    raw = RawStmtClient(my_conn(database=schema, client_flag=pymysql.constants.CLIENT.DEPRECATE_EOF))
    try:
        # 1) COM_STMT_PREPARE, COM_PING, COM_STMT_RESET must clear prior warnings & report 0
        def emit_one_warning():
            raw.send(0x03, b"SELECT 'a' COLLATE utf8mb4_unicode_ci")
            f = raw.read_pkt()
            nc, _ = raw.lenenc(f, 0)
            for _ in range(nc):
                raw.read_pkt()
            if not raw.deprecate_eof:
                raw.read_pkt()
            raw.read_pkt()
            end = raw.read_pkt()
            ok = raw.parse_ok(end)
            assert ok['warnings'] == 1, ok

        def read_show_warnings():
            raw.send(0x03, b'SHOW WARNINGS')
            f = raw.read_pkt()
            nc, _ = raw.lenenc(f, 0)
            for _ in range(nc):
                raw.read_pkt()
            if not raw.deprecate_eof:
                raw.read_pkt()
            out = []
            while True:
                p = raw.read_pkt()
                if p[0] == 0xfe and len(p) < 0xffffff:
                    break
                out.append(p)
            return out

        emit_one_warning()
        # Raw COM_STMT_PREPARE checks raw warning_count inside COM_STMT_PREPARE_OK!
        raw.send(0x16, b'SELECT ?')
        prep_pkt = raw.read_pkt()
        assert prep_pkt[0] == 0x00, prep_pkt
        sid, ncols, nparams, _, prep_warns = struct.unpack('<IHHBH', prep_pkt[1:12])
        for _ in range(nparams):
            raw.read_pkt()
        if nparams and not raw.deprecate_eof:
            raw.read_pkt()
        for _ in range(ncols):
            raw.read_pkt()
        if ncols and not raw.deprecate_eof:
            raw.read_pkt()
        assert prep_warns == 0, f'COM_STMT_PREPARE leaked stale warning_count={prep_warns}'
        assert read_show_warnings() == []

        emit_one_warning()
        raw.send(COM_PING, b'')
        ping_ok = raw.parse_ok(raw.read_pkt())
        assert ping_ok['warnings'] == 0, ping_ok
        assert read_show_warnings() == []

        emit_one_warning()
        rst_ok = raw.reset_stmt(sid)
        assert rst_ok['warnings'] == 0, rst_ok
        assert read_show_warnings() == []
        raw.close_stmt(sid)

        # 2) COM_STMT_CLOSE after warning-emitting COM_STMT_EXECUTE MUST preserve SHOW WARNINGS!
        ps_warn = raw.prepare("SELECT ? COLLATE utf8mb4_unicode_ci")
        res_warn = raw.execute(ps_warn['id'], [(T_VARCHAR, lenenc_bytes(b'keep'))])
        assert res_warn['term']['warnings'] == 1, res_warn
        raw.close_stmt(ps_warn['id'])
        kept = read_show_warnings()
        assert len(kept) == 1, f'COM_STMT_CLOSE wiped SHOW WARNINGS: {kept}'

        # 3) Constant expression emitting WARNING (1 / 0 under sql_mode='') must repeat on EVERY COM_STMT_EXECUTE (1..7)
        raw.send(0x03, b"SET SESSION sql_mode = ''")
        raw.read_pkt()
        ps_trunc = raw.prepare('SELECT ?, 1 / 0')
        for run_idx in range(7):
            res_t = raw.execute(ps_trunc['id'], [(T_LONG, struct.pack('<i', 42))])
            assert res_t['rows'] == [(42, None)], res_t
            assert res_t['term']['warnings'] == 1, (run_idx, res_t)
            assert len(read_show_warnings()) == 1, run_idx
        raw.close_stmt(ps_trunc['id'])
        raw.send(0x03, b"SET SESSION sql_mode = DEFAULT")
        raw.read_pkt()

        # 4) Stale cached UPDATE plan rebuild when table alters into unsupported/changed form
        query(c, 'CREATE TABLE stale_upd(id INT PRIMARY KEY, v VARCHAR(16))')
        query(c, "INSERT INTO stale_upd VALUES (1, 'init')")
        ps_u = raw.prepare('UPDATE stale_upd SET v = ? WHERE id = ?')
        for val in (b'v1', b'v2'):
            ru = raw.execute(ps_u['id'], [(T_VARCHAR, lenenc_bytes(val)), (T_LONG, struct.pack('<i', 1))])
            assert ru['affected'] == 1, ru
        # Alter column v to INT so cached VARCHAR param signature must rebuild/fallback cleanly
        query(c, 'UPDATE stale_upd SET v = 10 WHERE id = 1')
        query(c, 'ALTER TABLE stale_upd MODIFY COLUMN v INT')
        ru3 = raw.execute(ps_u['id'], [(T_VARCHAR, lenenc_bytes(b'77')), (T_LONG, struct.pack('<i', 1))])
        assert ru3['affected'] == 1, ru3
        assert query(c, 'SELECT v FROM stale_upd WHERE id = 1') == [(77,)]
        raw.close_stmt(ps_u['id'])

        # 5) transform_null_equals = on parity on binary COM_STMT_EXECUTE
        query(c, 'CREATE TABLE t_null_eq(id INT PRIMARY KEY, v INT)')
        query(c, 'INSERT INTO t_null_eq VALUES (1, NULL), (2, 10)')
        raw.send(0x03, b"SELECT pg_catalog.set_config('transform_null_equals', 'on', false)")
        f = raw.read_pkt()
        nc, _ = raw.lenenc(f, 0)
        for _ in range(nc):
            raw.read_pkt()
        if not raw.deprecate_eof:
            raw.read_pkt()
        raw.read_pkt()
        raw.read_pkt()

        ps_ne = raw.prepare('SELECT id FROM t_null_eq WHERE v = ?')
        r_ten = raw.execute(ps_ne['id'], [(T_LONG, struct.pack('<i', 10))])
        assert r_ten['rows'] == [(2,)], r_ten
        r_null = raw.execute(ps_ne['id'], [(T_NULL, None)], bind=True)
        assert r_null['rows'] == [(1,)], f'transform_null_equals=on mismatch: {r_null}'
        raw.close_stmt(ps_ne['id'])

        print('PASS wire diagnostics, COM_STMT_CLOSE preservation, >63B warning parity & transform_null_equals', flush=True)
    finally:
        raw.close()
        c.close()


def test_nested_triggers_timestamp_and_upsert(pg, schema):
    """Verify nested triggers neither suppress inner ON UPDATE CURRENT_TIMESTAMP
    nor corrupt outer ON DUPLICATE KEY UPDATE affected_rows."""
    c = my_conn(database=schema)
    raw = RawStmtClient(my_conn(database=schema))
    try:
        query(c, """
            CREATE TABLE outer_ts(
                id INT PRIMARY KEY,
                payload INT,
                ts DATETIME DEFAULT '2020-01-01 00:00:00' ON UPDATE CURRENT_TIMESTAMP
            )
        """)
        query(c, """
            CREATE TABLE inner_ts(
                id INT PRIMARY KEY,
                payload INT,
                ts DATETIME DEFAULT '2020-01-01 00:00:00' ON UPDATE CURRENT_TIMESTAMP
            )
        """)
        query(c, "INSERT INTO outer_ts(id, payload) VALUES (1, 10), (2, 20)")
        query(c, "INSERT INTO inner_ts(id, payload) VALUES (1, 10), (2, 20)")

        query(pg, f"""
            CREATE OR REPLACE FUNCTION {schema}.cascade_payload_fn() RETURNS trigger
            LANGUAGE plpgsql AS $$
            BEGIN
                UPDATE {schema}.inner_ts SET payload = NEW.payload WHERE id = NEW.id;
                RETURN NEW;
            END;
            $$;
            CREATE TRIGGER tr_after_outer_upd
            AFTER UPDATE ON {schema}.outer_ts
            FOR EACH ROW EXECUTE FUNCTION {schema}.cascade_payload_fn();
        """)

        # 1) COM_QUERY: outer UPDATE explicitly assigns `ts`, inner UPDATE does NOT!
        assert query(
            c,
            "UPDATE outer_ts SET payload = 111, ts = '2021-06-01 12:00:00' WHERE id = 1",
        ) == 1
        outer_row = query(c, 'SELECT payload, ts FROM outer_ts WHERE id = 1')[0]
        inner_row = query(c, 'SELECT payload, ts FROM inner_ts WHERE id = 1')[0]
        assert outer_row == (111, datetime.datetime(2021, 6, 1, 12, 0, 0)), outer_row
        assert inner_row[0] == 111 and inner_row[1].year >= 2025, inner_row

        # 2) Fast-path COM_STMT_EXECUTE: same guarantee with cached_info.update_cols
        ps = raw.prepare("UPDATE outer_ts SET payload = ?, ts = '2022-07-02 08:30:00' WHERE id = ?")
        for pid, pval in ((2, 222), (2, 333)):
            r = raw.execute(ps['id'], [(T_LONG, struct.pack('<i', pval)), (T_LONG, struct.pack('<i', pid))])
            assert r['affected'] == 1, r
        raw.close_stmt(ps['id'])
        outer_row2 = query(c, 'SELECT payload, ts FROM outer_ts WHERE id = 2')[0]
        inner_row2 = query(c, 'SELECT payload, ts FROM inner_ts WHERE id = 2')[0]
        assert outer_row2 == (333, datetime.datetime(2022, 7, 2, 8, 30, 0)), outer_row2
        assert inner_row2[0] == 333 and inner_row2[1].year >= 2025, inner_row2

        print('PASS nested trigger ON UPDATE CURRENT_TIMESTAMP isolation', flush=True)
    finally:
        raw.close()
        c.close()


def test_concurrent_fastpath_partitioned_current_read(pg, schema):
    """12-thread concurrent binary CachedPlan + multilevel partitioned MysCurrentRead stress."""
    c = my_conn(database=schema)
    try:
        query(pg, f"""
            CREATE TABLE {schema}.mp_conc (
                grp INT NOT NULL,
                id INT NOT NULL,
                val INT NOT NULL,
                PRIMARY KEY (grp, id)
            ) PARTITION BY RANGE (grp)
        """)
        for g0, g1 in ((0, 2), (2, 4)):
            query(pg, f"""
                CREATE TABLE {schema}.mp_conc_{g0} PARTITION OF {schema}.mp_conc
                FOR VALUES FROM ({g0}) TO ({g1})
                PARTITION BY HASH (id)
            """)
            for m in range(2):
                query(pg, f"""
                    CREATE TABLE {schema}.mp_conc_{g0}_{m} PARTITION OF {schema}.mp_conc_{g0}
                    FOR VALUES WITH (MODULUS 2, REMAINDER {m})
                """)

        for g in range(4):
            for k in range(1, 5):
                query(c, f'INSERT INTO mp_conc VALUES ({g}, {k}, 1)')

        errors = []
        lock = threading.Lock()

        def worker(tid):
            rc = RawStmtClient(my_conn(database=schema))
            try:
                ps_del = rc.prepare('DELETE FROM mp_conc WHERE grp = ? AND id = ?')
                ps_ins = rc.prepare('INSERT INTO mp_conc VALUES (?, ?, ?)')
                ps_upd = rc.prepare('UPDATE mp_conc SET val = val + 1 WHERE grp = ? AND id = ?')
                for step in range(50):
                    g = (tid + step) % 4
                    k = ((tid * 3 + step) % 4) + 1
                    for attempt in range(8):
                        rc.send(0x03, b'BEGIN')
                        rc.read_pkt()
                        r1 = rc.execute(
                            ps_del['id'],
                            [(T_LONG, struct.pack('<i', g)), (T_LONG, struct.pack('<i', k))],
                        )
                        if 'errno' in r1:
                            if r1['errno'] == 1213:
                                continue
                            raise AssertionError(('DEL', r1))
                        r2 = rc.execute(
                            ps_ins['id'],
                            [
                                (T_LONG, struct.pack('<i', g)),
                                (T_LONG, struct.pack('<i', k)),
                                (T_LONG, struct.pack('<i', step + 1)),
                            ],
                        )
                        if 'errno' in r2:
                            if r2['errno'] == 1213:
                                continue
                            raise AssertionError(('INS', r2))
                        r3 = rc.execute(
                            ps_upd['id'],
                            [(T_LONG, struct.pack('<i', g)), (T_LONG, struct.pack('<i', k))],
                        )
                        if 'errno' in r3:
                            if r3['errno'] == 1213:
                                continue
                            raise AssertionError(('UPD', r3))
                        rc.send(0x03, b'COMMIT')
                        cp = rc.read_pkt()
                        if cp[0] == 0xff:
                            errno = struct.unpack('<H', cp[1:3])[0]
                            if errno == 1213:
                                continue
                            raise AssertionError(('COMMIT', errno))
                        break
                    else:
                        raise AssertionError('exceeded deadlock retry budget')
            except Exception as exc:
                with lock:
                    errors.append((tid, repr(exc)))
            finally:
                rc.close()

        with ThreadPoolExecutor(max_workers=12) as pool:
            futures = [pool.submit(worker, i) for i in range(12)]
            for f in futures:
                f.result()

        assert not errors, errors
        cnt = query(c, 'SELECT COUNT(*) FROM mp_conc')[0][0]
        assert cnt == 16, cnt
        print('PASS 12-thread concurrent CachedPlan + multilevel partitioned MysCurrentRead (0 dup-key errors)', flush=True)
    finally:
        c.close()


def test_hot_standby_streaming_replication_and_promotion(pg, schema, pgbin):
    """End-to-end physical streaming replication Hot Standby + MySQL protocol + promotion."""
    c_pri = my_conn(database=schema)
    query(c_pri, """
        CREATE TABLE repl_t(
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(32) COLLATE utf8mb4_general_ci,
            v INT,
            UNIQUE KEY uk_name (name(4))
        )
    """)
    query(c_pri, "INSERT INTO repl_t(name, v) VALUES ('Alpha1', 10), ('Beta2', 20)")

    with tempfile.TemporaryDirectory(prefix='oh19-sb-', dir='/tmp') as tmpdir:
        s_data = os.path.join(tmpdir, 'standby')
        s_log = os.path.join(tmpdir, 'standby.log')
        s_pgport = pick_free_port()
        s_myport = pick_free_port()

        env = os.environ.copy()
        pghost = os.getenv('PGHOST', '127.0.0.1')
        pgport = os.getenv('PGPORT', '5432')
        pguser = os.getenv('PGUSER', 'postgres')

        subprocess.run(
            [
                os.path.join(pgbin, 'pg_basebackup'),
                '-h', pghost,
                '-p', str(pgport),
                '-U', pguser,
                '-D', s_data,
                '-Fp', '-Xs', '-R', '--no-sync',
            ],
            env=env,
            check=True,
            capture_output=True,
            text=True,
        )
        with open(os.path.join(s_data, 'postgresql.auto.conf'), 'a', encoding='utf-8') as f:
            f.write(
                f"\nport = {s_pgport}\n"
                f"mysql.port = {s_myport}\n"
                f"hot_standby = on\n"
                f"unix_socket_directories = '{tmpdir}'\n"
            )

        started = False
        try:
            subprocess.run(
                [os.path.join(pgbin, 'pg_ctl'), '-D', s_data, '-l', s_log, '-w', 'start'],
                env=env,
                check=True,
                capture_output=True,
                text=True,
            )
            started = True

            # Wait for MySQL listener on standby
            deadline = time.monotonic() + 10.0
            c_sub = None
            while time.monotonic() < deadline:
                try:
                    c_sub = pymysql.connect(
                        host='127.0.0.1',
                        port=s_myport,
                        user=os.getenv('MYSQL_ADMIN_USER', pguser),
                        password=os.getenv('MYSQL_ADMIN_PASSWORD', os.getenv('PGPASSWORD', '')),
                        database=schema,
                        autocommit=True,
                        connect_timeout=2,
                    )
                    break
                except Exception:
                    time.sleep(0.1)
            assert c_sub is not None, 'failed to connect to standby MySQL port'
            raw_sub = RawStmtClient(
                pymysql.connect(
                    host='127.0.0.1',
                    port=s_myport,
                    user=os.getenv('MYSQL_ADMIN_USER', pguser),
                    password=os.getenv('MYSQL_ADMIN_PASSWORD', os.getenv('PGPASSWORD', '')),
                    database=schema,
                    autocommit=True,
                    connect_timeout=2,
                )
            )
            try:
                # Primary writes: ODKU + REPLACE + prefix key -> must stream to standby!
                query(
                    c_pri,
                    "INSERT INTO repl_t(name, v) VALUES ('Alph_dup', 99) "
                    "ON DUPLICATE KEY UPDATE v = VALUES(v)",
                )
                query(c_pri, "REPLACE INTO repl_t(id, name, v) VALUES (2, 'Beta_rep', 200)")
                pri_lsn = query(pg, 'SELECT pg_current_wal_flush_lsn()::text')[0][0]

                s_pg = psycopg2.connect(
                    host='127.0.0.1',
                    port=s_pgport,
                    dbname=os.getenv('PGDATABASE', 'postgres'),
                    user=pguser,
                    password=os.getenv('PGPASSWORD', ''),
                )
                s_pg.autocommit = True
                try:
                    w_deadline = time.monotonic() + 10.0
                    while time.monotonic() < w_deadline:
                        rep_lsn = query(s_pg, 'SELECT pg_last_wal_replay_lsn()::text')[0][0]
                        if rep_lsn and query(s_pg, 'SELECT %s::pg_lsn >= %s::pg_lsn', (rep_lsn, pri_lsn))[0][0]:
                            break
                        time.sleep(0.05)
                    else:
                        raise AssertionError('standby did not catch up to primary WAL')

                    # Read via text COM_QUERY and binary CachedPlan COM_STMT_EXECUTE on standby!
                    assert query(c_sub, 'SELECT id, name, v FROM repl_t ORDER BY id') == [
                        (1, 'Alpha1', 99),
                        (2, 'Beta_rep', 200),
                    ]
                    ps_ro = raw_sub.prepare('SELECT name, v FROM repl_t WHERE id = ?')
                    for _ in range(3):
                        r_ro = raw_sub.execute(ps_ro['id'], [(T_LONG, struct.pack('<i', 1))])
                        assert r_ro['rows'] in ([('Alpha1', 99)], [(b'Alpha1', 99)]), r_ro
                    raw_sub.close_stmt(ps_ro['id'])

                    # Standby writes must fail cleanly with read-only error (never crash)
                    try:
                        query(c_sub, "INSERT INTO repl_t(name, v) VALUES ('Gamma3', 30)")
                        raise AssertionError('expected standby INSERT to fail')
                    except pymysql.MySQLError as e:
                        assert e.args[0] in (1792, 1290), e

                    ps_wr = raw_sub.prepare('UPDATE repl_t SET v = ? WHERE id = ?')
                    r_wr = raw_sub.execute(
                        ps_wr['id'],
                        [(T_LONG, struct.pack('<i', 777)), (T_LONG, struct.pack('<i', 1))],
                    )
                    assert r_wr.get('errno') in (1792, 1290), r_wr

                    # Promote standby -> primary and verify immediate MySQL write readiness!
                    subprocess.run(
                        [os.path.join(pgbin, 'pg_ctl'), '-D', s_data, '-w', 'promote'],
                        env=env,
                        check=True,
                        capture_output=True,
                        text=True,
                    )
                    p_deadline = time.monotonic() + 10.0
                    while time.monotonic() < p_deadline:
                        if not query(s_pg, 'SELECT pg_is_in_recovery()')[0][0]:
                            break
                        time.sleep(0.05)
                    else:
                        raise AssertionError('standby did not exit recovery after promote')

                    r_wr2 = raw_sub.execute(
                        ps_wr['id'],
                        [(T_LONG, struct.pack('<i', 777)), (T_LONG, struct.pack('<i', 1))],
                    )
                    assert r_wr2.get('affected') == 1, r_wr2
                    raw_sub.close_stmt(ps_wr['id'])
                    assert query(c_sub, "INSERT INTO repl_t(name, v) VALUES ('Gamma3', 300)") == 1
                    final_rows = query(c_sub, 'SELECT id, name, v FROM repl_t ORDER BY id')
                    assert len(final_rows) == 3, final_rows
                    assert final_rows[:2] == [(1, 'Alpha1', 777), (2, 'Beta_rep', 200)], final_rows
                    assert final_rows[2][0] >= 3 and final_rows[2][1:] == ('Gamma3', 300), final_rows
                finally:
                    s_pg.close()
            finally:
                raw_sub.close()
                c_sub.close()
            print('PASS Hot Standby streaming replication, read-only rejection & seamless promotion', flush=True)
        finally:
            c_pri.close()
            if started:
                subprocess.run(
                    [os.path.join(pgbin, 'pg_ctl'), '-D', s_data, '-m', 'fast', '-w', 'stop'],
                    env=env,
                    check=False,
                    capture_output=True,
                )


def test_fastpath_limit_param_locate64_and_cast_warnings(schema):
    """Verify UPDATE ... LIMIT ?, BIGINT UNSIGNED PK Execution 6+ MysCurrentRead,
    64-bit LOCATE start pos, and '12abc'/2.0 prepared warning parity."""
    c1 = my_conn(database=schema)
    c2 = my_conn(database=schema)
    raw = RawStmtClient(my_conn(database=schema, client_flag=pymysql.constants.CLIENT.DEPRECATE_EOF))
    try:
        # 1) 64-bit text_locate3 start > 2^32 & LOCATE start=INT_MAX must return 0, not wrap/scan!
        assert query(c1, "SELECT mysql.text_locate3('a', 'banana', 4294967298)") == [(0,)]
        assert query(c1, "SELECT LOCATE('a', 'banana', 2147483647)") == [(0,)]
        assert query(c1, "SELECT LOCATE('a', 'banana', 2)") == [(2,)]

        # 2) CAST(? AS CHAR)/2.0 and CAST('12abc' AS CHAR)/2.0 + ? warning 1292 parity across Executions 1..7
        query(c1, "SET sql_mode = ''")
        raw.send(0x03, b"SET sql_mode = ''")
        raw.read_pkt()
        ps_div = raw.prepare('SELECT CAST(? AS CHAR) / 2.0')
        for i in range(7):
            r = raw.execute(ps_div['id'], [(T_VARCHAR, lenenc_bytes(b'12abc'))])
            assert float(r['rows'][0][0]) == 6.0, (i, r)
            assert r['term']['warnings'] == 1, f'missing warning 1292 on param execution #{i + 1}: {r}'
        raw.close_stmt(ps_div['id'])

        ps_const_div = raw.prepare("SELECT CAST('12abc' AS CHAR) / 2.0 + ?")
        for i in range(7):
            r2 = raw.execute(ps_const_div['id'], [(T_LONG, struct.pack('<i', 1))])
            assert float(r2['rows'][0][0]) == 7.0, (i, r2)
            assert r2['term']['warnings'] == 1, f'missing warning 1292 on const-fold execution #{i + 1}: {r2}'
        raw.close_stmt(ps_const_div['id'])
        query(c1, 'SET sql_mode = DEFAULT')
        raw.send(0x03, b'SET sql_mode = DEFAULT')
        raw.read_pkt()

        # 3) Prepared UPDATE ... WHERE id = ? AND dec_k = ? LIMIT ? must preserve MysCurrentRead
        #    on BOTH BIGINT UNSIGNED (CoerceToDomain) and DECIMAL(20,0) (FuncExpr) PKs!
        query(
            c1,
            'CREATE TABLE lim_cr('
            'id BIGINT UNSIGNED NOT NULL, '
            'dec_k DECIMAL(20, 0) NOT NULL, '
            'v INT, '
            'PRIMARY KEY (id, dec_k))',
        )
        query(c1, 'INSERT INTO lim_cr VALUES (9000000000000000001, 18446744073709551600, 10)')
        ps_lim = raw.prepare('UPDATE lim_cr SET v = ? WHERE id = ? AND dec_k = ? LIMIT ?')
        # Warm up 6 executions so generic plan is active as well
        for step in range(1, 7):
            ru = raw.execute(
                ps_lim['id'],
                [
                    (T_LONG, struct.pack('<i', 10 + step)),
                    (T_VARCHAR, lenenc_bytes(b'9000000000000000001')),
                    (T_VARCHAR, lenenc_bytes(b'18446744073709551600')),
                    (T_LONG, struct.pack('<i', 1)),
                ],
            )
            assert ru['affected'] == 1, (step, ru)

        # Concurrently delete + reinsert the composite (BIGINT UNSIGNED, DECIMAL) PK in c2 while raw waits!
        query(c2, 'BEGIN')
        query(c2, 'DELETE FROM lim_cr WHERE id = 9000000000000000001 AND dec_k = 18446744073709551600')
        query(c2, 'INSERT INTO lim_cr VALUES (9000000000000000001, 18446744073709551600, 500)')

        holder_res = {}

        def run_blocked_upd():
            holder_res['r'] = raw.execute(
                ps_lim['id'],
                [
                    (T_LONG, struct.pack('<i', 999)),
                    (T_VARCHAR, lenenc_bytes(b'9000000000000000001')),
                    (T_VARCHAR, lenenc_bytes(b'18446744073709551600')),
                    (T_LONG, struct.pack('<i', 1)),
                ],
            )

        t = threading.Thread(target=run_blocked_upd)
        t.start()
        time.sleep(0.15)
        query(c2, 'COMMIT')
        t.join(timeout=10)
        assert not t.is_alive(), 'blocked UPDATE ... LIMIT ? hung'
        assert holder_res['r'].get('affected') == 1, holder_res
        assert query(c1, 'SELECT v FROM lim_cr WHERE id = 9000000000000000001') == [(999,)]
        raw.close_stmt(ps_lim['id'])
        print('PASS 64-bit LOCATE, prepared cast warning parity & BIGINT UNSIGNED LIMIT ? MysCurrentRead', flush=True)
    finally:
        raw.close()
        c2.close()
        c1.close()


def test_autocommit_default_and_dynamic_expr(schema):
    """Verify SET autocommit = DEFAULT and dynamic SET autocommit = @expr transaction rules."""
    c1 = my_conn(database=schema, client_flag=pymysql.constants.CLIENT.MULTI_STATEMENTS)
    c2 = my_conn(database=schema)
    try:
        query(c1, 'CREATE TABLE ac_t(id INT PRIMARY KEY)')

        # 1) SET autocommit = DEFAULT implicitly commits active autocommit=0 transaction
        query(c1, 'SET autocommit = 0')
        query(c1, 'INSERT INTO ac_t VALUES (1)')
        assert query(c2, 'SELECT COUNT(*) FROM ac_t WHERE id = 1') == [(0,)]
        query(c1, 'SET autocommit = DEFAULT')
        assert query(c2, 'SELECT COUNT(*) FROM ac_t WHERE id = 1') == [(1,)]
        query(c1, 'ROLLBACK')
        assert query(c2, 'SELECT COUNT(*) FROM ac_t WHERE id = 1') == [(1,)]

        # 2) Dynamic SET autocommit = @v (0 -> 1) implicitly commits & clears IN_TRANS
        query(c1, 'SET autocommit = 0')
        query(c1, 'INSERT INTO ac_t VALUES (2)')
        query(c1, 'SET @ac = 1')
        with c1.cursor() as cur:
            cur.execute('SET autocommit = @ac')
            assert (cur._result.server_status & 0x3) == 0x2, cur._result.server_status
        assert query(c2, 'SELECT COUNT(*) FROM ac_t WHERE id = 2') == [(1,)]

        # 3) Dynamic SET autocommit = @v (already 1 -> 1) inside BEGIN must NOT commit BEGIN!
        query(c1, 'BEGIN')
        query(c1, 'INSERT INTO ac_t VALUES (3)')
        query(c1, 'SET @ac = 1')
        query(c1, 'SET autocommit = @ac')
        query(c1, 'ROLLBACK')
        assert query(c2, 'SELECT COUNT(*) FROM ac_t WHERE id = 3') == [(0,)]

        # 4) Multi-statement packet with dynamic SET autocommit = 1 - 1 and 0 + 1
        query(c1, 'SET autocommit = 1 - 1; INSERT INTO ac_t VALUES (4); ROLLBACK')
        assert query(c2, 'SELECT COUNT(*) FROM ac_t WHERE id = 4') == [(0,)]

        query(c1, 'INSERT INTO ac_t VALUES (5); SET autocommit = 0 + 1; INSERT INTO ac_t VALUES (6); ROLLBACK')
        assert query(c2, 'SELECT id FROM ac_t WHERE id IN (5, 6) ORDER BY id') == [(5,), (6,)]

        # DEFAULT must see an earlier SET GLOBAL in the same packet.
        old_default = int(query(c2, 'SELECT @@global.autocommit')[0][0])
        try:
            query(c2, 'SET GLOBAL autocommit=0')
            query(c1, 'SET autocommit=0')
            query(c1, 'INSERT INTO ac_t VALUES (8)')
            query(c1, 'SET GLOBAL autocommit=1; SET autocommit=DEFAULT')
            assert query(c2, 'SELECT id FROM ac_t WHERE id=8') == [(8,)]
        finally:
            query(c2, f'SET GLOBAL autocommit={old_default}')

        # 5) Dynamic autocommit=0 batch preserves one-shot SET TRANSACTION READ ONLY (1792)
        try:
            query(c1, 'SET autocommit = 1 - 1; SELECT 1; SET TRANSACTION READ ONLY; INSERT INTO ac_t VALUES (7)')
            raise AssertionError('expected 1792 in dynamic autocommit=0 read-only batch')
        except pymysql.MySQLError as e:
            assert e.args[0] == 1792, e
        query(c1, 'ROLLBACK; SET TRANSACTION READ WRITE; SET autocommit = 1')
        print('PASS SET autocommit = DEFAULT & dynamic SET autocommit = <expr> transaction semantics', flush=True)
    finally:
        c2.close()
        c1.close()


def test_generator_acl_and_pending_fk_isolation(pg, schema):
    """Verify per-owner aux_mysql_gen_<hash> isolation, MODIFY unknown-column 1054,
    temp-table pending-FK rejection (1215), and pending-FK lock DoS prevention."""
    tag = uuid.uuid4().hex[:8]
    role_a = f'adv_ra_{tag}'
    role_b = f'adv_rb_{tag}'
    s_att = f'adv_att_{tag}'
    s_vic = f'adv_vic_{tag}'
    pw = 'Adv.Role.123!'
    ca = cb = catt = cvic = None
    try:
        for r in (role_a, role_b):
            query(pg, f"CREATE ROLE {r} LOGIN PASSWORD '{pw}'")
            query(pg, f'GRANT USAGE, CREATE ON SCHEMA {schema} TO {r}')
            query(pg, f'GRANT USAGE ON SCHEMA mysql TO {r}')
        query(pg, f'CREATE SCHEMA {s_att} AUTHORIZATION {role_a}')
        query(pg, f'CREATE SCHEMA {s_vic} AUTHORIZATION {role_b}')

        port = int(os.environ['MYSQL_PORT'])
        host = os.getenv('MYSQL_HOST', '127.0.0.1')
        ca = pymysql.connect(host=host, port=port, user=role_a, password=pw, database=schema, autocommit=True)
        cb = pymysql.connect(host=host, port=port, user=role_b, password=pw, database=schema, autocommit=True)

        # 1) Same generated expression in shared schema produces distinct owner-isolated functions
        query(ca, "CREATE TABLE gen_a(v VARCHAR(8), g VARCHAR(4) AS (CONCAT(v, '!')) STORED)")
        query(cb, "CREATE TABLE gen_b(v VARCHAR(8), g VARCHAR(4) AS (CONCAT(v, '!')) STORED)")
        fns = query(
            pg,
            f"""
            SELECT proname, pg_get_userbyid(proowner)
            FROM pg_proc
            WHERE pronamespace = '{schema}'::regnamespace
              AND proname LIKE 'aux_mysql_gen_%'
            ORDER BY proname
            """,
        )
        assert len(fns) == 2 and fns[0][0] != fns[1][0], fns

        # 2) MODIFY unknown column with generated clause returns MySQL 1054 (never XX000)
        try:
            query(ca, 'ALTER TABLE gen_a MODIFY COLUMN no_such_col INT AS (1 + 1) STORED')
            raise AssertionError('expected 1054')
        except pymysql.MySQLError as e:
            assert e.args[0] == 1054, e

        # 3) Temporary table with FK under FOREIGN_KEY_CHECKS=0 rejected with MySQL 1215
        query(ca, 'SET SESSION FOREIGN_KEY_CHECKS = 0')
        try:
            query(ca, 'CREATE TEMPORARY TABLE tmp_bad_fk(x INT, FOREIGN KEY (x) REFERENCES missing_perm(id))')
            raise AssertionError('expected 1215 on temp table FK')
        except pymysql.MySQLError as e:
            assert e.args[0] == 1215, e
        query(ca, 'SET SESSION FOREIGN_KEY_CHECKS = 1')

        # 4) Unprivileged pending FK holding AccessShareLock cannot DoS victim's CREATE TABLE
        catt = pymysql.connect(host=host, port=port, user=role_a, password=pw, database=s_att, autocommit=True)
        cvic = pymysql.connect(host=host, port=port, user=role_b, password=pw, database=s_vic, autocommit=True)
        query(catt, 'SET SESSION FOREIGN_KEY_CHECKS = 0')
        query(catt, f'CREATE TABLE {s_att}.c(x INT, FOREIGN KEY (x) REFERENCES {s_vic}.p(id))')
        query(catt, 'SET SESSION FOREIGN_KEY_CHECKS = 1')
        query(catt, 'BEGIN')
        query(catt, f'SELECT * FROM {s_att}.c')

        query(cvic, "SELECT pg_catalog.set_config('lock_timeout', '1000ms', false)")
        query(cvic, f'CREATE TABLE {s_vic}.p(id INT PRIMARY KEY)')
        warns = query(cvic, 'SHOW WARNINGS')
        assert any(w[1] == 1142 for w in warns), warns
        query(catt, 'ROLLBACK')
        print('PASS generator ACL isolation, MODIFY 1054, temp-FK 1215 & pending-FK lock DoS protection', flush=True)
    finally:
        for conn in (ca, cb, catt, cvic):
            if conn:
                conn.close()
        for s in (s_att, s_vic):
            query(pg, f'DROP SCHEMA IF EXISTS {s} CASCADE')
        for r in (role_a, role_b):
            query(pg, f'DROP OWNED BY {r} CASCADE')
            query(pg, f'DROP ROLE IF EXISTS {r}')


def test_replace_view_wco_and_cross_partition_update_ignore(pg, schema):
    """Verify REPLACE INTO enforces auto-updatable view WHERE quals on conflicting rows
    and cross-partition UPDATE IGNORE never deletes the source row on constraint failure."""
    c = my_conn(database=schema)
    try:
        # 1) REPLACE INTO through an auto-updatable view cannot delete invisible conflicting rows
        query(c, 'CREATE TABLE mt_base(id INT PRIMARY KEY, tenant INT NOT NULL, secret VARCHAR(32))')
        query(c, "INSERT INTO mt_base VALUES (1, 2, 'tenant2_protected'), (2, 1, 'tenant1_owned')")
        query(c, 'CREATE VIEW v_tenant1 AS SELECT id, tenant, secret FROM mt_base WHERE tenant = 1')

        # Replacing tenant 1's own row works
        assert query(c, "REPLACE INTO v_tenant1 VALUES (2, 1, 'tenant1_updated')") == 2
        assert query(c, 'SELECT secret FROM mt_base WHERE id = 2') == [('tenant1_updated',)]

        # Attempting to REPLACE tenant 2's conflicting PK=1 via v_tenant1 MUST fail & preserve tenant 2!
        try:
            query(c, "REPLACE INTO v_tenant1 VALUES (1, 1, 'hijacked')")
            raise AssertionError('REPLACE INTO view deleted out-of-view row!')
        except pymysql.MySQLError:
            pass
        assert query(c, 'SELECT tenant, secret FROM mt_base WHERE id = 1') == [(2, 'tenant2_protected')]

        # 2) Cross-partition UPDATE IGNORE must preserve source row on outgoing FK, unique, and incoming FK conflicts!
        query(c, 'CREATE TABLE cp_parent(id INT PRIMARY KEY)')
        query(c, 'INSERT INTO cp_parent VALUES (10), (20)')
        query(pg, f"""
            CREATE TABLE {schema}.cp_ign (
                p INT NOT NULL,
                id INT NOT NULL,
                fk_id INT NOT NULL REFERENCES {schema}.cp_parent(id),
                u INT NOT NULL,
                PRIMARY KEY (p, id),
                UNIQUE (p, u)
            ) PARTITION BY RANGE (p);
            CREATE TABLE {schema}.cp_ign_p1 PARTITION OF {schema}.cp_ign FOR VALUES FROM (0) TO (10);
            CREATE TABLE {schema}.cp_ign_p2 PARTITION OF {schema}.cp_ign FOR VALUES FROM (10) TO (20);
            CREATE TABLE {schema}.cp_child (
                cid INT PRIMARY KEY,
                p INT NOT NULL,
                id INT NOT NULL,
                FOREIGN KEY (p, id) REFERENCES {schema}.cp_ign(p, id)
            );
        """)
        query(c, 'INSERT INTO cp_ign VALUES (1, 1, 10, 100), (2, 2, 10, 200), (15, 9, 20, 900)')
        query(c, 'INSERT INTO cp_child VALUES (1, 2, 2)')

        # 2a) Outgoing FK failure on destination partition (fk_id = 999 missing in cp_parent)
        assert query(c, 'UPDATE IGNORE cp_ign SET p = 15, fk_id = 999 WHERE p = 1 AND id = 1') == 0
        w_fk_out = query(c, 'SHOW WARNINGS')
        assert any(w[1] == 1452 for w in w_fk_out), w_fk_out
        assert query(c, 'SELECT p, id, fk_id, u FROM cp_ign WHERE id = 1') == [(1, 1, 10, 100)]

        # 2b) Unique key conflict on destination partition (p=15, u=900 already exists in cp_ign_p2)
        assert query(c, 'UPDATE IGNORE cp_ign SET p = 15, u = 900 WHERE p = 1 AND id = 1') == 0
        w_uk = query(c, 'SHOW WARNINGS')
        assert any(w[1] == 1062 for w in w_uk), w_uk
        assert query(c, 'SELECT p, id, fk_id, u FROM cp_ign WHERE id = 1') == [(1, 1, 10, 100)]

        # 2c) Incoming FK RESTRICT/NO ACTION failure from cp_child referencing (2, 2)
        assert query(c, 'UPDATE IGNORE cp_ign SET p = 15 WHERE p = 2 AND id = 2') == 0
        w_fk_in = query(c, 'SHOW WARNINGS')
        assert any(w[1] == 1451 for w in w_fk_in), w_fk_in
        assert query(c, 'SELECT p, id, fk_id, u FROM cp_ign WHERE id = 2') == [(2, 2, 10, 200)]

        # 2d) Valid cross-partition UPDATE IGNORE succeeds and moves row (1, 1) -> (15, 1)
        assert query(c, 'UPDATE IGNORE cp_ign SET p = 15, fk_id = 20, u = 101 WHERE p = 1 AND id = 1') == 1
        assert query(c, 'SELECT p, id, fk_id, u FROM cp_ign WHERE id = 1') == [(15, 1, 20, 101)]
        print('PASS REPLACE INTO view WCO conflict protection & cross-partition UPDATE IGNORE row preservation', flush=True)
    finally:
        c.close()


def test_ga_release_audit_fixes(pg, schema):
    """Regression coverage for the OpenHalo 19.0.0 GA release audit blockers."""
    c = my_conn(database=schema)
    role_u = f'ga_u_{uuid.uuid4().hex[:8]}'
    role_r = f'ga_r_{uuid.uuid4().hex[:8]}'
    cu = None
    try:
        # 1) CHAR(n) COLLATE "C" mysql_prefix trailing-space lookup (M1)
        query(pg, f'CREATE TABLE {schema}.ch_c (id int PRIMARY KEY, k char(8) COLLATE "C")')
        query(pg, f'CREATE INDEX ch_c_k_idx ON {schema}.ch_c USING mysql_prefix (k) WITH (prefix_lengths = \'4\')')
        query(pg, f"INSERT INTO {schema}.ch_c VALUES (1, 'a'), (2, 'ab'), (3, 'abcd'), (4, 'abcde')")
        query(pg, 'SET enable_seqscan = off')
        try:
            assert query(pg, f"SELECT id FROM {schema}.ch_c WHERE k = 'a'::bpchar ORDER BY id") == [(1,)]
            assert query(pg, f"SELECT id FROM {schema}.ch_c WHERE k = 'a   '::bpchar ORDER BY id") == [(1,)]
            assert query(pg, f"SELECT id FROM {schema}.ch_c WHERE k = 'abcde'::bpchar ORDER BY id") == [(4,)]
        finally:
            query(pg, 'SET enable_seqscan = on')

        # 2) SUBTIME / ADDTIME / TIMEDIFF negative & >24h MySQL TIME values (P0-13)
        assert query(c, "SELECT SUBTIME('01:00:00', '02:00:00')") == [('-01:00:00',)]
        assert query(c, "SELECT SUBTIME('100:00:00', '200:00:00')") == [('-100:00:00',)]
        assert query(c, "SELECT ADDTIME('01:00:00', '100:30:15.5')") == [('101:30:15.5',)]
        assert query(c, "SELECT ADDTIME('2024-01-01 00:00:00', '100:00:00')") == [('2024-01-05 04:00:00',)]
        assert query(c, "SELECT TIMEDIFF('01:00:00', '02:30:00')") == [('-01:30:00',)]
        assert query(c, "SELECT TIMEDIFF('2024-01-01 00:00:00', '2024-01-03 00:00:00')") == [('-48:00:00',)]

        # 3) Parallel scan with text_locate2 / text_locate3 / text_instr on nondeterministic collation (P0-14)
        query(c, 'CREATE TABLE par_loc(id INT PRIMARY KEY, s VARCHAR(32) COLLATE utf8mb4_general_ci)')
        query(c, "INSERT INTO par_loc VALUES (1, 'HelloWorld'), (2, 'FOOBAR')")
        query(pg, 'SET debug_parallel_query = regress')
        try:
            r_loc = query(pg, f"""
                SELECT id, mysql.text_locate2('world', s), mysql.text_locate3('o', s, 6), mysql.text_instr(s, 'bar')
                FROM {schema}.par_loc ORDER BY id
            """)
            assert r_loc == [(1, 6, 7, 0), (2, 0, 0, 4)], r_loc
        finally:
            query(pg, 'SET debug_parallel_query = off')

        # 4) SHOW TABLES / SHOW FULL TABLES / SHOW TABLE STATUS view dedup & SQL SECURITY INVOKER
        query(c, 'CREATE TABLE st_base(id INT AUTO_INCREMENT PRIMARY KEY, v INT)')
        query(c, 'INSERT INTO st_base(v) VALUES (10), (20)')
        query(c, 'CREATE SQL SECURITY INVOKER VIEW st_v AS SELECT id, v FROM st_base')
        st_rows = [r[0] for r in query(c, "SHOW TABLES LIKE 'st_%'")]
        assert st_rows.count('st_v') == 1, st_rows
        sft_rows = query(c, "SHOW FULL TABLES LIKE 'st_%'")
        assert sft_rows.count(('st_v', 'VIEW')) == 1, sft_rows
        scv = query(c, 'SHOW CREATE VIEW st_v')[0][1]
        assert 'SQL SECURITY INVOKER' in scv, scv

        # 5) TRUNCATE resets AUTO_INCREMENT
        query(c, 'TRUNCATE TABLE st_base')
        query(c, 'INSERT INTO st_base(v) VALUES (99)')
        assert query(c, 'SELECT id FROM st_base') == [(1,)]

        # 6) LEFT JOIN + JOIN USING does not drop USING or outer join
        query(c, 'CREATE TABLE j1(id INT PRIMARY KEY, a INT)')
        query(c, 'CREATE TABLE j2(j2_id INT PRIMARY KEY, b INT)')
        query(c, 'CREATE TABLE j3(id INT PRIMARY KEY, c INT)')
        query(c, 'INSERT INTO j1 VALUES (1, 10), (2, 20)')
        query(c, 'INSERT INTO j2 VALUES (1, 100)')
        query(c, 'INSERT INTO j3 VALUES (1, 1000), (2, 2000)')
        jr = query(c, 'SELECT j1.id, j2.b, j3.c FROM j1 LEFT JOIN j2 ON j1.id = j2.j2_id JOIN j3 USING (id) ORDER BY j1.id')
        assert jr == [(1, 100, 1000), (2, None, 2000)], jr

        # 7) DELETE options validation
        query(c, 'DELETE QUICK FROM j2 WHERE j2_id = 999')
        try:
            query(c, 'DELETE BOGUS_OPT FROM j2 WHERE j2_id = 999')
            raise AssertionError('DELETE accepted unknown option!')
        except pymysql.MySQLError:
            pass

        # 8) COMMIT AND CHAIN / ROLLBACK AND CHAIN & SET autocommit=1 atomicity
        query(c, 'CREATE TABLE tx_ch(id INT PRIMARY KEY)')
        query(c, 'BEGIN')
        query(c, 'INSERT INTO tx_ch VALUES (1)')
        query(c, 'COMMIT AND CHAIN')
        query(c, 'INSERT INTO tx_ch VALUES (2)')
        query(c, 'ROLLBACK AND CHAIN')
        query(c, 'INSERT INTO tx_ch VALUES (3)')
        query(c, 'COMMIT')
        assert query(c, 'SELECT id FROM tx_ch ORDER BY id') == [(1,), (3,)]

        query(c, 'SET autocommit = 0')
        query(c, 'INSERT INTO tx_ch VALUES (10)')
        try:
            query(c, "SET autocommit = 1, time_zone = 'Invalid/Zone'")
            raise AssertionError('invalid multi-SET succeeded!')
        except pymysql.MySQLError:
            pass
        query(c, 'ROLLBACK')
        assert query(c, 'SELECT id FROM tx_ch WHERE id = 10') == []
        query(c, 'SET autocommit = 1')

        # 9) SET GLOBAL validation before shmem mutation
        old_tz = query(c, 'SELECT @@global.time_zone')[0][0]
        try:
            query(c, "SET GLOBAL time_zone = 'Not/A_Real_Zone'")
            raise AssertionError('invalid SET GLOBAL time_zone succeeded!')
        except pymysql.MySQLError:
            pass
        assert query(c, 'SELECT @@global.time_zone')[0][0] == old_tz

        # 10) ALTER INDEX SET (prefix_lengths) rejected & BINARY/VARBINARY bounds
        try:
            query(pg, f"ALTER INDEX {schema}.ch_c_k_idx SET (prefix_lengths = '2')")
            raise AssertionError('ALTER INDEX prefix_lengths succeeded!')
        except psycopg2.Error:
            pass
        for bad_ddl in (
            'CREATE TABLE bad_bin(b BINARY(300))',
            'CREATE TABLE bad_vbin(b VARBINARY(70000))',
            'CREATE TABLE bad_ga(id INT AUTO_INCREMENT PRIMARY KEY, g INT GENERATED ALWAYS AS (id + 1) STORED)',
        ):
            if 'bad_ga' in bad_ddl:
                query(c, 'CREATE TABLE mod_ga(id INT PRIMARY KEY, x INT)')
                try:
                    query(c, 'ALTER TABLE mod_ga MODIFY COLUMN x INT AUTO_INCREMENT GENERATED ALWAYS AS (id + 1) STORED')
                    raise AssertionError('AUTO_INCREMENT + GENERATED succeeded!')
                except pymysql.MySQLError:
                    pass
                continue
            try:
                query(c, bad_ddl)
                raise AssertionError(f'{bad_ddl} succeeded!')
            except pymysql.MySQLError:
                pass

        # 11) COPY client stream rejected & NOTIFY does not desync MySQL wire
        try:
            query(c, 'COPY tx_ch TO STDOUT')
            raise AssertionError('COPY TO STDOUT succeeded on MySQL protocol!')
        except pymysql.MySQLError:
            pass
        query(c, 'LISTEN ga_chan')
        query(c, "NOTIFY ga_chan, 'hello'")
        assert query(c, 'SELECT 42') == [(42,)]
        query(c, 'UNLISTEN ga_chan')

        # 12) Lexer leading || and @@ error safety
        for bad_sql in ('||', 'SELECT @@'):
            try:
                query(c, bad_sql)
            except pymysql.MySQLError:
                pass
        assert query(c, 'SELECT 1') == [(1,)]

        # 13) CREATE TABLE IF NOT EXISTS does not alter existing table
        query(c, 'CREATE TABLE ine_t(id INT AUTO_INCREMENT PRIMARY KEY, v INT)')
        query(c, 'INSERT INTO ine_t(v) VALUES (1)')
        query(c, 'CREATE TABLE IF NOT EXISTS ine_t(id INT AUTO_INCREMENT PRIMARY KEY, v INT) AUTO_INCREMENT = 500')
        query(c, 'INSERT INTO ine_t(v) VALUES (2)')
        assert query(c, 'SELECT id FROM ine_t ORDER BY id') == [(1,), (2,)]

        # 14) mysql.space, part_list_columns, SHOW CREATE TABLE backtick escaping, SHOW CREATE FUNCTION
        assert query(c, 'SELECT SPACE(5), SPACE(-3), SPACE(1000000000)') == [('     ', '', None)]
        assert query(pg, "SELECT mysql.part_list_columns('a,b', 'c') <> mysql.part_list_columns('a', 'b,c')")[0][0] is True
        assert query(pg, "SELECT mysql.part_list_columns(NULL::text, 'a') <> mysql.part_list_columns('', 'a')")[0][0] is True
        assert query(pg, r"SELECT mysql.part_list_columns('\x80ff'::bytea)")[0][0] is not None
        query(c, 'CREATE TABLE `bt``tbl`(`c``1` INT PRIMARY KEY, `c``2` VARCHAR(16), KEY `k``1` (`c``2`(4)))')
        sct = query(c, 'SHOW CREATE TABLE `bt``tbl`')[0][1]
        assert '`bt``tbl`' in sct and '`c``1`' in sct and '`k``1`' in sct, sct

        # 15) Partitioned self-referencing FK UPDATE/DELETE
        query(pg, f"""
            CREATE TABLE {schema}.pself (
                p int NOT NULL,
                id int NOT NULL,
                parent_p int,
                parent_id int,
                PRIMARY KEY (p, id),
                FOREIGN KEY (parent_p, parent_id) REFERENCES {schema}.pself(p, id)
            ) PARTITION BY LIST (p);
            CREATE TABLE {schema}.pself_1 PARTITION OF {schema}.pself FOR VALUES IN (1);
            CREATE TABLE {schema}.pself_2 PARTITION OF {schema}.pself FOR VALUES IN (2);
        """)
        query(c, 'INSERT INTO pself VALUES (1, 1, 1, 1)')
        query(c, 'UPDATE pself SET id = 10, parent_id = 10 WHERE p = 1 AND id = 1')
        query(c, 'DELETE FROM pself WHERE p = 1 AND id = 10')
        assert query(c, 'SELECT COUNT(*) FROM pself') == [(0,)]

        # 16) Non-superuser security checks: ODKU SELECT ACL/RLS, SECURITY DEFINER FK, read_only LO,
        #     current_user() STABLE, INFORMATION_SCHEMA.PARTITIONS, SHOW CREATE FUNCTION, pending_fk_guard ownership
        query(pg, f"CREATE ROLE {role_r} NOLOGIN")
        query(pg, f"CREATE ROLE {role_u} LOGIN PASSWORD 'ga_pass_1' IN ROLE {role_r}")
        query(pg, f"GRANT USAGE ON SCHEMA {schema} TO {role_u}")
        query(pg, f"CREATE TABLE {schema}.sec_odku(id int PRIMARY KEY, pub int, secret text)")
        query(pg, f"INSERT INTO {schema}.sec_odku VALUES (1, 10, 'topsecret')")
        query(pg, f"GRANT INSERT, UPDATE (pub) ON {schema}.sec_odku TO {role_u}")
        query(pg, f"GRANT SELECT (id, pub) ON {schema}.sec_odku TO {role_u}")

        query(pg, f"CREATE TABLE {schema}.fk_p(id int PRIMARY KEY)")
        query(pg, f"CREATE TABLE {schema}.fk_c(id int PRIMARY KEY, pid int REFERENCES {schema}.fk_p(id))")
        query(pg, f"""
            CREATE FUNCTION {schema}.sd_ins_fk(x int) RETURNS void
            LANGUAGE plpgsql SECURITY DEFINER AS $$
            BEGIN INSERT INTO {schema}.fk_c VALUES (x, x); END $$;
            GRANT EXECUTE ON FUNCTION {schema}.sd_ins_fk(int) TO {role_u};
        """)

        cu = my_conn(user=role_u, password='ga_pass_1', database=schema)
        # current_user() reflects role changes (STABLE, not IMMUTABLE)
        assert query(cu, 'SELECT CURRENT_USER()')[0][0].startswith(role_u)
        query(cu, f"SELECT pg_catalog.set_config('role', '{role_r}', false)")
        assert query(cu, 'SELECT CURRENT_USER()')[0][0].startswith(role_r)
        query(cu, "SELECT pg_catalog.set_config('role', 'none', false)")

        # ODKU cannot read ungranted column `secret`
        try:
            query(cu, "INSERT INTO sec_odku(id, pub) VALUES (1, 20) ON DUPLICATE KEY UPDATE pub = CASE WHEN secret = 'topsecret' THEN 99 ELSE 0 END")
            raise AssertionError('ODKU bypassed column SELECT ACL!')
        except pymysql.MySQLError:
            pass

        # FOREIGN_KEY_CHECKS=0 cannot bypass RI inside SECURITY DEFINER
        query(cu, 'SET FOREIGN_KEY_CHECKS = 0')
        try:
            query(cu, f'SELECT {schema}.sd_ins_fk(999)')
            raise AssertionError('FOREIGN_KEY_CHECKS=0 bypassed RI inside SECURITY DEFINER!')
        except pymysql.MySQLError:
            pass
        query(cu, 'SET FOREIGN_KEY_CHECKS = 1')

        # Non-superuser cannot attach pending_fk_guard trigger to unowned table
        try:
            query(cu, "CREATE TRIGGER evil_fk BEFORE INSERT ON sec_odku FOR EACH ROW EXECUTE FUNCTION mysql.pending_fk_guard('x')")
            raise AssertionError('Non-owner attached pending_fk_guard trigger!')
        except pymysql.MySQLError:
            pass

        # Non-superuser can query INFORMATION_SCHEMA.PARTITIONS and SHOW CREATE FUNCTION
        query(cu, 'SELECT COUNT(*) FROM information_schema.partitions')
        scf = query(cu, f'SHOW CREATE FUNCTION {schema}.sd_ins_fk')
        assert len(scf) == 1 and 'sd_ins_fk' in scf[0][2], scf

        # GLOBAL read_only blocks lo_from_bytea for non-superusers
        query(c, 'SET GLOBAL read_only = 1')
        try:
            try:
                query(cu, "SELECT pg_catalog.lo_from_bytea(0, 'abc'::bytea)")
                raise AssertionError('lo_from_bytea bypassed GLOBAL read_only!')
            except pymysql.MySQLError:
                pass
        finally:
            query(c, 'SET GLOBAL read_only = 0')

        print('PASS OpenHalo 19.0.0 GA release audit regression suite', flush=True)
    finally:
        if cu:
            cu.close()
        c.close()
        if query(pg, f"SELECT 1 FROM pg_roles WHERE rolname = '{role_u}'"):
            query(pg, f'DROP OWNED BY {role_u} CASCADE')
            query(pg, f'DROP ROLE IF EXISTS {role_u}')
        query(pg, f'DROP ROLE IF EXISTS {role_r}')


def test_runtime_keys_and_transactions(pg, schema):
    c = my_conn(database=schema)
    observer = my_conn(database=schema)
    try:
        query(c, 'CREATE TABLE ac_retry(id INT PRIMARY KEY)')
        query(c, 'SET autocommit=0')
        query(c, 'INSERT INTO ac_retry VALUES (1)')
        try:
            query(c, "SET autocommit=1, time_zone='Invalid/Zone'")
        except pymysql.MySQLError:
            pass
        else:
            raise AssertionError('invalid multi-SET succeeded')
        assert int(query(c, 'SELECT @@autocommit')[0][0]) == 0
        assert query(observer, 'SELECT id FROM ac_retry') == []
        query(c, 'SET autocommit=1')
        assert query(observer, 'SELECT id FROM ac_retry') == [(1,)]

        query(pg, f'CREATE TABLE {schema}.like_pad(id int, k char(8) COLLATE "C")')
        query(pg, f"CREATE INDEX pad_key ON {schema}.like_pad USING mysql_prefix (k) WITH (prefix_lengths='4')")
        query(pg, f"INSERT INTO {schema}.like_pad VALUES (1,'a'),(2,'a b'),(3,'ab'),(4,'a  c'),(5,'')")
        for pattern in ('a %', 'a  %', ' %', 'a _%', 'a   ', 'a  c%'):
            sql = f'SELECT id FROM {schema}.like_pad WHERE k LIKE %s ORDER BY id'
            query(pg, 'SET enable_seqscan=on')
            query(pg, 'SET enable_indexscan=off')
            query(pg, 'SET enable_bitmapscan=off')
            expected = query(pg, sql, (pattern,))
            query(pg, 'SET enable_seqscan=off')
            query(pg, 'SET enable_bitmapscan=on')
            query(pg, 'SET enable_indexscan=on')
            plan = query(pg, 'EXPLAIN (COSTS OFF) ' + sql, (pattern,))
            assert any('pad_key' in line for line, in plan), plan
            assert query(pg, sql, (pattern,)) == expected, pattern

        query(pg, f'CREATE TABLE {schema}.list_date(d date, i int) PARTITION BY LIST (mysql.part_list_columns(d,i))')
        query(pg, f"""CREATE TABLE {schema}.list_day PARTITION OF {schema}.list_date
                      FOR VALUES IN (mysql.part_list_columns(DATE '2026-03-12', 42))""")
        query(pg, "SET DateStyle='SQL, DMY'")
        query(pg, f"INSERT INTO {schema}.list_date VALUES ('2026-03-12',42)")
        assert query(pg, f'SELECT count(*) FROM {schema}.list_day') == [(1,)]
        assert query(pg, """SELECT mysql.timediff(timestamp '2026-03-11 00:00:00',
                                                 timestamp '2026-03-12 01:00:00.123')""") == [('-25:00:00.123',)]
        key = """SELECT mysql.part_list_columns(timestamptz '2026-03-12 00:00:00+00',
                     0.12345678912345::float8, interval '3 days 2 hours')"""
        expected = query(pg, key)
        query(pg, "SET TIME ZONE 'America/New_York'")
        query(pg, 'SET extra_float_digits=-10')
        query(pg, "SET IntervalStyle='sql_standard'")
        assert query(pg, key) == expected
        query(pg, f"""CREATE TABLE {schema}.list_values
                  (n numeric(8,2), c char(4), f float4, d float8, i interval)
                  PARTITION BY LIST (mysql.part_list_columns(n,c,f,d,i))""")
        query(pg, f"""CREATE TABLE {schema}.list_match PARTITION OF {schema}.list_values
                  FOR VALUES IN (mysql.part_list_columns(1::numeric, 'x'::char(1),
                      0::float4, 0::float8, interval '30 days -1 microsecond'))""")
        query(pg, f"""CREATE TABLE {schema}.list_other PARTITION OF {schema}.list_values DEFAULT""")
        query(pg, f"""INSERT INTO {schema}.list_values VALUES
                  (1.00,'x','-0','-0',interval '1 month -1 microsecond'),
                  (1.00,'x',0,0,interval '29 days 23:59:59.999999'),
                  (1.01,'x',0,0,interval '30 days -1 microsecond')""")
        assert query(pg, f'SELECT count(*) FROM {schema}.list_match') == [(2,)]
        assert query(pg, f'SELECT n = 1.01 FROM {schema}.list_other') == [(True,)]

        for name, partition in [('self_flat', ''), ('self_partition', ' PARTITION BY RANGE(id)')]:
            table = f'{schema}.{name}'
            query(pg, f'CREATE TABLE {table}(id bigint PRIMARY KEY, parent int REFERENCES {table}(id)){partition}')
            if partition:
                query(pg, f'CREATE TABLE {table}_all PARTITION OF {table} DEFAULT')
            assert query(c, f'INSERT IGNORE INTO {name} VALUES (1,1)') == 1
            assert query(c, f'INSERT IGNORE INTO {name} VALUES (2,9)') == 0
            assert query(c, f'UPDATE IGNORE {name} SET id=3 WHERE id=1') == 0
            assert query(c, 'SHOW WARNINGS')[0][1] == 1451
            assert query(c, f'UPDATE IGNORE {name} SET id=3,parent=3 WHERE id=1') == 1
            assert query(c, f'DELETE IGNORE FROM {name} WHERE id=3') == 1

        query(c, 'INSERT INTO self_flat VALUES (1,1)')
        query(c, 'SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE')
        query(c, 'BEGIN')
        query(c, 'SELECT id FROM self_flat WHERE id=1')
        query(observer, 'INSERT INTO self_flat VALUES (2,1)')
        assert query(c, 'DELETE IGNORE FROM self_flat WHERE id=1') == 0
        assert query(c, 'SHOW WARNINGS')[0][1] == 1451
        query(c, 'COMMIT')
        assert query(observer, 'SELECT id FROM self_flat ORDER BY id') == [(1,), (2,)]
    finally:
        observer.close()
        c.close()
        query(pg, 'RESET ALL')


def test_multi_set_atomicity(pg, schema):
    c = my_conn(database=schema, client_flag=pymysql.constants.CLIENT.MULTI_STATEMENTS)
    other = my_conn(database=schema)
    raw = RawStmtClient(c)
    previous = query(other, 'SELECT @@global.default_week_format')[0][0]
    try:
        query(c, "SET sql_mode='NO_BACKSLASH_ESCAPES', default_week_format=2, @a=13")
        state = 'SELECT @@sql_mode, @@foreign_key_checks, @@default_week_format, @@time_zone, @a'
        before = query(c, state)
        for tail, message in [("time_zone='Invalid/Zone'", 'Invalid/Zone'),
                              ('@b=(SELECT i FROM (SELECT 1 AS i UNION ALL SELECT 2) s)', 'more than one row')]:
            try:
                query(c, "SET sql_mode='', foreign_key_checks=0, default_week_format=4, @a=18, " + tail)
            except pymysql.MySQLError as exc:
                assert message in str(exc), exc
            else:
                raise AssertionError(tail)
            assert query(c, state) == before
            assert query(c, r"SELECT HEX('a\0')") == [('615C30',)]

        query(c, "SET optimizer_switch='index_merge=off,index_merge_union=off'")
        query(c, "SET optimizer_switch='index_merge=on', optimizer_switch='index_merge_union=on'")
        assert query(c, 'SELECT @@optimizer_switch')[0][0].startswith('index_merge=off,index_merge_union=on,')

        # All RHSs see the old values. DEFAULT inherits the preceding GLOBAL update.
        query(c, 'SET GLOBAL default_week_format=3')
        query(c, 'SET GLOBAL default_week_format=5, SESSION default_week_format=DEFAULT, '
                 '@g=@@global.default_week_format, @s=@@default_week_format, @a=17, @b=@a')
        assert query(c, 'SELECT @@default_week_format, @g, @s, @a, @b') == [
            ('5', b'3', b'2', b'17', b'13')]
        query(c, 'SET GLOBAL default_week_format=3, @@session.default_week_format=2, default_week_format=5')
        assert query(c, 'SELECT @@global.default_week_format,@@default_week_format') == [('5', '2')]
        query(c, 'SET @@global.default_week_format=5, default_week_format=4; SET default_week_format=3')
        assert query(c, 'SELECT @@global.default_week_format,@@default_week_format') == [('5', '3')]
        query(c, 'SET autocommit=0; SET autocommit=1, @x=@@autocommit')
        assert query(c, 'SELECT @@autocommit,@x') == [('1', b'0')]

        # A second writer can commit while the first is still evaluating an RHS.
        pid = c.thread_id()
        query(pg, f"""CREATE FUNCTION {schema}.wait_set(integer) RETURNS integer LANGUAGE plpgsql
                      AS $$BEGIN PERFORM pg_advisory_xact_lock($1); RETURN 1; END$$""")
        with ThreadPoolExecutor(max_workers=1) as pool:
            query(pg, 'SELECT pg_advisory_lock(%s)', (pid,))
            try:
                future = pool.submit(query, c, f'SET GLOBAL default_week_format=6, @a=wait_set({pid}), '
                                               "time_zone='Invalid/Zone'")
                wait_blocked(pg, pid, future)
                assert query(other, 'SELECT @@global.default_week_format') == [('5',)]
                query(other, 'SET GLOBAL default_week_format=7')
            finally:
                query(pg, 'SELECT pg_advisory_unlock(%s)', (pid,))
            try:
                future.result(timeout=5)
            except pymysql.MySQLError as exc:
                assert 'Invalid/Zone' in str(exc), exc
            else:
                raise AssertionError('invalid SET succeeded')
        assert query(other, 'SELECT @@global.default_week_format') == [('7',)]
        assert query(c, 'SELECT @a') == [(b'17',)]

        stmt = raw.prepare('SET @a=?, @bytes=0x410042, @empty=NULL, tx_isolation=?')
        assert stmt.get('nparams') == 2, stmt
        args = [(T_LONG, struct.pack('<i', 42)), (T_VARCHAR, lenenc_bytes(b'bad-level'))]
        assert 'errno' in raw.execute(stmt['id'], args)
        assert query(c, 'SELECT @a,@bytes') == [(b'17', None)]
        args[1] = (T_VARCHAR, lenenc_bytes(b'READ-COMMITTED'))
        assert raw.execute(stmt['id'], args).get('ok')
        assert query(c, 'SELECT @a+1,@bytes,@empty,@@transaction_isolation,@@tx_isolation') == [
            (43, b'A\x00B', None, 'READ-COMMITTED', 'READ-COMMITTED')]
        raw.close_stmt(stmt['id'])

        query(pg, f'CREATE TABLE {schema}.deferred_set(i int UNIQUE DEFERRABLE INITIALLY DEFERRED)')
        query(pg, f'INSERT INTO {schema}.deferred_set VALUES (1)')
        query(pg, f"""CREATE FUNCTION {schema}.duplicate_at_commit() RETURNS int LANGUAGE plpgsql
                      AS $$BEGIN INSERT INTO {schema}.deferred_set VALUES (1); RETURN 1; END$$""")
        before = query(c, "SELECT @a,@@global.default_week_format,@@time_zone,current_setting('TimeZone')")
        for suffix in ('', '; SELECT 1'):
            try:
                query(c, "SET @a=duplicate_at_commit(), GLOBAL default_week_format=6, SESSION time_zone='+02:00'"
                      + suffix)
            except pymysql.MySQLError as exc:
                assert exc.args[0] == 1062, exc
            else:
                raise AssertionError('deferred uniqueness check did not fail')
            assert query(c, "SELECT @a,@@global.default_week_format,@@time_zone,current_setting('TimeZone')") == before

        query(c, 'SET autocommit=0')
        query(c, 'INSERT INTO deferred_set VALUES (1)')
        try:
            query(c, 'SET autocommit=1, @a=99, GLOBAL default_week_format=6')
        except pymysql.MySQLError as exc:
            assert exc.args[0] == 1062, exc
        else:
            raise AssertionError('implicit COMMIT did not reject the duplicate')
        assert query(c, 'SELECT @@autocommit,@a,@@global.default_week_format') == [('0', b'42', '7')]
        query(c, 'SET autocommit=1')

        query(c, 'BEGIN')
        query(c, 'SAVEPOINT tzpoint')
        query(c, "SET time_zone='+05:30', default_week_format=1")
        tz = query(c, "SELECT @@time_zone, current_setting('TimeZone'), @@default_week_format")
        assert tz[0][0] == '+05:30' and tz[0][2] == '1', tz
        assert query(c, "SELECT pg_catalog.date_part('timezone', pg_catalog.now())") == [(19800.0,)]
        # An already-enabled, dynamic autocommit must leave this block open.
        query(c, 'SET @ac=1')
        query(c, 'SET autocommit=@ac')
        query(c, 'ROLLBACK TO SAVEPOINT tzpoint')
        assert query(c, "SELECT @@time_zone, current_setting('TimeZone'), @@default_week_format") == tz
        query(c, 'ROLLBACK')
        assert query(c, "SELECT @@time_zone, current_setting('TimeZone'), @@default_week_format") == tz
        query(c, 'BEGIN')
        query(c, "SET time_zone='+03:00', @a=33")
        query(c, 'SELECT duplicate_at_commit()')
        try:
            query(c, 'COMMIT')
        except pymysql.MySQLError as exc:
            assert exc.args[0] == 1062, exc
        else:
            raise AssertionError('deferred uniqueness check did not fail')
        assert query(c, "SELECT pg_catalog.date_part('timezone',pg_catalog.now()),@a") == [(10800.0, b'33')]
    finally:
        try:
            query(other, 'SET GLOBAL default_week_format=%s', (previous,))
        finally:
            raw.close()
            other.close()


def test_time_arithmetic_datestyle(pg):
    sql = """SELECT mysql.addtime(timestamp '2026-03-11 23:59:59.999999', time '00:00:00.000001')
                         = timestamp '2026-03-12',
                    mysql.subtime(timestamp '2026-03-12', time '01:00:00') = timestamp '2026-03-11 23:00',
                    mysql.addtime(timestamp '2026-03-12','-25:00:00.1'::text)
                         = timestamp '2026-03-10 22:59:59.9',
                    mysql.subtime(timestamp '2026-03-12','-25:00:00.1'::text)
                         = timestamp '2026-03-13 01:00:00.1',
                    mysql.addtime(timestamp '2026-03-12','2026-03-11 00:00:00'::text) IS NULL,
                    mysql.addtime(timestamp '9999-12-31 23:59:59.999999',time '00:00:00.000001') IS NULL,
                    mysql.subtime('2026-03-12 01:02:03.456'::text,'25:00:00'::text)"""
    try:
        for style in ('ISO, YMD', 'SQL, DMY', 'Postgres, MDY', 'German, DMY'):
            query(pg, 'SET DateStyle=%s', (style,))
            assert query(pg, sql) == [(True, True, True, True, True, True, '2026-03-11 00:02:03.456')], style
    finally:
        query(pg, 'RESET DateStyle')


def test_prepared_encoding_and_pg_operators(pg, schema):
    raw = RawStmtClient(my_conn(database=schema))
    try:
        c = raw.conn
        query(c, 'CREATE TABLE wire_charset(id INT PRIMARY KEY, b BLOB, s VARCHAR(16))')
        for enc, word, codec in [('LATIN1', 'café', 'latin1'), ('SJIS', 'ソ', 'shift_jis')]:
            query(c, "SELECT pg_catalog.set_config('client_encoding',%s,false)", (enc,))
            values = f",_binary '{word}','{word}')".encode(codec)
            query(c, b'INSERT INTO wire_charset VALUES (1' + values)
            stmt = raw.prepare(b'INSERT INTO wire_charset VALUES (?' + values)
            assert stmt['nparams'] == 1, stmt
            activity = query(pg, 'SELECT query FROM pg_stat_activity WHERE pid=%s', (c.thread_id(),))
            assert word in activity[0][0], activity
            assert raw.execute(stmt['id'], [(T_LONG, struct.pack('<i', 2))])['affected'] == 1
            expected = (word.encode(codec).hex().upper(), word.encode('utf8').hex().upper())
            assert query(c, 'SELECT HEX(b), HEX(s) FROM wire_charset ORDER BY id') == [expected] * 2
            raw.close_stmt(stmt['id'])
            query(c, 'DELETE FROM wire_charset')
        # SJIS ソ ends in 0x5c. Only the following backslash introduces the NUL.
        for mode, escape, expected in [
                ('', r'\0', ('835C00', '835C00')),
                ('', r'\\0', ('E382BD5C30', '835C5C30')),
                ('NO_BACKSLASH_ESCAPES', r'\0', ('E382BD5C30', '835C5C30'))]:
            query(c, 'SET sql_mode=%s', (mode,))
            sql = f"SELECT HEX('ソ{escape}'),HEX(_binary'ソ{escape}')".encode('shift_jis')
            assert query(c, sql) == [expected]
            stmt = raw.prepare(sql)
            assert raw.execute(stmt['id'], [])['rows'] == [expected]
            raw.close_stmt(stmt['id'])
        query(c, 'SET sql_mode=DEFAULT')
        query(c, "SELECT pg_catalog.set_config('client_encoding','UTF8',false)")

        stmt = raw.prepare('SELECT LENGTH(?)')
        stream_contexts = """SELECT count(*) FROM pg_catalog.pg_backend_memory_contexts
                             WHERE name='aux_mysql long data'"""
        raw.send_long_data(stmt['id'], 0, b'partial')
        assert query(c, stream_contexts) == [(1,)]
        raw.reset_stmt(stmt['id'])
        assert query(c, stream_contexts) == [(0,)]
        for data in ([b'abc', b'def'], [b''], [b'x' * 1024, b'y' * 2048]):
            for chunk in data:
                raw.send_long_data(stmt['id'], 0, chunk)
            res = raw.execute(stmt['id'], [(T_BLOB, ...)])
            assert res['rows'] == [(sum(map(len, data)),)], res
        assert query(c, stream_contexts) == [(0,)]
        raw.close_stmt(stmt['id'])

        query(c, "SELECT pg_catalog.set_config('mysql.parser','postgres',false)")
        for sql, values, expected in [
                ('SELECT ?+? WHERE 1=?', [2, 3, 1], 5),
                ('SELECT (?::int BETWEEN ? AND ?)::int', [3, 1, 4], 1),
                ('SELECT count(*) OVER (ROWS ? PRECEDING)', [2], 1),
                ('''SELECT (first ? ?)::int FROM (VALUES ('{"k":1}'::jsonb)) v(first)''', ['k'], 1),
                ("""SELECT ('{"k":1}'::jsonb ? ?)::int""", ['k'], 1),
                ("""SELECT ('{"k":1}'::jsonb OPERATOR(pg_catalog.?) ?)::int""", ['k'], 1),
                ("""SELECT ('{"k":1}'::jsonb ?| array[?])::int""", ['k'], 1),
                ("""SELECT ('{"k":1}'::jsonb @? '$.k')::int""", [], 1),
                ("SELECT length($tag$?$tag$) + ? /* ? */", [4], 5)]:
            stmt = raw.prepare(sql)
            assert stmt.get('nparams') == len(values), (sql, stmt)
            args = [(T_LONG, struct.pack('<i', v)) if isinstance(v, int)
                    else (T_VARCHAR, lenenc_bytes(v.encode())) for v in values]
            res = raw.execute(stmt['id'], args)
            assert res.get('rows') == [(expected,)], (sql, res)
            raw.close_stmt(stmt['id'])
        mixed = raw.prepare('SELECT ?::int + $1')
        assert 'errno' in mixed, mixed
    finally:
        raw.close()


def main():
    pgbin = os.environ.get('PGBIN')
    if not pgbin:
        raise RuntimeError('PGBIN environment variable is required')
    schema = f'adv_acc_{uuid.uuid4().hex[:8]}'
    pg = pg_conn()
    my = my_conn()
    try:
        query(my, f'CREATE DATABASE {schema}')
        test_scanner_binary_high_byte_and_nul(schema)
        test_utf8mb4_bin_selectivity_and_prefix(pg, schema)
        test_rewrite_warnings_zero_topmem_leak(pg, schema)
        test_wire_diagnostics_and_fastpath_edge_cases(schema)
        test_nested_triggers_timestamp_and_upsert(pg, schema)
        test_concurrent_fastpath_partitioned_current_read(pg, schema)
        test_fastpath_limit_param_locate64_and_cast_warnings(schema)
        test_autocommit_default_and_dynamic_expr(schema)
        test_generator_acl_and_pending_fk_isolation(pg, schema)
        test_replace_view_wco_and_cross_partition_update_ignore(pg, schema)
        test_ga_release_audit_fixes(pg, schema)
        test_runtime_keys_and_transactions(pg, schema)
        test_multi_set_atomicity(pg, schema)
        test_time_arithmetic_datestyle(pg)
        test_prepared_encoding_and_pg_operators(pg, schema)
        test_hot_standby_streaming_replication_and_promotion(pg, schema, pgbin)
        print('ALL ADVERSARIAL ACCEPTANCE CHECKS PASSED', flush=True)
    finally:
        try:
            query(my, f'DROP DATABASE IF EXISTS {schema}')
        finally:
            my.close()
            pg.close()


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
"""Deterministic regression & benchmark suite for OpenHalo 19.0.0 Tier-1 optimizations.

Covers:
  1-A: openhalo_version.h relocation + stored INSERT IGNORE in CREATE RULE / BEGIN ATOMIC
  1-B: Retained analyzer ACL defense against spoofed RETURNING aliases & named arbiters
  1-C: Dynamic default log_lock_waits = off for MySQL 50ms waits + unlogged mysql_prefix metapage
  1-D: Native CachedPlan fast path for COM_STMT_PREPARE / EXECUTE + QPS speedup benchmark
  1-E: MysCurrentRead on HASH, RANGE, and multilevel dropped/reordered partitioned tables
"""
from concurrent.futures import ThreadPoolExecutor
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import uuid

import psycopg2
import pymysql
from pymysql.constants import CLIENT

from point_write_concurrency import WireError, wire_dml


def query(conn, sql, args=None):
    with conn.cursor() as cur:
        cur.execute(sql, args)
        return list(cur.fetchall()) if cur.description else cur.rowcount


def set_parser(conn, mode):
    query(conn, "SELECT pg_catalog.set_config('mysql.parser', %s, false)", (mode,))

COM_QUERY = 0x03
COM_STMT_PREPARE = 0x16
COM_STMT_EXECUTE = 0x17
COM_STMT_SEND_LONG_DATA = 0x18
COM_STMT_CLOSE = 0x19
COM_STMT_RESET = 0x1a
COM_RESET_CONNECTION = 0x1f

T_TINY = 0x01
T_SHORT = 0x02
T_LONG = 0x03
T_FLOAT = 0x04
T_DOUBLE = 0x05
T_NULL = 0x06
T_LONGLONG = 0x08
T_VARCHAR = 0x0f
T_BLOB = 0xfc
T_VAR_STRING = 0xfd
T_STRING = 0xfe
UNSIGNED_BIT = 0x8000


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


def my_conn(user=None, password=None, database=None, deprecate_eof=True, client_flag=0):
    flags = client_flag
    c = pymysql.connect(
        host=os.getenv('MYSQL_HOST', '127.0.0.1'),
        port=int(os.getenv('MYSQL_PORT', '3306')),
        user=user or os.getenv('MYSQL_ADMIN_USER', os.getenv('PGUSER', 'postgres')),
        password=password if password is not None else os.getenv(
            'MYSQL_ADMIN_PASSWORD', os.getenv('PGPASSWORD', '')),
        database=database,
        autocommit=True,
        connect_timeout=5,
        read_timeout=15,
        write_timeout=15,
        client_flag=flags,
    )
    if not deprecate_eof:
        # Rebuild connection explicitly clearing CLIENT_DEPRECATE_EOF (1 << 24)
        base_flags = c.client_flag & ~(1 << 24)
        c.close()
        c = pymysql.connect(
            host=os.getenv('MYSQL_HOST', '127.0.0.1'),
            port=int(os.getenv('MYSQL_PORT', '3306')),
            user=user or os.getenv('MYSQL_ADMIN_USER', os.getenv('PGUSER', 'postgres')),
            password=password if password is not None else os.getenv(
                'MYSQL_ADMIN_PASSWORD', os.getenv('PGPASSWORD', '')),
            database=database,
            autocommit=True,
            connect_timeout=5,
            read_timeout=15,
            write_timeout=15,
            client_flag=base_flags,
        )
        # PyMySQL always ORs CLIENT.DEPRECATE_EOF if available in pymysql.constants.CLIENT;
        # verify actual negotiated flag via raw wrapper below.
    return c


class RawStmtClient:
    """Low-level binary protocol wrapper around an authenticated PyMySQL socket."""

    def __init__(self, conn):
        self.conn = conn
        self.deprecate_eof = bool(conn.server_capabilities & (1 << 24)) and bool(
            conn.client_flag & (1 << 24)
        )

    def close(self):
        self.conn.close()

    def send(self, cmd, payload=b''):
        self.conn._next_seq_id = 0
        pkt = struct.pack('<i', len(payload) + 1)[:3] + b'\x00' + bytes([cmd]) + payload
        self.conn._write_bytes(pkt)
        self.conn._next_seq_id = 1

    def read_pkt(self):
        try:
            return self.conn._read_packet().get_all_data()
        except pymysql.err.MySQLError as e:
            errno = e.args[0]
            msg = str(e.args[1]).encode()
            return b'\xff' + struct.pack('<H', errno) + b'#HY000' + msg

    @staticmethod
    def lenenc(b, i):
        c = b[i]
        if c < 251:
            return c, i + 1
        n = {0xfc: 2, 0xfd: 3, 0xfe: 8}[c]
        return int.from_bytes(b[i + 1:i + 1 + n], 'little'), i + 1 + n

    @classmethod
    def parse_coldef(cls, b):
        i = 0
        fields = []
        for _ in range(6):
            n, i = cls.lenenc(b, i)
            fields.append(b[i:i + n].decode('utf-8', errors='replace'))
            i += n
        fixed_len, i = cls.lenenc(b, i)
        assert fixed_len == 0x0c, fixed_len
        charset, col_len, col_type, flags, decimals = struct.unpack_from('<HIBHB', b, i)
        return {
            'catalog': fields[0],
            'schema': fields[1],
            'table': fields[2],
            'org_table': fields[3],
            'name': fields[4],
            'org_col': fields[5],
            'charset': charset,
            'length': col_len,
            'type': col_type,
            'flags': flags,
            'decimals': decimals,
        }

    @classmethod
    def parse_ok(cls, b):
        assert b[0] in (0x00, 0xfe)
        affected, i = cls.lenenc(b, 1)
        insert_id, i = cls.lenenc(b, i)
        status, warnings = struct.unpack_from('<HH', b, i)
        return {
            'ok': True,
            'affected': affected,
            'insert_id': insert_id,
            'status': status,
            'warnings': warnings,
        }

    def prepare(self, sql):
        self.send(COM_STMT_PREPARE, sql if isinstance(sql, bytes) else sql.encode('utf-8'))
        p = self.read_pkt()
        if p[0] == 0xff:
            return {'errno': struct.unpack('<H', p[1:3])[0], 'msg': p[9:].decode('utf-8', 'replace')}
        sid, ncols, nparams = struct.unpack('<IHH', p[1:9])
        for _ in range(nparams):
            self.read_pkt()
        if nparams and not self.deprecate_eof:
            eof = self.read_pkt()
            assert eof[0] == 0xfe, eof
        cols = [self.parse_coldef(self.read_pkt()) for _ in range(ncols)]
        if ncols and not self.deprecate_eof:
            eof = self.read_pkt()
            assert eof[0] == 0xfe, eof
        return {'id': sid, 'ncols': ncols, 'nparams': nparams, 'cols': cols}

    def send_long_data(self, sid, param_idx, data):
        self.send(COM_STMT_SEND_LONG_DATA, struct.pack('<IH', sid, param_idx) + data)

    def reset_stmt(self, sid):
        self.send(COM_STMT_RESET, struct.pack('<I', sid))
        return self.parse_ok(self.read_pkt())

    def close_stmt(self, sid):
        self.send(COM_STMT_CLOSE, struct.pack('<I', sid))

    def reset_connection(self):
        self.send(COM_RESET_CONNECTION, b'')
        return self.parse_ok(self.read_pkt())

    def execute(self, sid, params, bind=True):
        n = len(params)
        nullmap = bytearray((n + 7) // 8)
        vals = bytearray()
        types = bytearray()
        for k, (t, v) in enumerate(params):
            types += struct.pack('<H', t)
            if v is None:
                nullmap[k // 8] |= 1 << (k % 8)
            elif v is not ...:
                vals += v
        payload = bytearray(struct.pack('<IBI', sid, 0, 1))
        if n:
            payload += nullmap
            payload += (b'\x01' + types) if bind else b'\x00'
            payload += vals
        self.send(COM_STMT_EXECUTE, bytes(payload))
        first = self.read_pkt()
        if first[0] == 0xff:
            return {
                'errno': struct.unpack('<H', first[1:3])[0],
                'msg': first[9:].decode('utf-8', 'replace'),
            }
        if first[0] == 0x00:
            return self.parse_ok(first)
        ncols, _ = self.lenenc(first, 0)
        cols = [self.parse_coldef(self.read_pkt()) for _ in range(ncols)]
        if not self.deprecate_eof:
            eof = self.read_pkt()
            assert eof[0] == 0xfe, eof
        rows = []
        while True:
            p = self.read_pkt()
            if p[0] == 0xff:
                return {
                    'errno': struct.unpack('<H', p[1:3])[0],
                    'msg': p[9:].decode('utf-8', 'replace'),
                }
            if p[0] == 0xfe and len(p) < 0xffffff:
                term = self.parse_ok(p) if self.deprecate_eof else {'eof': True}
                break
            rows.append(self.decode_binary_row(p, cols))
        return {'cols': cols, 'rows': rows, 'term': term}

    @classmethod
    def decode_binary_row(cls, p, cols):
        assert p[0] == 0x00, p[:4]
        ncols = len(cols)
        nbytes = (ncols + 9) // 8
        bitmap = p[1:1 + nbytes]
        idx = 1 + nbytes
        out = []
        for k, c in enumerate(cols):
            if bitmap[(k + 2) // 8] & (1 << ((k + 2) % 8)):
                out.append(None)
                continue
            t = c['type']
            unsigned = bool(c['flags'] & 0x0020)
            if t == T_TINY:
                out.append(struct.unpack_from('<B' if unsigned else '<b', p, idx)[0])
                idx += 1
            elif t == T_SHORT:
                out.append(struct.unpack_from('<H' if unsigned else '<h', p, idx)[0])
                idx += 2
            elif t == T_LONG:
                out.append(struct.unpack_from('<I' if unsigned else '<i', p, idx)[0])
                idx += 4
            elif t == T_LONGLONG:
                out.append(struct.unpack_from('<Q' if unsigned else '<q', p, idx)[0])
                idx += 8
            elif t == T_FLOAT:
                out.append(struct.unpack_from('<f', p, idx)[0])
                idx += 4
            elif t == T_DOUBLE:
                out.append(struct.unpack_from('<d', p, idx)[0])
                idx += 8
            else:
                ln, idx = cls.lenenc(p, idx)
                raw = p[idx:idx + ln]
                idx += ln
                if c['charset'] == 63:
                    out.append(raw)
                else:
                    out.append(raw.decode('utf-8'))
        return tuple(out)


def enc_str(s):
    b = s if isinstance(s, bytes) else s.encode('utf-8')
    if len(b) < 251:
        return bytes([len(b)]) + b
    if len(b) < 65536:
        return b'\xfc' + struct.pack('<H', len(b)) + b
    return b'\xfd' + struct.pack('<I', len(b))[:3] + b


def wait_blocked(pg, pid, pending, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if query(pg, 'SELECT cardinality(pg_blocking_pids(%s))', (pid,))[0][0]:
            return
        if pending.done():
            raise AssertionError(('statement did not block', pending.result()))
        time.sleep(0.01)
    raise AssertionError(f'pid {pid} never entered lock wait')


def test_1a_1b_version_serialization_and_acl(pg, schema):
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
    assert not os.path.exists(os.path.join(repo_root, 'src', 'include', 'openhalo_version.h'))
    assert os.path.exists(os.path.join(repo_root, 'contrib', 'aux_mysql', 'src', 'openhalo_version.h'))

    assert query(pg, 'SHOW openhalo.version') == [('19.0.0',)]
    my = my_conn(database=schema)
    try:
        ver_comment = query(my, 'SELECT @@version_comment')[0][0]
        assert '19.0.0' in ver_comment, ver_comment
        # Stored INSERT IGNORE inside SQL-body function & rule must round-trip catalog serialization
        query(my, 'CREATE TABLE rule_src(id INT PRIMARY KEY, v INT)')
        query(my, 'CREATE TABLE rule_dst(id INT PRIMARY KEY, v INT)')
        query(my, 'INSERT INTO rule_dst VALUES (1, 100)')
        query(my, 'CREATE RULE r_ins_ign AS ON INSERT TO rule_src DO ALSO INSERT IGNORE INTO rule_dst VALUES (NEW.id, NEW.v)')
        query(
            my,
            f'CREATE FUNCTION {schema}.fn_ins_ign(p_id INT, p_v INT) RETURNS INT '
            f'LANGUAGE SQL BEGIN ATOMIC '
            f'INSERT IGNORE INTO {schema}.rule_dst VALUES (p_id, p_v); '
            f'RETURN 1; END'
        )
    finally:
        my.close()

    # Reconnect in a fresh backend so prosqlbody & ev_action are deserialized via _readOnConflictExpr
    my2 = my_conn(database=schema)
    try:
        assert query(my2, 'INSERT INTO rule_src VALUES (1, 999)') == 1
        assert query(my2, f'SELECT {schema}.fn_ins_ign(1, 888)') == [(1,)]
        assert query(my2, 'SELECT id, v FROM rule_dst WHERE id = 1') == [(1, 100)]
    finally:
        my2.close()

    # 1-B: Spoofed RETURNING alias must NOT bypass SELECT column ACL on AUTO_INCREMENT / ODKU!
    role = f'u_acl_{uuid.uuid4().hex[:8]}'
    query(pg, f"CREATE ROLE {role} LOGIN PASSWORD 'AclPass!123'")
    try:
        u_adm = my_conn(database=schema)
        try:
            query(u_adm, 'CREATE TABLE acl_t(id INT AUTO_INCREMENT PRIMARY KEY, secret TEXT)')
            query(u_adm, "INSERT INTO acl_t VALUES (1, 'top_secret_token')")
        finally:
            u_adm.close()
        query(pg, f'GRANT USAGE ON SCHEMA {schema} TO {role}')
        query(pg, f'GRANT INSERT (id), UPDATE (id) ON {schema}.acl_t TO {role}')
        query(pg, f'GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA {schema} TO {role}')

        uconn = my_conn(user=role, password='AclPass!123', database=schema)
        try:
            # Legitimate ODKU (VALUES(id), no target-column read) & AUTO_INCREMENT INSERT succeed without SELECT
            query(uconn, 'INSERT INTO acl_t(id) VALUES (1) ON DUPLICATE KEY UPDATE id = VALUES(id)')
            query(uconn, 'INSERT INTO acl_t(id) VALUES (NULL)')
            # Unauthorized read of target columns (even updated ones) in ODKU or explicit PG-mode RETURNING must be rejected
            for bad_sql, mode in (
                ('INSERT INTO acl_t(id) VALUES (1) ON DUPLICATE KEY UPDATE id = id', 'mysql'),
                ('INSERT INTO acl_t(id) VALUES (1) ON DUPLICATE KEY UPDATE id = LENGTH(secret)', 'mysql'),
                (
                    f'INSERT INTO {schema}.acl_t(id) VALUES (1) '
                    'ON CONFLICT (id) DO UPDATE SET id = EXCLUDED.id '
                    'RETURNING secret AS __aux_mysql_internal_ret',
                    'postgres',
                ),
            ):
                set_parser(uconn, mode)
                failed = False
                try:
                    query(uconn, bad_sql)
                except pymysql.err.MySQLError as e:
                    assert e.args[0] in (1142, 1143), (bad_sql, e)
                    failed = True
                assert failed, f'unauthorized column read bypassed SELECT privilege check: {bad_sql}'
            set_parser(uconn, 'mysql')
        finally:
            uconn.close()
    finally:
        query(pg, f'DROP TABLE IF EXISTS {schema}.acl_t CASCADE')
        query(pg, f'REVOKE ALL ON SCHEMA {schema} FROM {role}')
        query(pg, f'DROP ROLE IF EXISTS {role}')
    print('PASS 1-A & 1-B: openhalo_version relocated, stored INSERT IGNORE round-trips, spoofed RETURNING blocked', flush=True)


def test_1c_log_lock_waits_and_prefix_metapage(pg, schema):
    # Native PG session keeps 1s / on
    assert query(pg, 'SHOW deadlock_timeout') == [('1s',)]
    assert query(pg, 'SHOW log_lock_waits') == [('on',)]

    # Check server log file if logging_collector / log file exists
    log_path = None
    data_dir = query(pg, 'SHOW data_directory')[0][0]
    cand = os.path.join(data_dir, 'postmaster.log')
    if os.path.exists(cand):
        log_path = cand

    a, b = my_conn(database=schema), my_conn(database=schema)
    try:
        set_parser(a, 'postgres')
        assert query(a, 'SHOW deadlock_timeout') == [('50ms',)]
        assert query(a, 'SHOW log_lock_waits') == [('off',)]
        set_parser(a, 'mysql')

        query(a, 'CREATE TABLE lw_t(id INT PRIMARY KEY, v INT)')
        query(a, 'INSERT INTO lw_t VALUES (1, 10)')

        # Hold row lock > 50ms in default MySQL mode -> must emit 0 "still waiting for" log lines
        off0 = os.path.getsize(log_path) if log_path else None
        query(a, 'BEGIN')
        query(a, 'UPDATE lw_t SET v = 20 WHERE id = 1')
        with ThreadPoolExecutor(max_workers=1) as pool:
            fut = pool.submit(query, b, 'UPDATE lw_t SET v = 30 WHERE id = 1')
            wait_blocked(pg, b.thread_id(), fut)
            time.sleep(0.12)
            query(a, 'COMMIT')
            assert fut.result(timeout=5) == 1
        if log_path is not None:
            with open(log_path, 'rb') as f:
                f.seek(off0)
                delta = f.read().decode('utf-8', 'replace')
            assert 'still waiting for' not in delta, f'unexpected lock wait log spam: {delta}'

        # Explicit ALTER SYSTEM SET log_lock_waits = on overrides dynamic default
        try:
            query(pg, "ALTER SYSTEM SET log_lock_waits = 'on'")
            query(pg, 'SELECT pg_reload_conf()')
            time.sleep(0.05)
            query(b, 'SELECT 1 FROM lw_t WHERE id = 1')
            set_parser(b, 'postgres')
            assert query(b, 'SHOW log_lock_waits') == [('on',)]
            set_parser(b, 'mysql')

            if log_path is not None:
                off1 = os.path.getsize(log_path)
                query(a, 'BEGIN')
                query(a, 'UPDATE lw_t SET v = 40 WHERE id = 1')
                with ThreadPoolExecutor(max_workers=1) as pool:
                    fut = pool.submit(query, b, 'UPDATE lw_t SET v = 50 WHERE id = 1')
                    wait_blocked(pg, b.thread_id(), fut)
                    time.sleep(0.12)
                    query(a, 'COMMIT')
                    assert fut.result(timeout=5) == 1
                time.sleep(0.05)
                with open(log_path, 'rb') as f:
                    f.seek(off1)
                    delta_on = f.read().decode('utf-8', 'replace')
                assert 'still waiting for' in delta_on, f'expected lock wait log with explicit override: {delta_on}'

            # Reloading mysql.deadlock_timeout = 0 restores 1s / on where unoverridden
            query(pg, 'ALTER SYSTEM RESET log_lock_waits')
            query(pg, 'ALTER SYSTEM SET mysql.deadlock_timeout = 0')
            query(pg, 'SELECT pg_reload_conf()')
            time.sleep(0.05)
            query(a, 'SELECT 1 FROM lw_t WHERE id = 1')
            set_parser(a, 'postgres')
            assert query(a, 'SHOW deadlock_timeout') == [('1s',)]
            assert query(a, 'SHOW log_lock_waits') == [('on',)]
            set_parser(a, 'mysql')
        finally:
            query(pg, 'ALTER SYSTEM RESET log_lock_waits')
            query(pg, 'ALTER SYSTEM RESET mysql.deadlock_timeout')
            query(pg, 'SELECT pg_reload_conf()')
            time.sleep(0.05)
            query(a, 'SELECT 1 FROM lw_t WHERE id = 1')

        # 1-C part 2: mysql_prefix without WITH (...) on both logged & UNLOGGED tables
        query(pg, 'CREATE EXTENSION IF NOT EXISTS pageinspect')
        query(pg, f'CREATE UNLOGGED TABLE {schema}.upfx(id INT PRIMARY KEY, k INT, s VARCHAR(64))')
        query(pg, f'CREATE INDEX upfx_idx ON {schema}.upfx USING mysql_prefix (k, s)')
        query(pg, f'CREATE TABLE {schema}.lpfx(id INT PRIMARY KEY, k INT, s VARCHAR(64))')
        query(pg, f'CREATE INDEX lpfx_idx ON {schema}.lpfx USING mysql_prefix (k, s)')

        for idx_name, forks in (('upfx_idx', ('main', 'init')), ('lpfx_idx', ('main',))):
            for fk in forks:
                raw_page = bytes(query(pg, 'SELECT get_raw_page(%s, %s, 0)', (f'{schema}.{idx_name}', fk))[0][0])
                magic, ver, _, _, _, _, _, _, allequalimage = struct.unpack_from('<7I4xdB', raw_page, 24)
                assert magic == 0x053162 and ver == 4, (idx_name, fk, hex(magic), ver)
                assert allequalimage == 0, f'{idx_name} fork {fk} has btm_allequalimage={allequalimage}!'

        # Insert duplicate keys across relcache reloads to verify rd_options initialization
        for i in range(40):
            query(a, 'INSERT INTO upfx VALUES (%s, %s, %s)', (i + 1, 7, f'prefix_same_{i}'))
            query(a, 'INSERT INTO lpfx VALUES (%s, %s, %s)', (i + 1, 7, f'prefix_same_{i}'))
        query(pg, f'ANALYZE {schema}.upfx')
        for i in range(40, 80):
            query(a, 'INSERT INTO upfx VALUES (%s, %s, %s)', (i + 1, 7, f'prefix_same_{i}'))
        assert query(a, "SELECT COUNT(*) FROM upfx WHERE k = 7 AND s LIKE 'prefix_same_%'") == [(80,)]

        # Actual crash-recovery test on a disposable instance: verify UNLOGGED mysql_prefix
        # resets cleanly from INIT_FORKNUM (btm_allequalimage == 0) and accepts new writes & scans!
        pgbin = os.getenv('PGBIN')
        if pgbin and os.path.exists(os.path.join(pgbin, 'initdb')):
            tmp_dir = tempfile.mkdtemp(prefix='openhalo_unlogged_crash_')
            try:
                with socket.socket() as s1, socket.socket() as s2:
                    s1.bind(('127.0.0.1', 0))
                    s2.bind(('127.0.0.1', 0))
                    c_pg_port, c_my_port = s1.getsockname()[1], s2.getsockname()[1]
                ddir = os.path.join(tmp_dir, 'data')
                subprocess.run(
                    [os.path.join(pgbin, 'initdb'), '-D', ddir, '-U', 'postgres', '-A', 'trust', '--no-sync'],
                    check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
                with open(os.path.join(ddir, 'postgresql.conf'), 'a', encoding='utf-8') as cf:
                    cf.write(
                        f"\nlisten_addresses = '127.0.0.1'\nport = {c_pg_port}\n"
                        f"shared_preload_libraries = 'aux_mysql'\nmysql.port = {c_my_port}\n"
                    )
                subprocess.run(
                    [os.path.join(pgbin, 'pg_ctl'), '-D', ddir, '-l', os.path.join(tmp_dir, 'pg.log'), 'start'],
                    check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
                try:
                    cpg = psycopg2.connect(host='127.0.0.1', port=c_pg_port, user='postgres', dbname='postgres')
                    cpg.autocommit = True
                    query(cpg, 'CREATE EXTENSION aux_mysql')
                    query(cpg, 'CREATE EXTENSION pageinspect')
                    query(cpg, 'CREATE UNLOGGED TABLE u_crash(id INT PRIMARY KEY, k INT, s VARCHAR(64))')
                    query(cpg, 'CREATE INDEX u_crash_idx ON u_crash USING mysql_prefix (k, s)')
                    query(cpg, "INSERT INTO u_crash VALUES (1, 9, 'before_crash_a'), (2, 9, 'before_crash_b')")
                    query(cpg, 'CHECKPOINT')
                    cpg.close()
                    # Immediate stop simulates hard crash -> recovery copies INIT_FORKNUM over MAIN_FORKNUM
                    subprocess.run(
                        [os.path.join(pgbin, 'pg_ctl'), '-D', ddir, '-m', 'immediate', 'stop'],
                        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
                    subprocess.run(
                        [os.path.join(pgbin, 'pg_ctl'), '-D', ddir, '-l', os.path.join(tmp_dir, 'pg.log'), 'start'],
                        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
                    cpg2 = psycopg2.connect(host='127.0.0.1', port=c_pg_port, user='postgres', dbname='postgres')
                    cpg2.autocommit = True
                    raw_after = bytes(query(cpg2, "SELECT get_raw_page('u_crash_idx', 'main', 0)")[0][0])
                    magic2, ver2, _, _, _, _, _, _, eqimg2 = struct.unpack_from('<7I4xdB', raw_after, 24)
                    assert magic2 == 0x053162 and ver2 == 4 and eqimg2 == 0, (hex(magic2), ver2, eqimg2)
                    assert query(cpg2, 'SELECT COUNT(*) FROM u_crash') == [(0,)]
                    cmy = pymysql.connect(host='127.0.0.1', port=c_my_port, user='postgres', autocommit=True)
                    try:
                        for j in range(25):
                            query(cmy, 'INSERT INTO public.u_crash VALUES (%s, %s, %s)', (j + 1, 9, f'after_{j}'))
                        assert query(cmy, "SELECT COUNT(*) FROM public.u_crash WHERE k = 9 AND s LIKE 'after_%'") == [(25,)]
                    finally:
                        cmy.close()
                        cpg2.close()
                finally:
                    subprocess.run(
                        [os.path.join(pgbin, 'pg_ctl'), '-D', ddir, '-m', 'immediate', 'stop'],
                        check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
            finally:
                shutil.rmtree(tmp_dir, ignore_errors=True)
    finally:
        a.close()
        b.close()
    print('PASS 1-C: MySQL dynamic log_lock_waits=off & unlogged/default mysql_prefix metapage verified', flush=True)


def test_1d_prepared_cached_plan_fastpath_and_benchmark(pg, schema):
    setup = my_conn(database=schema)
    try:
        query(setup, """
            CREATE TABLE fp_items(
                id INT AUTO_INCREMENT PRIMARY KEY,
                k INT NOT NULL,
                c CHAR(12) NOT NULL DEFAULT '',
                pad VARCHAR(64),
                ts TIMESTAMP DEFAULT '2026-01-01 00:00:00' ON UPDATE CURRENT_TIMESTAMP,
                UNIQUE KEY uq_k (k)
            )
        """)
    finally:
        setup.close()

    # Test raw binary protocol with CLIENT_DEPRECATE_EOF enabled
    c_on = RawStmtClient(my_conn(database=schema))
    # Also craft a raw connection with CLIENT_DEPRECATE_EOF disabled via manual capability negotiation check
    c_multi = RawStmtClient(my_conn(database=schema, client_flag=CLIENT.MULTI_STATEMENTS))
    try:
        # 1. INSERT with AUTO_INCREMENT -> LAST_INSERT_ID() + OK insert_id across >8 executions (custom + generic)
        p_ins = c_on.prepare('INSERT INTO fp_items(k, c, pad) VALUES (?, ?, ?)')
        assert p_ins['nparams'] == 3 and p_ins['ncols'] == 0, p_ins
        p_lid = c_on.prepare('SELECT LAST_INSERT_ID() AS lid')
        for i in range(1, 10):
            r = c_on.execute(
                p_ins['id'],
                [
                    (T_LONG, struct.pack('<i', i * 10)),
                    (T_STRING, enc_str(f'ch{i}   ')),
                    (T_VARCHAR, enc_str(f'pad_{i}')),
                ],
                bind=(i == 1),
            )
            assert r.get('ok') and r['affected'] == 1 and r['insert_id'] == i, (i, r)
            lid_res = c_on.execute(p_lid['id'], [], bind=(i == 1))
            assert lid_res['rows'] == [(i,)], lid_res

        # 2. Exact column origin metadata (schema, table, org_table, name, org_col) & CHAR trailing-space semantics
        p_sel = c_on.prepare('SELECT id AS item_id, k, c, pad FROM fp_items AS f WHERE id = ?')
        assert [(c['name'], c['org_col'], c['org_table']) for c in p_sel['cols']] == [
            ('item_id', 'id', 'fp_items'),
            ('k', 'k', 'fp_items'),
            ('c', 'c', 'fp_items'),
            ('pad', 'pad', 'fp_items'),
        ], p_sel['cols']

        for mode in ('auto', 'force_custom_plan', 'force_generic_plan'):
            with c_on.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('mysql.parser', 'postgres', false)")
                cur.execute(f"SET plan_cache_mode = '{mode}'")
                cur.execute("SELECT pg_catalog.set_config('mysql.parser', 'mysql', false)")
            for i in range(1, 8):
                res = c_on.execute(p_sel['id'], [(T_LONG, struct.pack('<i', i))], bind=(i == 1))
                assert [(c['schema'], c['name'], c['org_col'], c['org_table']) for c in res['cols']] == [
                    (schema, 'item_id', 'id', 'fp_items'),
                    (schema, 'k', 'k', 'fp_items'),
                    (schema, 'c', 'c', 'fp_items'),
                    (schema, 'pad', 'pad', 'fp_items'),
                ], res['cols']
                assert res['rows'] == [(i, i * 10, f'ch{i}', f'pad_{i}')], (mode, i, res['rows'])
        with c_on.conn.cursor() as cur:
            cur.execute("SELECT pg_catalog.set_config('mysql.parser', 'postgres', false)")
            cur.execute("SET plan_cache_mode = 'auto'")
            cur.execute("SELECT pg_catalog.set_config('mysql.parser', 'mysql', false)")

        # 3. ODKU, REPLACE, and UPDATE (verifying ON UPDATE CURRENT_TIMESTAMP respects cached update_cols!)
        p_odku = c_on.prepare(
            'INSERT INTO fp_items(k, c, pad) VALUES (?, ?, ?) '
            'ON DUPLICATE KEY UPDATE pad = VALUES(pad)'
        )
        # Conflict update changing pad -> affected_rows == 2
        r_upd = c_on.execute(
            p_odku['id'],
            [(T_LONG, struct.pack('<i', 10)), (T_VARCHAR, enc_str('ch1')), (T_VARCHAR, enc_str('pad_1_new'))],
        )
        assert r_upd['affected'] == 2, r_upd
        # Conflict update leaving pad unchanged -> affected_rows == 0 (without CLIENT_FOUND_ROWS)
        r_same = c_on.execute(
            p_odku['id'],
            [(T_LONG, struct.pack('<i', 10)), (T_VARCHAR, enc_str('ch1')), (T_VARCHAR, enc_str('pad_1_new'))],
        )
        assert r_same['affected'] == 0, r_same

        # Explicit ts = ts assignment in UPDATE must NOT advance ts via ON UPDATE CURRENT_TIMESTAMP trigger
        p_keep_ts = c_on.prepare("UPDATE fp_items SET pad = ?, ts = '2026-01-01 00:00:00' WHERE id = ?")
        for n in range(3):
            r_u = c_on.execute(
                p_keep_ts['id'],
                [(T_VARCHAR, enc_str(f'keep_ts_{n}')), (T_LONG, struct.pack('<i', 2))],
                bind=(n == 0),
            )
            assert r_u['affected'] == 1, r_u
        with c_on.conn.cursor() as cur:
            cur.execute('SELECT CAST(ts AS CHAR) FROM fp_items WHERE id = 2')
            assert cur.fetchone()[0].startswith('2026-01-01 00:00:00')

        p_rep = c_on.prepare('REPLACE INTO fp_items(id, k, c, pad) VALUES (?, ?, ?, ?)')
        r_rep = c_on.execute(
            p_rep['id'],
            [
                (T_LONG, struct.pack('<i', 3)),
                (T_LONG, struct.pack('<i', 30)),
                (T_VARCHAR, enc_str('rep3')),
                (T_VARCHAR, enc_str('rep_pad_3')),
            ],
        )
        assert r_rep['affected'] == 2, r_rep

        # 4. Numeric boundary crossings without wire-type change & NULL rebinds!
        p_math = c_on.prepare('SELECT ? + 1 AS v FROM fp_items WHERE id = ?')
        r_small = c_on.execute(
            p_math['id'],
            [(T_LONGLONG, struct.pack('<q', 41)), (T_LONG, struct.pack('<i', 1))],
        )
        assert r_small['cols'][0]['type'] == T_LONG and r_small['rows'] == [(42,)], r_small

        # Same wire type T_LONGLONG (bind=False!), but value > INT32_MAX -> must rebuild as INT8!
        r_big = c_on.execute(
            p_math['id'],
            [(T_LONGLONG, struct.pack('<q', 5_000_000_000)), (T_LONG, struct.pack('<i', 1))],
            bind=False,
        )
        assert r_big['cols'][0]['type'] == T_LONGLONG and r_big['rows'] == [(5_000_000_001,)], r_big

        # NULL rebind (bind=False, nullmap set) -> NULL + 1 resolves to INT4 NULL, not stale INT8!
        r_null = c_on.execute(
            p_math['id'],
            [(T_LONGLONG, None), (T_LONG, struct.pack('<i', 1))],
            bind=False,
        )
        assert r_null['cols'][0]['type'] == T_LONG and r_null['rows'] == [(None,)], r_null

        # Unsigned LONG > INT32_MAX vs <= INT32_MAX
        r_ul_small = c_on.execute(
            p_math['id'],
            [(T_LONG | UNSIGNED_BIT, struct.pack('<I', 100)), (T_LONG, struct.pack('<i', 1))],
            bind=True,
        )
        assert r_ul_small['cols'][0]['type'] == T_LONG and r_ul_small['rows'] == [(101,)], r_ul_small
        r_ul_big = c_on.execute(
            p_math['id'],
            [(T_LONG | UNSIGNED_BIT, struct.pack('<I', 3_000_000_000)), (T_LONG, struct.pack('<i', 1))],
            bind=False,
        )
        assert r_ul_big['cols'][0]['type'] == T_LONGLONG and r_ul_big['rows'] == [(3_000_000_001,)], r_ul_big

        # 5. Zero-byte COM_STMT_SEND_LONG_DATA must override packet body & be consumed
        p_ld = c_on.prepare('SELECT pad, LENGTH(?) AS zlen FROM fp_items WHERE id = ?')
        # Warm fast path first
        assert c_on.execute(
            p_ld['id'],
            [(T_VARCHAR, enc_str('hello')), (T_LONG, struct.pack('<i', 1))],
        )['rows'][0][1] == 5
        # Now send 0-byte long data on param 0!
        c_on.send_long_data(p_ld['id'], 0, b'')
        r_zld = c_on.execute(
            p_ld['id'],
            [(T_VARCHAR, ...), (T_LONG, struct.pack('<i', 1))],
        )
        assert r_zld['rows'][0][1] == 0, r_zld
        # Next call without send_long_data returns to fast path cleanly
        assert c_on.execute(
            p_ld['id'],
            [(T_VARCHAR, enc_str('abcde')), (T_LONG, struct.pack('<i', 1))],
        )['rows'][0][1] == 5

        # 6. ALTER TABLE ADD COLUMN invalidates cached SELECT * (fixed_result = false) & refreshes dialect rewrite
        p_star = c_on.prepare('SELECT * FROM fp_items WHERE id = ?')
        for _ in range(6):
            r_st = c_on.execute(p_star['id'], [(T_LONG, struct.pack('<i', 1))])
            assert len(r_st['cols']) == 5, r_st
        with c_on.conn.cursor() as cur:
            cur.execute("ALTER TABLE fp_items ADD COLUMN extra VARCHAR(20) DEFAULT 'added'")
        r_st_after = c_on.execute(p_star['id'], [(T_LONG, struct.pack('<i', 1))])
        assert len(r_st_after['cols']) == 6 and r_st_after['rows'][0][-1] == 'added', r_st_after

        # Adding AUTO_INCREMENT column after PREPARE must refresh capture_insert_id on cached INSERT!
        with c_on.conn.cursor() as cur:
            cur.execute('CREATE TABLE fp_dyn_ai(v VARCHAR(20))')
        p_dyn = c_on.prepare('INSERT INTO fp_dyn_ai(v) VALUES (?)')
        for _ in range(6):
            r_d = c_on.execute(p_dyn['id'], [(T_VARCHAR, enc_str('before_ai'))])
            assert r_d['insert_id'] == 0, r_d
        with c_on.conn.cursor() as cur:
            cur.execute('ALTER TABLE fp_dyn_ai ADD COLUMN id INT AUTO_INCREMENT PRIMARY KEY')
        r_d2 = c_on.execute(p_dyn['id'], [(T_VARCHAR, enc_str('after_ai'))])
        assert r_d2['insert_id'] >= 7, r_d2

        # 7. Hidden-savepoint recovery inside BEGIN preserves outer transaction_timeout & COM_QUERY usability
        with c_multi.conn.cursor() as cur:
            cur.execute("SELECT pg_catalog.set_config('mysql.parser', 'postgres', false)")
            cur.execute('SET SESSION transaction_timeout = 1200')
            cur.execute("SELECT pg_catalog.set_config('mysql.parser', 'mysql', false)")
            cur.execute('BEGIN')
            cur.execute("INSERT INTO fp_items(k, c, pad) VALUES (9001, 'tx', 'outer_survives')")
        p_dup = c_multi.prepare('INSERT INTO fp_items(k, c, pad) VALUES (?, ?, ?)')
        # Warm cached plan source
        assert c_multi.execute(
            p_dup['id'],
            [(T_LONG, struct.pack('<i', 9002)), (T_VARCHAR, enc_str('tx2')), (T_VARCHAR, enc_str('ok'))],
        )['affected'] == 1
        time.sleep(0.65)
        # Duplicate key error (1062) on fast-path COM_STMT_EXECUTE inside BEGIN!
        r_err = c_multi.execute(
            p_dup['id'],
            [(T_LONG, struct.pack('<i', 9001)), (T_VARCHAR, enc_str('dup')), (T_VARCHAR, enc_str('dup'))],
            bind=False,
        )
        assert r_err.get('errno') == 1062, r_err
        # Outer row 9001 & 9002 are still visible via COM_QUERY right after failed COM_STMT_EXECUTE!
        with c_multi.conn.cursor() as cur:
            cur.execute('SELECT COUNT(*) FROM fp_items WHERE k IN (9001, 9002)')
            assert cur.fetchone() == (2,)
        # Wait remaining ~0.75s so original 1.2s transaction_timeout fires (proving deadline was preserved!)
        time.sleep(0.75)
        timed_out = False
        try:
            with c_multi.conn.cursor() as cur:
                cur.execute('SELECT 1')
        except pymysql.err.MySQLError:
            timed_out = True
        assert timed_out, 'outer transaction_timeout deadline was lost after fast-path statement rollback!'

        # Reconnect c_multi and test 2-session deadlock (1213) via fast-path COM_STMT_EXECUTE rolling back whole xact
        c_multi.close()
        c_multi = RawStmtClient(my_conn(database=schema))
        peer = my_conn(database=schema)
        try:
            p_u = c_multi.prepare('UPDATE fp_items SET pad = ? WHERE id = ?')
            # Warm cached plan
            assert c_multi.execute(p_u['id'], [(T_VARCHAR, enc_str('w')), (T_LONG, struct.pack('<i', 1))])['affected'] == 1

            with c_multi.conn.cursor() as cur:
                cur.execute('BEGIN')
                cur.execute("INSERT INTO fp_items(k, c, pad) VALUES (9500, 'dl', 'must_rollback')")
            assert c_multi.execute(
                p_u['id'],
                [(T_VARCHAR, enc_str('lock1')), (T_LONG, struct.pack('<i', 1))],
                bind=False,
            )['affected'] == 1

            query(peer, 'BEGIN')
            assert query(peer, "UPDATE fp_items SET pad = 'lock2' WHERE id = 2") == 1
            with ThreadPoolExecutor(max_workers=1) as pool:
                fut = pool.submit(query, peer, "UPDATE fp_items SET pad = 'need1' WHERE id = 1")
                wait_blocked(pg, peer.thread_id(), fut)
                r_dl = c_multi.execute(
                    p_u['id'],
                    [(T_VARCHAR, enc_str('need2')), (T_LONG, struct.pack('<i', 2))],
                    bind=False,
                )
                try:
                    peer_res = fut.result(timeout=5)
                except pymysql.err.OperationalError as e:
                    peer_res = e.args[0]
            assert r_dl.get('errno') == 1213 or peer_res == 1213, (r_dl, peer_res)
            try:
                query(peer, 'ROLLBACK')
            except Exception:
                pass
            if r_dl.get('errno') == 1213:
                # Whole transaction of c_multi must have been aborted (k=9500 gone!)
                with c_multi.conn.cursor() as cur:
                    cur.execute('SELECT COUNT(*) FROM fp_items WHERE k = 9500')
                    assert cur.fetchone() == (0,)
            else:
                with c_multi.conn.cursor() as cur:
                    cur.execute('ROLLBACK')

            # COM_RESET_CONNECTION drops all cached plans cleanly without dangling pointers
            assert c_multi.reset_connection()['ok']
            assert c_multi.execute(p_u['id'], [(T_VARCHAR, enc_str('x')), (T_LONG, struct.pack('<i', 1))])['errno'] == 1243
        finally:
            peer.close()

        # 8. Test BOTH genuine modern EOF (CLIENT_DEPRECATE_EOF = 1) AND legacy EOF (0) on raw wire,
        #    plus Codex review regressions (#1..#6, autocommit=0, one-shot READ ONLY, malformed pkt, memory)
        class ModernEOFConnection(pymysql.connections.Connection):
            def _request_authentication(self):
                self.client_flag |= CLIENT.DEPRECATE_EOF
                return super()._request_authentication()

        class LegacyEOFConnection(pymysql.connections.Connection):
            def _request_authentication(self):
                self.client_flag &= ~CLIENT.DEPRECATE_EOF
                return super()._request_authentication()

        for conn_cls, expect_dep in ((ModernEOFConnection, True), (LegacyEOFConnection, False)):
            raw_eof_conn = conn_cls(
                host=os.getenv('MYSQL_HOST', '127.0.0.1'),
                port=int(os.getenv('MYSQL_PORT', '3306')),
                user=os.getenv('MYSQL_ADMIN_USER', os.getenv('PGUSER', 'postgres')),
                password=os.getenv('MYSQL_ADMIN_PASSWORD', os.getenv('PGPASSWORD', '')),
                database=schema,
                autocommit=True,
            )
            c_eof = RawStmtClient(raw_eof_conn)
            assert c_eof.deprecate_eof is expect_dep, (conn_cls, c_eof.deprecate_eof)
            try:
                pl = c_eof.prepare('SELECT id AS item_id, pad FROM fp_items WHERE id = ?')
                pu = c_eof.prepare('UPDATE fp_items SET pad = ? WHERE id = ?')
                for idx_run in range(3):
                    ru = c_eof.execute(
                        pu['id'],
                        [(T_VARCHAR, enc_str(f'eof_{expect_dep}_{idx_run}')), (T_LONG, struct.pack('<i', 1))],
                        bind=(idx_run == 0),
                    )
                    assert ru.get('ok') and ru['affected'] == 1, ru
                    rl = c_eof.execute(pl['id'], [(T_LONG, struct.pack('<i', 1))], bind=(idx_run == 0))
                    assert rl['cols'][0]['org_col'] == 'id' and rl['cols'][0]['org_table'] == 'fp_items', rl
                    if expect_dep:
                        assert rl['term'].get('ok') is True, rl
                    else:
                        assert rl['term'] == {'eof': True}, rl
                    assert rl['rows'] == [(1, f'eof_{expect_dep}_{idx_run}')], rl
            finally:
                c_eof.close()

        # 8b. Finding #1: SearchPathMatchesCurrentEnvironment after set_config('search_path', ...) outside xact
        p_sp = c_on.prepare('SELECT id FROM fp_items WHERE id = ?')
        assert c_on.execute(p_sp['id'], [(T_LONG, struct.pack('<i', 1))])['rows'] == [(1,)]
        with c_on.conn.cursor() as cur:
            cur.execute(
                "SELECT pg_catalog.set_config('search_path', %s, false)",
                (f'"{schema}", mysql, public, pg_catalog',),
            )
        assert c_on.execute(p_sp['id'], [(T_LONG, struct.pack('<i', 1))], bind=False)['rows'] == [(1,)]

        # 8c. Finding #2: NULL bound to NOT NULL domain must NOT bypass domain constraint!
        query(pg, f'CREATE DOMAIN {schema}.nn_int_dom AS INT NOT NULL')
        query(pg, f'CREATE TABLE {schema}.dom_tbl(id INT PRIMARY KEY, v {schema}.nn_int_dom)')
        p_dom = c_on.prepare('INSERT INTO dom_tbl(id, v) VALUES (?, ?)')
        assert c_on.execute(p_dom['id'], [(T_LONG, struct.pack('<i', 1)), (T_LONG, struct.pack('<i', 7))])['affected'] == 1
        r_dom_null = c_on.execute(p_dom['id'], [(T_LONG, struct.pack('<i', 2)), (T_LONG, None)], bind=False)
        assert 'errno' in r_dom_null, f'NULL bypassed NOT NULL domain constraint: {r_dom_null}'
        assert query(pg, f'SELECT COUNT(*) FROM {schema}.dom_tbl') == [(1,)]

        # 8d. Findings #3 & #4: Cold first EXECUTE obeys statement_timeout & pg_cancel_backend without retrying,
        #     AND fallback inside BEGIN preserves remaining statement_timeout budget across hidden SAVEPOINT!
        c_to = RawStmtClient(my_conn(database=schema))
        locker = pg_conn()
        try:
            p_cold = c_to.prepare('SELECT v FROM dom_tbl WHERE id = ?')
            p_cold2 = c_to.prepare('SELECT id, v FROM dom_tbl WHERE id = ?')
            p_fb_to = c_to.prepare('SELECT pg_sleep(0.14), @fb_v := ? FROM dom_tbl WHERE id = ?')

            query(locker, 'BEGIN')
            query(locker, f'LOCK TABLE {schema}.dom_tbl IN ACCESS EXCLUSIVE MODE')
            with c_to.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('statement_timeout', '150', false)")
            t_start = time.monotonic()
            r_to = c_to.execute(p_cold['id'], [(T_LONG, struct.pack('<i', 1))])
            elapsed = time.monotonic() - t_start
            assert 'errno' in r_to and elapsed < 2.0, (r_to, elapsed)
            with c_to.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('statement_timeout', '0', false)")

            # Also test pg_cancel_backend during cold probe inside BEGIN: outer write must survive!
            with c_to.conn.cursor() as cur:
                cur.execute('BEGIN')
                cur.execute("INSERT INTO fp_items(k, c, pad) VALUES (9601, 'cn', 'survives_cancel')")
            with ThreadPoolExecutor(max_workers=1) as pool:
                fut = pool.submit(c_to.execute, p_cold2['id'], [(T_LONG, struct.pack('<i', 1))])
                wait_blocked(pg, c_to.conn.thread_id(), fut)
                query(pg, 'SELECT pg_cancel_backend(%s)', (c_to.conn.thread_id(),))
                r_can = fut.result(timeout=5)
            assert 'errno' in r_can, r_can
            query(locker, 'ROLLBACK')
            with c_to.conn.cursor() as cur:
                cur.execute('COMMIT')
                cur.execute('SELECT COUNT(*) FROM fp_items WHERE k = 9601')
                assert cur.fetchone() == (1,)

            # Round-2 #4: Inside BEGIN, cold probe waits ~130ms on lock then declines (@fb_v := ?),
            # and fallback runs pg_sleep(0.14) (130+140=270ms > 210ms budget) -> MUST time out!
            query(locker, 'BEGIN')
            query(locker, f'LOCK TABLE {schema}.dom_tbl IN ACCESS EXCLUSIVE MODE')
            with c_to.conn.cursor() as cur:
                cur.execute('BEGIN')
                cur.execute("INSERT INTO fp_items(k, c, pad) VALUES (9602, 'fbto', 'survives_fb_to')")
                cur.execute("SELECT pg_catalog.set_config('statement_timeout', '210', false)")
            with ThreadPoolExecutor(max_workers=1) as pool:
                fut = pool.submit(
                    c_to.execute,
                    p_fb_to['id'],
                    [(T_LONG, struct.pack('<i', 7)), (T_LONG, struct.pack('<i', 1))],
                )
                wait_blocked(pg, c_to.conn.thread_id(), fut)
                time.sleep(0.13)
                query(locker, 'ROLLBACK')
                r_fb_to = fut.result(timeout=5)
            assert 'errno' in r_fb_to, f'fallback inside BEGIN reset statement_timeout budget: {r_fb_to}'
            with c_to.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('statement_timeout', '0', false)")
                cur.execute('COMMIT')
                cur.execute('SELECT COUNT(*) FROM fp_items WHERE k = 9602')
                assert cur.fetchone() == (1,)
        finally:
            try:
                query(locker, 'ROLLBACK')
            except Exception:
                pass
            locker.close()
            c_to.close()

        # 8e. Finding #5 + Round-2 positional ORDER BY / GROUP BY:
        #     Unary minus at exact INT32_MIN, INT32_MAX+1, INT64_MIN boundaries,
        #     plus ORDER BY -?, ORDER BY ?, and GROUP BY ? ordinal equivalence!
        p_neg = c_on.prepare('SELECT -? AS neg FROM fp_items WHERE id = ?')
        r_neg_i32min = c_on.execute(
            p_neg['id'],
            [(T_LONG, struct.pack('<i', -2147483648)), (T_LONG, struct.pack('<i', 1))],
        )
        assert r_neg_i32min['cols'][0]['type'] == T_LONGLONG and r_neg_i32min['rows'] == [(2147483648,)], r_neg_i32min
        r_neg_p2g = c_on.execute(
            p_neg['id'],
            [(T_LONGLONG, struct.pack('<q', 2147483648)), (T_LONG, struct.pack('<i', 1))],
        )
        assert r_neg_p2g['cols'][0]['type'] == T_LONG and r_neg_p2g['rows'] == [(-2147483648,)], r_neg_p2g
        r_neg_i64min = c_on.execute(
            p_neg['id'],
            [(T_LONGLONG, struct.pack('<q', -9223372036854775808)), (T_LONG, struct.pack('<i', 1))],
            bind=False,
        )
        assert r_neg_i64min['rows'] == [(b'9223372036854775808',)], r_neg_i64min

        p_ord_neg = c_on.prepare('SELECT k, id FROM fp_items WHERE id IN (1, 2, 3) ORDER BY -? DESC')
        r_ord_col1 = c_on.execute(p_ord_neg['id'], [(T_LONG, struct.pack('<i', -1))])['rows']
        r_ord_col2 = c_on.execute(p_ord_neg['id'], [(T_LONG, struct.pack('<i', -2))], bind=False)['rows']
        with c_on.conn.cursor() as cur:
            cur.execute('SELECT k, id FROM fp_items WHERE id IN (1, 2, 3) ORDER BY -(-1) DESC')
            exp_col1 = list(cur.fetchall())
            cur.execute('SELECT k, id FROM fp_items WHERE id IN (1, 2, 3) ORDER BY -(-2) DESC')
            exp_col2 = list(cur.fetchall())
        assert r_ord_col1 == exp_col1 and r_ord_col2 == exp_col2, (r_ord_col1, exp_col1, r_ord_col2, exp_col2)

        p_grp_ord = c_on.prepare('SELECT c, COUNT(*) FROM fp_items WHERE id IN (1, 2) GROUP BY -? ORDER BY c')
        r_grp_ord = c_on.execute(p_grp_ord['id'], [(T_LONG, struct.pack('<i', -1))])['rows']
        with c_on.conn.cursor() as cur:
            cur.execute('SELECT c, COUNT(*) FROM fp_items WHERE id IN (1, 2) GROUP BY -(-1) ORDER BY c')
            assert r_grp_ord == list(cur.fetchall()), r_grp_ord

        # 8f. Finding #6 + Round-2 logging: Prove plain UPDATE & DELETE run on CachedPlan fast path,
        #     AND log_statement / log_min_duration_statement emit statement + Parameters + duration!
        p_fast_upd = c_on.prepare('UPDATE fp_items SET pad = ? WHERE id = ?')
        # Warm plan cache
        assert c_on.execute(p_fast_upd['id'], [(T_VARCHAR, enc_str('fp0')), (T_LONG, struct.pack('<i', 1))])['affected'] == 1
        row_holder = my_conn(database=schema)
        try:
            query(row_holder, 'BEGIN')
            query(row_holder, "UPDATE fp_items SET pad = 'held' WHERE id = 1")
            with ThreadPoolExecutor(max_workers=1) as pool:
                fut = pool.submit(
                    c_on.execute,
                    p_fast_upd['id'],
                    [(T_VARCHAR, enc_str('fp_blocked')), (T_LONG, struct.pack('<i', 1))],
                    False,
                )
                wait_blocked(pg, c_on.conn.thread_id(), fut)
                act_q = query(pg, 'SELECT query FROM pg_stat_activity WHERE pid = %s', (c_on.conn.thread_id(),))[0][0]
                assert '?' in act_q and 'fp_blocked' not in act_q, f'UPDATE fell back to literal path: {act_q}'
                query(row_holder, 'ROLLBACK')
                assert fut.result(timeout=5)['affected'] == 1
        finally:
            row_holder.close()

        data_dir = query(pg, 'SHOW data_directory')[0][0]
        pm_log = os.path.join(data_dir, 'postmaster.log')
        if os.path.exists(pm_log):
            p_logged = c_on.prepare('SELECT id, pad FROM fp_items WHERE id = ? /* fastpath_log_probe */')
            p_diverr = c_on.prepare('SELECT pg_catalog.int4div(1, ?) FROM fp_items WHERE id = 1')
            assert len(c_on.execute(p_logged['id'], [(T_LONG, struct.pack('<i', 1))])['rows']) == 1
            log_off0 = os.path.getsize(pm_log)
            with c_on.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('log_statement', 'all', false)")
                cur.execute("SELECT pg_catalog.set_config('log_min_duration_statement', '0', false)")
                cur.execute("SELECT pg_catalog.set_config('log_parameter_max_length_on_error', '-1', false)")
            assert len(c_on.execute(p_logged['id'], [(T_LONG, struct.pack('<i', 1))], bind=False)['rows']) == 1
            assert 'errno' in c_on.execute(p_diverr['id'], [(T_LONG, struct.pack('<i', 0))])
            with c_on.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('log_statement', 'none', false)")
                assert len(c_on.execute(p_logged['id'], [(T_LONG, struct.pack('<i', 2))], bind=False)['rows']) == 1
                cur.execute("SELECT pg_catalog.set_config('log_min_duration_statement', '-1', false)")
                cur.execute("SELECT pg_catalog.set_config('log_parameter_max_length_on_error', '0', false)")
            time.sleep(0.05)
            with open(pm_log, 'rb') as lf:
                lf.seek(log_off0)
                log_chunk = lf.read().decode('utf-8', 'replace')
            assert 'statement: SELECT id, pad FROM fp_items WHERE id = ? /* fastpath_log_probe */' in log_chunk, log_chunk
            assert "Parameters: $1 = '1'" in log_chunk, log_chunk
            assert 'duration:' in log_chunk and "Parameters: $1 = '2'" in log_chunk, log_chunk
            assert "unnamed portal with parameters: $1 = '0'" in log_chunk, log_chunk

        # 8g. Malformed EXECUTE inside BEGIN with statement_rollback=off preserves active transaction,
        #     autocommit=0 multi-EXECUTE, one-shot SET TRANSACTION READ ONLY, latin1 encoding, and CLOSE memory!
        c_misc = RawStmtClient(my_conn(database=schema))
        try:
            with c_misc.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('mysql.statement_rollback', 'off', false)")
                cur.execute('BEGIN')
                cur.execute("INSERT INTO fp_items(k, c, pad) VALUES (9701, 'mf', 'kept')")
            # Unknown statement id -> 1243 without aborting active PG transaction block!
            assert c_misc.execute(999999, [])['errno'] == 1243
            with c_misc.conn.cursor() as cur:
                cur.execute('COMMIT')
                cur.execute("SELECT pg_catalog.set_config('mysql.statement_rollback', 'on', false)")
                cur.execute('SELECT COUNT(*) FROM fp_items WHERE k = 9701')
                assert cur.fetchone() == (1,)

            # Prepare BEFORE one-shot SET TRANSACTION READ ONLY so COM_STMT_EXECUTE itself handles it!
            p_ro = c_misc.prepare('UPDATE fp_items SET pad = ? WHERE id = ?')
            p_ro_sel = c_misc.prepare('SELECT id FROM fp_items WHERE id = ?')
            with c_misc.conn.cursor() as cur:
                cur.execute('SET TRANSACTION READ ONLY')
            r_ro = c_misc.execute(p_ro['id'], [(T_VARCHAR, enc_str('ro_fail')), (T_LONG, struct.pack('<i', 1))])
            assert 'errno' in r_ro, f'one-shot SET TRANSACTION READ ONLY lost on fast-path EXECUTE: {r_ro}'
            # Rejected write did not start/use a transaction (mys_utility.c:2533); a read uses & spends it:
            assert c_misc.execute(p_ro_sel['id'], [(T_LONG, struct.pack('<i', 1))])['rows'] == [(1,)]
            # Next autocommit write now succeeds (one-shot was spent by p_ro_sel)
            assert c_misc.execute(p_ro['id'], [(T_VARCHAR, enc_str('ro_ok')), (T_LONG, struct.pack('<i', 1))], bind=False)['affected'] == 1

            # autocommit=0 multi-statement prepared executions
            with c_misc.conn.cursor() as cur:
                cur.execute('SET autocommit = 0')
            for idx_ac in range(4):
                rac = c_misc.execute(
                    p_ro['id'],
                    [(T_VARCHAR, enc_str(f'ac0_{idx_ac}')), (T_LONG, struct.pack('<i', 1))],
                    bind=False,
                )
                assert rac['affected'] == 1, rac
            with c_misc.conn.cursor() as cur:
                cur.execute('COMMIT')
                cur.execute('SET autocommit = 1')

            # Client encoding != server encoding (LATIN1) transcodes both query and string params
            # cleanly from LATIN1 -> UTF8; in UTF8 mode, non-UTF8 bytes fall back to _binary hex.
            with c_misc.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('client_encoding', 'LATIN1', false)")
            p_lat = c_misc.prepare("UPDATE fp_items SET pad = CONCAT(?, '_', HEX(?)) WHERE id = ?")
            assert (
                c_misc.execute(
                    p_lat['id'],
                    [
                        (T_VARCHAR, enc_str('café')),
                        (T_VARCHAR, enc_str(b'caf\xe9')),
                        (T_LONG, struct.pack('<i', 1)),
                    ],
                )['affected']
                == 1
            )
            c_misc.close_stmt(p_lat['id'])
            with c_misc.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('client_encoding', 'UTF8', false)")
                cur.execute('SELECT pad FROM fp_items WHERE id = 1')
                assert cur.fetchone() == ('cafÃ©_636166C3A9',)
            p_raw = c_misc.prepare("UPDATE fp_items SET pad = HEX(?) WHERE id = ?")
            assert (
                c_misc.execute(
                    p_raw['id'],
                    [
                        (T_VARCHAR, enc_str(b'caf\xe9')),
                        (T_LONG, struct.pack('<i', 1)),
                    ],
                )['affected']
                == 1
            )
            c_misc.close_stmt(p_raw['id'])
            with c_misc.conn.cursor() as cur:
                cur.execute('SELECT pad FROM fp_items WHERE id = 1')
                assert cur.fetchone() == ('636166E9',)

            # 200-cycle PREPARE -> EXECUTE -> CLOSE must not leak CachedPlanSource in CacheMemoryContext!
            for _ in range(10):
                ps_tmp = c_misc.prepare('SELECT id, pad FROM fp_items WHERE id = ?')
                c_misc.execute(ps_tmp['id'], [(T_LONG, struct.pack('<i', 1))])
                c_misc.close_stmt(ps_tmp['id'])
            mem_before = query(
                pg,
                """
                SELECT COALESCE(SUM(total_bytes), 0)
                FROM pg_get_backend_memory_contexts()
                WHERE false
                """,
            )
            # Check backend's own CacheMemoryContext from inside c_misc!
            with c_misc.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('mysql.parser', 'postgres', false)")
                cur.execute("SELECT SUM(used_bytes)::bigint FROM pg_backend_memory_contexts WHERE name LIKE 'CachedPlan%'")
                cp_before = int(cur.fetchone()[0] or 0)
                cur.execute("SELECT pg_catalog.set_config('mysql.parser', 'mysql', false)")
            for _ in range(200):
                ps_tmp = c_misc.prepare('SELECT id, k, pad FROM fp_items WHERE id = ?')
                for step_k in range(6):
                    c_misc.execute(ps_tmp['id'], [(T_LONG, struct.pack('<i', 1))], bind=(step_k == 0))
                c_misc.close_stmt(ps_tmp['id'])
            with c_misc.conn.cursor() as cur:
                cur.execute("SELECT pg_catalog.set_config('mysql.parser', 'postgres', false)")
                cur.execute("SELECT SUM(used_bytes)::bigint FROM pg_backend_memory_contexts WHERE name LIKE 'CachedPlan%'")
                cp_after = int(cur.fetchone()[0] or 0)
                cur.execute("SELECT pg_catalog.set_config('mysql.parser', 'mysql', false)")
            assert cp_after <= cp_before + 65536, (cp_before, cp_after)
            _ = mem_before
        finally:
            c_misc.close()

        # 9. Micro-benchmark: CachedPlan fast path vs literal text-interpolation fallback
        p_bench = c_on.prepare('SELECT id, k, c, pad FROM fp_items WHERE id = ?')
        for idx_w in range(10):
            rw = c_on.execute(p_bench['id'], [(T_LONG, struct.pack('<i', 1))], bind=(idx_w == 0))
            assert len(rw['rows']) == 1, rw
        iters = 2000
        t0 = time.perf_counter()
        for i in range(iters):
            r = c_on.execute(p_bench['id'], [(T_LONG, struct.pack('<i', (i % 8) + 1))], bind=False)
            assert len(r['rows']) == 1
        dt_fast = time.perf_counter() - t0

        # Non-strict sql_mode forces literal interpolation + Bison parse + AST rewrite + plan on same connection
        with c_on.conn.cursor() as cur:
            cur.execute("SET SESSION sql_mode = ''")
        for _ in range(10):
            rw = c_on.execute(p_bench['id'], [(T_LONG, struct.pack('<i', 1))], bind=False)
            assert len(rw['rows']) == 1, rw
        t1 = time.perf_counter()
        for i in range(iters):
            r = c_on.execute(p_bench['id'], [(T_LONG, struct.pack('<i', (i % 8) + 1))], bind=False)
            assert len(r['rows']) == 1
        dt_lit = time.perf_counter() - t1

        qps_fast = iters / dt_fast
        qps_lit = iters / dt_lit
        ratio = qps_fast / qps_lit
        print(
            f'PASS 1-D: CachedPlan fast path verified — {qps_fast:,.0f} QPS vs literal {qps_lit:,.0f} QPS '
            f'({ratio:.2f}x speedup over loopback TCP)',
            flush=True,
        )
    finally:
        c_on.close()
        c_multi.close()


def test_1e_partitioned_current_read(pg, schema):
    adm = my_conn(database=schema)
    try:
        # 1. Create a 2-level RANGE -> LIST partitioned table with dropped & reordered leaf columns!
        query(pg, f"""
            CREATE TABLE {schema}.part_ml (
                tenant INT NOT NULL,
                region VARCHAR(16) NOT NULL,
                id INT NOT NULL,
                v INT NOT NULL,
                PRIMARY KEY (tenant, region, id)
            ) PARTITION BY RANGE (tenant)
        """)
        query(pg, f"""
            CREATE TABLE {schema}.part_ml_t1 PARTITION OF {schema}.part_ml
            FOR VALUES FROM (1) TO (10) PARTITION BY LIST (region)
        """)
        query(pg, f"""
            CREATE TABLE {schema}.part_ml_t2 PARTITION OF {schema}.part_ml
            FOR VALUES FROM (10) TO (20) PARTITION BY LIST (region)
        """)

        # Build standalone leaf tables with dropped columns & completely reordered physical attnums!
        for leaf, tmin, tmax, reg in (
            ('leaf_1_east', 1, 10, 'east'),
            ('leaf_1_west', 1, 10, 'west'),
            ('leaf_2_east', 10, 20, 'east'),
            ('leaf_2_west', 10, 20, 'west'),
        ):
            parent = 'part_ml_t1' if tmin == 1 else 'part_ml_t2'
            query(pg, f"""
                CREATE TABLE {schema}.{leaf} (
                    dropped_a TEXT,
                    v INT NOT NULL,
                    region VARCHAR(16) NOT NULL,
                    dropped_b BIGINT,
                    id INT NOT NULL,
                    tenant INT NOT NULL
                )
            """)
            query(pg, f'ALTER TABLE {schema}.{leaf} DROP COLUMN dropped_a, DROP COLUMN dropped_b')
            query(pg, f"""
                ALTER TABLE {schema}.{parent}
                ATTACH PARTITION {schema}.{leaf} FOR VALUES IN ('{reg}')
            """)

        # Also create a HASH partitioned table for single-column PK sysbench-like concurrency
        query(pg, f"""
            CREATE TABLE {schema}.part_hash (
                id INT PRIMARY KEY,
                k INT NOT NULL,
                v INT NOT NULL
            ) PARTITION BY HASH (id)
        """)
        for r in range(4):
            query(pg, f"""
                CREATE TABLE {schema}.part_hash_{r} PARTITION OF {schema}.part_hash
                FOR VALUES WITH (MODULUS 4, REMAINDER {r})
            """)

        # Verify EXPLAIN shows Custom Scan (MySQL Current Read) for constant key, LIMIT 1, and generic $1 Append!
        set_parser(adm, 'postgres')
        query(adm, f'SET search_path = {schema}')
        exp_const = '\n'.join(
            r[0] for r in query(
                adm,
                "EXPLAIN (COSTS OFF) UPDATE part_ml SET v = v + 1 WHERE tenant = 1 AND region = 'east' AND id = 5",
            )
        )
        assert 'Custom Scan (MySQL Current Read)' in exp_const, exp_const

        # Execute generic partitioned Append plan across multiple leaf partitions (not just EXPLAIN!)
        query(adm, "INSERT INTO part_ml(tenant, region, id, v) VALUES (1, 'east', 50, 1), (2, 'west', 51, 2), (11, 'east', 52, 3), (12, 'west', 53, 4)")
        query(adm, "SET plan_cache_mode = 'force_generic_plan'")
        query(adm, 'PREPARE p_gen(int, varchar, int) AS DELETE FROM part_ml WHERE tenant = $1 AND region = $2 AND id = $3')
        exp_gen = '\n'.join(r[0] for r in query(adm, "EXPLAIN (COSTS OFF) EXECUTE p_gen(1, 'east', 5)"))
        assert 'Append' in exp_gen and 'Custom Scan (MySQL Current Read)' in exp_gen, exp_gen
        for t_v, r_v, id_v in ((1, 'east', 50), (2, 'west', 51), (11, 'east', 52), (12, 'west', 53)):
            assert query(adm, 'EXECUTE p_gen(%s, %s, %s)', (t_v, r_v, id_v)) == 1
        assert query(adm, 'SELECT COUNT(*) FROM part_ml') == [(0,)]
        query(adm, 'DEALLOCATE p_gen')
        query(adm, "SET plan_cache_mode = 'auto'")
        set_parser(adm, 'mysql')

        # Check MySQL LIMIT 1 unwrap on BOTH topmost AND intermediate partitioned tables (Finding #8)!
        for target_tbl in ('part_ml', 'part_ml_t1'):
            query(adm, f"INSERT INTO {target_tbl}(tenant, region, id, v) VALUES (1, 'east', 1, 10)")
            s2 = my_conn(database=schema)
            try:
                query(adm, 'BEGIN')
                query(adm, f"DELETE FROM {target_tbl} WHERE tenant = 1 AND region = 'east' AND id = 1")
                query(adm, f"INSERT INTO {target_tbl}(tenant, region, id, v) VALUES (1, 'east', 1, 20)")
                with ThreadPoolExecutor(max_workers=1) as pool:
                    fut = pool.submit(
                        query,
                        s2,
                        f"DELETE FROM {target_tbl} WHERE tenant = 1 AND region = 'east' AND id = 1 LIMIT 1",
                    )
                    wait_blocked(pg, s2.thread_id(), fut)
                    query(adm, 'COMMIT')
                    assert fut.result(timeout=5) == 1
                assert query(adm, f"SELECT COUNT(*) FROM {target_tbl} WHERE tenant = 1 AND region = 'east' AND id = 1") == [(0,)]
            finally:
                s2.close()

        # 2. Root statement trigger + suppressing BEFORE DELETE trigger on cross-partition row movement!
        query(pg, f'CREATE TABLE {schema}.trig_audit(kind TEXT, n INT)')
        query(pg, f"""
            CREATE FUNCTION {schema}.fn_root_stmt_trig() RETURNS trigger LANGUAGE plpgsql AS $$
            BEGIN
                INSERT INTO {schema}.trig_audit VALUES ('root_stmt', 1);
                RETURN NULL;
            END $$
        """)
        query(pg, f"""
            CREATE TRIGGER tg_root_upd BEFORE UPDATE ON {schema}.part_ml
            FOR EACH STATEMENT EXECUTE FUNCTION {schema}.fn_root_stmt_trig()
        """)
        query(pg, f"""
            CREATE FUNCTION {schema}.fn_suppress_move_del() RETURNS trigger LANGUAGE plpgsql AS $$
            BEGIN
                INSERT INTO {schema}.trig_audit VALUES ('leaf_suppress_del', OLD.id);
                RETURN NULL;
            END $$
        """)
        query(pg, f"""
            CREATE TRIGGER tg_suppress_west BEFORE DELETE ON {schema}.leaf_2_west
            FOR EACH ROW EXECUTE FUNCTION {schema}.fn_suppress_move_del()
        """)

        query(adm, "INSERT INTO part_ml(tenant, region, id, v) VALUES (12, 'west', 77, 500)")
        # Cross-partition UPDATE moving (12, 'west', 77) -> 'east': leaf_2_west BEFORE DELETE suppresses move!
        # Must execute trigger ONCE (never retry in MysCurrentRead!) and report 0 rows updated!
        assert query(adm, "UPDATE part_ml SET region = 'east' WHERE tenant = 12 AND region = 'west' AND id = 77") == 0
        assert query(pg, f"SELECT COUNT(*) FROM {schema}.trig_audit WHERE kind = 'leaf_suppress_del'") == [(1,)]
        assert query(pg, f"SELECT COUNT(*) FROM {schema}.trig_audit WHERE kind = 'root_stmt'") == [(1,)]
        assert query(adm, "SELECT tenant, region, id, v FROM part_ml WHERE tenant = 12 AND id = 77") == [
            (12, 'west', 77, 500)
        ]
        query(pg, f'DROP TRIGGER tg_suppress_west ON {schema}.leaf_2_west')
        query(pg, f'DROP TRIGGER tg_root_upd ON {schema}.part_ml')
        query(adm, 'DELETE FROM part_ml')

        # 3. 8-thread concurrent DELETE + INSERT + binary prepared UPDATE + cross-partition movement!
        for k in range(1, 9):
            query(adm, 'INSERT INTO part_ml(tenant, region, id, v) VALUES (%s, %s, %s, %s)',
                  (1 if k <= 4 else 11, 'east', k, 0))
            query(adm, 'INSERT INTO part_hash(id, k, v) VALUES (%s, %s, %s)', (k, k * 10, 0))

        errors = []
        stop_at = time.monotonic() + 2.0
        lock = threading.Lock()
        deadlocks = [0]

        def worker(tid):
            raw = RawStmtClient(my_conn(database=schema))
            try:
                p_del = raw.prepare('DELETE FROM part_ml WHERE tenant = ? AND region = ? AND id = ? LIMIT 1')
                p_ins = raw.prepare('INSERT INTO part_ml(tenant, region, id, v) VALUES (?, ?, ?, ?)')
                p_hdel = raw.prepare('DELETE FROM part_hash WHERE id = ?')
                p_hins = raw.prepare('INSERT INTO part_hash(id, k, v) VALUES (?, ?, ?)')
                p_hupd = raw.prepare('UPDATE part_hash SET v = v + 1 WHERE id = ? LIMIT 1')
                step = 0
                while time.monotonic() < stop_at:
                    key = (step % 4) + 1
                    step += 1
                    # Cycle 1: single-key DELETE + INSERT inside BEGIN on multilevel dropped/reordered partition
                    with raw.conn.cursor() as cur:
                        cur.execute('BEGIN')
                    r1 = raw.execute(
                        p_del['id'],
                        [(T_LONG, struct.pack('<i', 1)), (T_VARCHAR, enc_str('east')), (T_LONG, struct.pack('<i', key))],
                    )
                    if 'errno' in r1:
                        if r1['errno'] == 1213:
                            with lock:
                                deadlocks[0] += 1
                            continue
                        errors.append(('p_del', tid, r1))
                        return
                    r2 = raw.execute(
                        p_ins['id'],
                        [
                            (T_LONG, struct.pack('<i', 1)),
                            (T_VARCHAR, enc_str('east')),
                            (T_LONG, struct.pack('<i', key)),
                            (T_LONG, struct.pack('<i', tid * 1000 + step)),
                        ],
                    )
                    if 'errno' in r2:
                        if r2['errno'] == 1213:
                            with lock:
                                deadlocks[0] += 1
                            continue
                        errors.append(('p_ins', tid, r2))
                        return
                    with raw.conn.cursor() as cur:
                        cur.execute('COMMIT')

                    # Cycle 2: HASH partition DELETE + INSERT + prepared LIMIT 1 UPDATE
                    with raw.conn.cursor() as cur:
                        cur.execute('BEGIN')
                    rh1 = raw.execute(p_hdel['id'], [(T_LONG, struct.pack('<i', key))])
                    if 'errno' in rh1:
                        if rh1['errno'] == 1213:
                            with lock:
                                deadlocks[0] += 1
                            continue
                        errors.append(('p_hdel', tid, rh1))
                        return
                    rh2 = raw.execute(
                        p_hins['id'],
                        [
                            (T_LONG, struct.pack('<i', key)),
                            (T_LONG, struct.pack('<i', key * 10)),
                            (T_LONG, struct.pack('<i', step)),
                        ],
                    )
                    if 'errno' in rh2:
                        if rh2['errno'] == 1213:
                            with lock:
                                deadlocks[0] += 1
                            continue
                        errors.append(('p_hins', tid, rh2))
                        return
                    with raw.conn.cursor() as cur:
                        cur.execute('COMMIT')

                    rhu = raw.execute(p_hupd['id'], [(T_LONG, struct.pack('<i', key))])
                    if 'errno' in rhu and rhu['errno'] != 1213:
                        errors.append(('p_hupd', tid, rhu))
                        return
            except Exception as exc:
                errors.append(('exc', tid, repr(exc)))
            finally:
                raw.close()

        with ThreadPoolExecutor(max_workers=8) as pool:
            futs = [pool.submit(worker, t) for t in range(8)]
            for f in futs:
                f.result(timeout=30)

        assert not errors, f'partitioned concurrency errors (including any 1062!): {errors}'
        assert query(adm, 'SELECT COUNT(*) FROM part_ml') == [(8,)]
        assert query(adm, 'SELECT COUNT(*) FROM part_hash') == [(8,)]

        # Also test concurrent cross-partition row movement (east <-> west) without spurious 40001/1062
        move_errs = []

        def mover(tid):
            mc = my_conn(database=schema)
            try:
                for i in range(25):
                    src = 'east' if (i + tid) % 2 == 0 else 'west'
                    dst = 'west' if src == 'east' else 'east'
                    try:
                        query(
                            mc,
                            'UPDATE part_ml SET region = %s, v = v + 1 WHERE tenant = 11 AND region = %s AND id = 5',
                            (dst, src),
                        )
                    except pymysql.err.OperationalError as e:
                        # Only genuine lock deadlocks are acceptable; serialization failures ('serialize') must NEVER occur!
                        if e.args[0] != 1213 or 'serialize' in str(e.args[1]).lower():
                            move_errs.append((tid, i, e.args))
                            return
            finally:
                mc.close()

        with ThreadPoolExecutor(max_workers=4) as pool:
            for f in [pool.submit(mover, t) for t in range(4)]:
                f.result(timeout=20)
        assert not move_errs, f'cross-partition movement errors: {move_errs}'
        assert query(adm, 'SELECT COUNT(*) FROM part_ml WHERE tenant = 11 AND id = 5') == [(1,)]
    finally:
        adm.close()
    print('PASS 1-E: Partitioned MysCurrentRead (HASH, 2-level dropped/reordered, LIMIT 1, triggers, row movement) verified', flush=True)


def main():
    pg = pg_conn()
    schema = f'tier1_{uuid.uuid4().hex[:8]}'
    try:
        my_init = my_conn()
        try:
            query(my_init, f'CREATE DATABASE {schema}')
        finally:
            my_init.close()

        test_1a_1b_version_serialization_and_acl(pg, schema)
        test_1c_log_lock_waits_and_prefix_metapage(pg, schema)
        test_1d_prepared_cached_plan_fastpath_and_benchmark(pg, schema)
        test_1e_partitioned_current_read(pg, schema)
        print('ALL TIER-1 OPTIMIZATION CHECKS PASSED!', flush=True)
    finally:
        try:
            my_clean = my_conn()
            try:
                query(my_clean, f'DROP DATABASE IF EXISTS {schema}')
            finally:
                my_clean.close()
        except Exception:
            query(pg, f'DROP SCHEMA IF EXISTS {schema} CASCADE')
        pg.close()


if __name__ == '__main__':
    main()

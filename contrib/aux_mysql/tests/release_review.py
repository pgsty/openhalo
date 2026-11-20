#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
"""Release boundaries on a disposable TLS/SCRAM instance (PG* / MYSQL_*).

Tests version identity, authentication cache invalidation, RLS, cancellation,
malformed wire messages, multi-packet binary values and concurrent writes from
both protocols. Requires psycopg2, PyMySQL and a superuser PG connection.
Uses unique roles/schema, removes them on exit, and changes no global settings.
"""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
import socket
import struct
import time
import uuid

import psycopg2
import pymysql


SCHEMA = "release_review_" + uuid.uuid4().hex[:10]
ROLE = SCHEMA + "_login"
HOST = os.getenv("MYSQL_HOST", "127.0.0.1")
PORT = int(os.environ["MYSQL_PORT"])
PASSWORD = "Review.fixture.123"


def pg():
    conn = psycopg2.connect(connect_timeout=5)
    conn.autocommit = True
    return conn


def mysql(user=None, password=None, tls=False):
    return pymysql.connect(host=HOST, port=PORT,
        user=user or os.getenv("MYSQL_USER", "dbuser_mysql"),
        password=password or os.getenv("MYSQL_PASSWORD", "Mysql.123"),
        charset="utf8mb4", autocommit=True, connect_timeout=5,
        read_timeout=30, write_timeout=30, binary_prefix=True,
        ssl_disabled=not tls,
        ssl={"check_hostname": False} if tls else None)


def query(conn, statement, args=None):
    with conn.cursor() as cur:
        cur.execute(statement, args)
        return cur.fetchall() if cur.description else ()


def rejected(conn, statement, errno=None):
    try:
        query(conn, statement)
    except (pymysql.MySQLError, psycopg2.Error) as error:
        if errno is not None:
            assert error.args[0] == errno, (statement, error)
        return
    raise AssertionError("unexpected success: " + statement)


def wire_rejected(conn, statement, errno, sqlstate):
    # Inspect SQLSTATE as well as errno: PyMySQL's exception omits SQLSTATE.
    conn._execute_command(0x03, statement)
    header = conn._read_bytes(4)
    size = int.from_bytes(header[:3], "little")
    assert header[3] == 1 and 9 <= size < 65536, header
    packet = conn._read_bytes(size)
    expected = b"\xff" + struct.pack("<H", errno) + b"#" + sqlstate.encode("ascii")
    assert packet[:9] == expected, (statement, packet)


def main():
    admin, client = pg(), mysql()
    try:
        query(admin, f"CREATE SCHEMA {SCHEMA}")
        query(client, f"USE {SCHEMA}")
        assert query(admin, "SHOW openhalo.version") == [("19.0.0",)]
        assert query(admin, "SHOW server_version_num") == [("190000",)]
        assert query(admin, "SELECT extversion FROM pg_extension WHERE extname='aux_mysql'") == [("2.0",)]
        compatibility = query(admin, "SHOW mysql.halo_mysql_version")[0][0]
        assert client.get_server_info() == compatibility
        assert query(client, "SELECT VERSION(),@@version") == ((compatibility, compatibility),)
        rejected(admin, "SET openhalo.version='spoofed'")
        comment = "OpenHalo 19.0.0 (PostgreSQL 19.0)"
        for scope in ("", "GLOBAL ", "SESSION "):
            assert query(client, f"SHOW {scope}VARIABLES LIKE 'version_comment'") == (("version_comment", comment),)
            for name in ("version", "version_comment"):
                wire_rejected(client, f"SET {scope}{name}='spoofed'", 1238, "HY000")
        assert query(client, "SELECT @@version_comment,@@GLOBAL.version_comment") == ((comment, comment),)
        print("PASS product/kernel/extension identity and read-only version variables", flush=True)

        query(admin, f"CREATE ROLE {ROLE} LOGIN PASSWORD '{PASSWORD}'")
        for tls in (False, False, True):
            with mysql(ROLE, PASSWORD, tls) as conn:
                assert conn._secure == tls
                identity = query(conn, "SELECT CURRENT_USER()")[0][0]
                assert identity.split("@", 1)[0] == ROLE, identity
        query(admin, f"ALTER ROLE {ROLE} PASSWORD 'Changed.fixture.123'")

        def denied_login(password):
            try:
                conn = mysql(ROLE, password)
            except pymysql.MySQLError:
                return
            conn.close()
            raise AssertionError("revoked/expired credentials accepted")

        denied_login(PASSWORD)
        with mysql(ROLE, "Changed.fixture.123") as conn:
            rejected(conn, "SELECT mysql.flush_auth_cache()", 1142)
            wire_rejected(conn, "SET GLOBAL sql_mode=@@global.sql_mode", 1227, "42000")
        query(admin, f"ALTER ROLE {ROLE} NOLOGIN")
        denied_login("Changed.fixture.123")
        query(admin, f"ALTER ROLE {ROLE} LOGIN VALID UNTIL '2000-01-01'")
        denied_login("Changed.fixture.123")
        query(admin, f"ALTER ROLE {ROLE} VALID UNTIL 'infinity'")
        print("PASS RSA/TLS auth, password rotation, NOLOGIN, expiry and privileged operations", flush=True)

        query(admin, f"CREATE TABLE {SCHEMA}.guarded(id int PRIMARY KEY, tenant text, v int)")
        query(admin, f"INSERT INTO {SCHEMA}.guarded VALUES(1,'other',10),(2,'{ROLE}',20)")
        query(admin, f"ALTER TABLE {SCHEMA}.guarded ENABLE ROW LEVEL SECURITY")
        query(admin, f"CREATE POLICY tenant ON {SCHEMA}.guarded USING(tenant=current_user) WITH CHECK(tenant=current_user)")
        query(admin, f"GRANT USAGE ON SCHEMA {SCHEMA} TO {ROLE}")
        query(admin, f"GRANT SELECT,INSERT,UPDATE,DELETE ON {SCHEMA}.guarded TO {ROLE}")
        with mysql(ROLE, "Changed.fixture.123") as conn:
            query(conn, f"USE {SCHEMA}")
            rejected(conn, f"REPLACE INTO guarded VALUES(1,'{ROLE}',99)", 1142)
            rejected(conn, f"INSERT INTO guarded VALUES(1,'{ROLE}',99) ON DUPLICATE KEY UPDATE v=99", 1142)
            query(conn, f"INSERT INTO guarded VALUES(2,'{ROLE}',21) ON DUPLICATE KEY UPDATE v=21")
        assert query(admin, f"SELECT id,tenant,v FROM {SCHEMA}.guarded ORDER BY id") == [(1, "other", 10), (2, ROLE, 21)]
        print("PASS RLS on REPLACE and all-key ON DUPLICATE KEY UPDATE", flush=True)

        sleeper = mysql()
        try:
            pid = query(sleeper, "SELECT pg_backend_pid()")[0][0]
            with ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(query, sleeper, "SELECT pg_sleep(20)")
                for _ in range(100):
                    if query(admin, "SELECT wait_event FROM pg_stat_activity WHERE pid=%s", (pid,)) == [("PgSleep",)]:
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError("MySQL sleep did not start")
                assert query(admin, "SELECT pg_cancel_backend(%s)", (pid,)) == [(True,)]
                try:
                    pending.result(timeout=5)
                except pymysql.MySQLError as error:
                    assert error.args[0] == 1317, error
                else:
                    raise AssertionError("MySQL query was not cancelled")
            assert query(sleeper, "SELECT 42") == ((42,),)
        finally:
            sleeper.close()
        print("PASS native cancellation of MySQL query and subsequent session reuse", flush=True)

        def read_frame(sock):
            def exact(n):
                data = b""
                while len(data) < n:
                    part = sock.recv(n - len(data))
                    if not part:
                        raise AssertionError("unexpected disconnect")
                    data += part
                return data
            header = exact(4)
            size = int.from_bytes(header[:3], "little")
            assert size < 65536, size
            return exact(size)

        caps = 0x0200 | 0x8000 | 0x00200000
        malformed = [bytes(n) for n in (0, 1, 3, 4, 7, 31)]
        malformed += [struct.pack("<I", caps) + bytes(28) + b"x\0" + b"\xfe" + bytes(n)
                      for n in (0, 1, 4, 7)]
        for payload in malformed:
            with socket.create_connection((HOST, PORT), timeout=5) as sock:
                sock.settimeout(5)
                read_frame(sock)
                sock.sendall(len(payload).to_bytes(3, "little") + b"\x01" + payload)
                assert read_frame(sock)[0] == 0xff
        with socket.create_connection((HOST, PORT), timeout=5) as sock:
            sock.settimeout(5)
            read_frame(sock)
            sock.sendall((2 * 1024 * 1024).to_bytes(3, "little") + b"\x01")
            packet = read_frame(sock)
            assert packet[:3] == b"\xff\x81\x04", packet  # 1153, before reading payload
        for command, payload in ((0x7f, b""), (0x17, b""), (0x17, bytes(3)),
                                 (0x17, struct.pack("<I", 0xffffffff))):
            with mysql() as conn:
                conn._execute_command(command, payload)
                try:
                    conn._read_packet()
                except pymysql.MySQLError:
                    pass
                else:
                    raise AssertionError((command, payload))
                assert query(conn, "SELECT 42") == ((42,),)
        assert query(admin, "SELECT 42") == [(42,)]
        print("PASS malformed/pre-auth oversized packets and invalid commands without backend disruption", flush=True)

        blob = bytes(range(256)) * 65536 + b"frame boundary\0\xff"
        query(client, "CREATE TABLE packet_data(v LONGBLOB)")
        query(client, "INSERT INTO packet_data VALUES(%s)", (blob,))
        assert query(client, "SELECT v FROM packet_data") == ((blob,),)
        assert query(admin, f"SELECT octet_length(v),md5(v) FROM {SCHEMA}.packet_data") == [(len(blob), hashlib.md5(blob).hexdigest())]
        print("PASS multi-packet input/output above 16 MiB with all binary byte values", flush=True)

        query(client, "CREATE TABLE counter(id int PRIMARY KEY,v int NOT NULL)")
        query(client, "INSERT INTO counter VALUES(1,0)")

        def writer(index):
            conn = mysql() if index % 2 else pg()
            try:
                query(conn, f"USE {SCHEMA}" if index % 2 else
                      f"SET search_path={SCHEMA},mysql,public")
                for i in range(100):
                    if i % 5 == 0:
                        query(conn, "BEGIN")
                        query(conn, "UPDATE counter SET v=v+1000 WHERE id=1")
                        query(conn, "ROLLBACK")
                    statement = ("INSERT INTO counter VALUES(1,1) ON DUPLICATE KEY UPDATE v=v+1"
                                 if index % 2 else
                                 "INSERT INTO counter VALUES(1,1) ON CONFLICT(id) DO UPDATE SET v=counter.v+1")
                    query(conn, statement)
            finally:
                conn.close()

        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(writer, range(8)))
        assert query(client, "SELECT v FROM counter") == ((800,),)
        assert query(admin, f"SELECT v FROM {SCHEMA}.counter") == [(800,)]
        print("PASS 8 mixed-protocol writers: 800 committed increments and 160 rolled-back updates", flush=True)
    finally:
        client.close()
        query(admin, f"DROP SCHEMA IF EXISTS {SCHEMA} CASCADE")
        if query(admin, "SELECT 1 FROM pg_roles WHERE rolname=%s", (ROLE,)):
            query(admin, f"DROP ROLE {ROLE}")
        admin.close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
"""Rehearse PG18 -> PG19 with matching aux_mysql 2.0 on disposable clusters.

Requires built old/new prefixes and PyMySQL. The work directory must be empty;
all logs and data remain there, and both servers are stopped on exit.
This does not test upgrading the SQL objects from aux_mysql 1.x to 2.0.
"""
import argparse
import os
from pathlib import Path
import subprocess

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-prefix", type=Path, required=True)
    parser.add_argument("--new-prefix", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--old-pg-port", type=int, default=55828)
    parser.add_argument("--new-pg-port", type=int, default=55829)
    parser.add_argument("--old-mysql-port", type=int, default=33828)
    parser.add_argument("--new-mysql-port", type=int, default=33829)
    args = parser.parse_args()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        parser.error("work directory must be empty")
    ports = [args.old_pg_port, args.new_pg_port, args.old_mysql_port, args.new_mysql_port]
    if len(set(ports)) != 4 or any(p < 1024 or p > 65535 for p in ports):
        parser.error("provide four distinct unprivileged ports")
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("PG"):
            env.pop(key)
    env["LC_ALL"] = "C"
    sockets = work / "sockets"
    sockets.mkdir()
    clusters = []

    def run(command, name):
        with (work / (name + ".log")).open("wb") as log:
            subprocess.run([str(x) for x in command], cwd=work, env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)

    def start(cluster):
        run([cluster[0] / "pg_ctl", "-D", cluster[1], "-l", work / (cluster[2] + "-server.log"),
             "-w", "start"], cluster[2] + "-start")

    def stop(cluster):
        if (cluster[1] / "postmaster.pid").exists():
            run([cluster[0] / "pg_ctl", "-D", cluster[1], "-m", "fast", "-w", "stop"], cluster[2] + "-stop")

    def pgsql(cluster, port, statement):
        run([cluster[0] / "psql", "-XAt", "-v", "ON_ERROR_STOP=1", "-h", sockets,
             "-p", port, "-U", "postgres", "-d", "postgres", "-c", statement], cluster[2] + "-sql")

    def mysql(port):
        return pymysql.connect(host="127.0.0.1", port=port, user="postgres",
                               password="Upgrade.fixture.123", autocommit=True,
                               read_timeout=15, write_timeout=15)

    def sql(conn, statement):
        with conn.cursor() as cur:
            cur.execute(statement)
            return cur.fetchall() if cur.description else cur.rowcount

    def snapshot(conn):
        sql(conn, "USE pg19_upgrade")
        rows = [sql(conn, query) for query in (
            "SELECT id,code,HEX(payload),n,g FROM parent ORDER BY id",
            "SELECT pid FROM child ORDER BY pid",
            "SELECT name FROM pref ORDER BY name",
            "SHOW CREATE TABLE parent",
            "SHOW CREATE TABLE child",
            "SHOW CREATE TABLE pref",
            "SHOW CREATE TABLE pending",
        )]
        # pg_dump recreates indexes in a different order. Compare all their
        # definitions while preserving column order and every table option.
        for i in range(3, len(rows)):
            name, definition = rows[i][0]
            lines = definition.splitlines()
            keys = [line.rstrip(",") for line in lines
                    if line.startswith(("  PRIMARY KEY", "  UNIQUE KEY", "  KEY", "  CONSTRAINT"))]
            columns = [line for line in lines
                       if not line.startswith(("  PRIMARY KEY", "  UNIQUE KEY", "  KEY", "  CONSTRAINT"))]
            rows[i] = (name, columns, sorted(keys))
        return rows

    try:
        for label, prefix, pgport, myport, version in (
            ("old", args.old_prefix, args.old_pg_port, args.old_mysql_port, "18.6"),
            ("new", args.new_prefix, args.new_pg_port, args.new_mysql_port, "19.0"),
        ):
            binary = prefix.resolve() / "bin"
            actual = subprocess.check_output([binary / "postgres", "--version"], text=True).strip()
            if not actual.endswith(" " + version):
                raise RuntimeError(f"expected {version}, got {actual}")
            data = work / label
            run([binary / "initdb", "-D", data, "-U", "postgres", "-E", "UTF8",
                 "--locale=C", "--auth=trust"], label + "-initdb")
            with (data / "postgresql.conf").open("a") as config:
                config.write(f"\nlisten_addresses='127.0.0.1'\nport={pgport}\nmysql.port={myport}\n"
                             f"unix_socket_directories='{sockets}'\nmax_connections=30\n")
            clusters.append((binary, data, label))
        old, new = clusters
        start(old)
        pgsql(old, args.old_pg_port, "ALTER ROLE postgres PASSWORD 'Upgrade.fixture.123'; CREATE EXTENSION aux_mysql;")
        with mysql(args.old_mysql_port) as conn:
            version = sql(conn, "SELECT extversion FROM pg_catalog.pg_extension WHERE extname='aux_mysql'")
            assert version == (("2.0",),), version
            for statement in (
                "CREATE DATABASE pg19_upgrade", "USE pg19_upgrade",
                "CREATE TABLE parent(id int AUTO_INCREMENT PRIMARY KEY,code varchar(40) UNIQUE,payload varbinary(20),n int,g int AS(n*2) STORED)",
                "CREATE TABLE child(pid int, FOREIGN KEY(pid) REFERENCES parent(id) ON DELETE CASCADE ON UPDATE CASCADE)",
                "CREATE TABLE pref(name varchar(400), UNIQUE KEY p(name(3)))",
                "INSERT INTO parent(id,code,payload,n) VALUES(100,'Café',X'0041FF',7)",
                "INSERT INTO child VALUES(100)", "INSERT INTO pref VALUES('Abcdef')",
                "SET FOREIGN_KEY_CHECKS=0",
                "CREATE TABLE pending(pid int,FOREIGN KEY(pid) REFERENCES missing(id))",
                "INSERT INTO pending VALUES(999)", "SET FOREIGN_KEY_CHECKS=1",
            ):
                sql(conn, statement)
            before = snapshot(conn)
        pgsql(old, args.old_pg_port, """CREATE FUNCTION pg19_upgrade.bump() RETURNS integer
            LANGUAGE SQL BEGIN ATOMIC
            INSERT INTO pg19_upgrade.parent(id,code,n) VALUES(100,'Café',8)
            ON CONFLICT(id) DO UPDATE SET n=EXCLUDED.n RETURNING n; END;""")
        stop(old)
        command = [new[0] / "pg_upgrade", "-b", old[0], "-B", new[0], "-d", old[1], "-D", new[1],
                   "-p", args.old_pg_port, "-P", args.new_pg_port, "-U", "postgres", "--socketdir", sockets]
        run([*command, "--check"], "pg-upgrade-check")
        run(command, "pg-upgrade")
        start(new)
        with mysql(args.new_mysql_port) as conn:
            after = snapshot(conn)
            assert before == after, (before, after)
            sql(conn, "INSERT INTO parent(code,payload,n) VALUES('next',X'00',3)")
            assert sql(conn, "SELECT id,g FROM parent WHERE code='next'") == ((101, 6),)
            try:
                sql(conn, "INSERT INTO pref VALUES('abcZZ')")
            except pymysql.IntegrityError as error:
                assert error.args[0] == 1062, error
            else:
                raise AssertionError("prefix uniqueness was lost")
            sql(conn, "CREATE TABLE missing(id int PRIMARY KEY)")
            try:
                sql(conn, "INSERT INTO pending VALUES(998)")
            except pymysql.IntegrityError as error:
                assert error.args[0] == 1452, error
            else:
                raise AssertionError("pending FK was not rebound")
            sql(conn, "DELETE FROM parent WHERE id=100")
            assert sql(conn, "SELECT * FROM child") == ()
        pgsql(new, args.new_pg_port, "SELECT pg19_upgrade.bump();")
        assert (work / "new-sql.log").read_text().strip() == "8"
        print("PASS PG18.6 -> PG19.0 pg_upgrade --check and copy upgrade", flush=True)
        print("PASS binary bytes, generated columns, sequence state, prefix uniqueness, FK cascade/pending metadata and stored SQL body", flush=True)
        print(f"Evidence: {work}")
    finally:
        for cluster in reversed(clusters):
            stop(cluster)


if __name__ == "__main__":
    main()

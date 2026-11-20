#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
"""Bounded sysbench contention runs with integrity and latency evidence.

Requires sysbench and PyMySQL. Uses MYSQL_HOST/PORT/USER/PASSWORD; creates a
unique disposable schema. Store --output outside the source tree. Only 1213
is retryable: duplicate keys, lock timeouts and all other errors fail the run.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time
import uuid

import pymysql


def run(args):
    host = os.getenv('MYSQL_HOST', '127.0.0.1')
    port = int(os.getenv('MYSQL_PORT', '3306'))
    user = os.getenv('MYSQL_USER', 'dbuser_mysql')
    password = os.getenv('MYSQL_PASSWORD', 'Mysql.123')
    schema = 'hotspot_' + uuid.uuid4().hex[:12]
    conn = pymysql.connect(host=host, port=port, user=user, password=password,
                           autocommit=True, connect_timeout=5, read_timeout=15)
    args.output.mkdir(parents=True, exist_ok=False)
    common = ['--db-driver=mysql', '--mysql-host='+host, '--mysql-port='+str(port),
              '--mysql-user='+user, '--mysql-password='+password, '--mysql-db='+schema,
              '--tables='+str(args.tables), '--table-size='+str(args.size),
              '--db-ps-mode='+args.protocol, '--rand-type='+args.distribution]
    results = []
    summary = dict(schema=schema, parameters={k: str(v) if isinstance(v, Path) else v
                               for k, v in vars(args).items()}, results=results)

    def save():
        (args.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')

    try:
        with conn.cursor() as cur:
            cur.execute('CREATE DATABASE ' + schema)
        for repetition in range(args.repeat):
            for workload in args.workloads:
                name = f'{repetition+1}-{workload}'
                base = ['sysbench', workload, *common, '--rand-seed='+str(args.seed+repetition)]
                with (args.output/(name+'-prepare.log')).open('w') as log:
                    subprocess.run([*base, 'prepare'], stdout=log, stderr=subprocess.STDOUT,
                                   timeout=120, check=True)
                command = [*base, '--threads='+str(args.threads), '--time='+str(args.seconds),
                           '--mysql-ignore-errors=1213', '--percentile=99', 'run']
                start = time.monotonic()
                failure = None
                with (args.output/(name+'.log')).open('w') as log:
                    with subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT) as child:
                        while child.poll() is None:
                            # A failed sysbench worker can keep its connection
                            # and transaction open while the others wait on it.
                            # Preserve the fatal error and stop this failed run.
                            output = (args.output/(name+'.log')).read_text()
                            if 'FATAL:' in output:
                                failure = 'sysbench reported a fatal error'
                            elif time.monotonic()-start >= args.seconds+args.max_drain:
                                failure = 'excessive drain time'
                            if failure:
                                child.terminate()
                                try:
                                    child.wait(timeout=5)
                                except subprocess.TimeoutExpired:
                                    child.kill()
                                    child.wait()
                                break
                            time.sleep(.1)
                        code = child.wait()
                wall = time.monotonic()-start
                text = (args.output/(name+'.log')).read_text()
                record = dict(case=name, exit=code, wall_seconds=round(wall, 3))
                if failure:
                    record['failure'] = failure
                patterns = dict(transactions=r'transactions:\s+(\d+)',
                                retries=r'ignored errors:\s+(\d+)',
                                elapsed_seconds=r'total time:\s+([\d.]+)s',
                                average_ms=r'avg:\s+([\d.]+)',
                                p99_ms=r'99th percentile:\s+([\d.]+)',
                                maximum_ms=r'max:\s+([\d.]+)')
                for key, pattern in patterns.items():
                    match = re.search(pattern, text)
                    record[key] = (int(match[1]) if key in ('transactions', 'retries') else
                                   float(match[1])) if match else None
                results.append(record)
                save()
                assert code == 0 and failure is None, (name, failure, text[-3000:])
                assert all(record[k] is not None for k in patterns), record
                assert record['transactions'] > 0, record
                assert wall < args.seconds+args.max_drain, ('excessive drain time', record)
                with conn.cursor() as cur:
                    counts = []
                    for i in range(1, args.tables+1):
                        cur.execute(f'SELECT COUNT(*),COUNT(DISTINCT id),MIN(id),MAX(id),SUM(id) '
                                    f'FROM {schema}.sbtest{i}')
                        values = tuple(int(v) for v in cur.fetchone())
                        assert values == (args.size, args.size, 1, args.size,
                                          args.size*(args.size+1)//2), (name, values)
                        counts.append(values)
                    record['integrity'] = counts
                    for i in range(1, args.tables+1):
                        cur.execute(f'DROP TABLE {schema}.sbtest{i}')
                save()
                print(json.dumps(record), flush=True)
    finally:
        with conn.cursor() as cur:
            cur.execute('DROP DATABASE IF EXISTS ' + schema)
        conn.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--threads', type=int, default=16)
    parser.add_argument('--size', type=int, default=100)
    parser.add_argument('--tables', type=int, default=1)
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--max-drain', type=int, default=10)
    parser.add_argument('--distribution', choices=['special', 'uniform'], default='special')
    parser.add_argument('--protocol', choices=['auto', 'disable'], default='auto')
    parser.add_argument('--workloads', nargs='+', choices=['oltp_read_write', 'oltp_write_only',
                        'oltp_point_select', 'oltp_read_only'],
                        default=['oltp_read_write', 'oltp_write_only'])
    args = parser.parse_args()
    assert all(getattr(args, k) > 0 for k in
               ['threads', 'size', 'tables', 'seconds', 'repeat', 'max_drain'])
    run(args)

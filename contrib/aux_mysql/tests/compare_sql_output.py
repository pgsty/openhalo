#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
"""Compare mysql table output and errors independently of client buffering.

Expected files contain both streams from the MariaDB client. Oracle's client
does not echo the failed statement; remove only that presentation block.
All table output and every error (including its order and text) must match.
"""
import difflib
from pathlib import Path
import re
import sys


def without_statement_echo(text):
    return re.sub(r"(?ms)^--------------\n.*?^--------------\n\n", "", text)


def main():
    expected_path, stdout_path, stderr_path = map(Path, sys.argv[1:])
    expected = without_statement_echo(expected_path.read_text())
    expected_errors = "".join(line for line in expected.splitlines(True)
                              if line.startswith("ERROR "))
    expected_output = "".join(line for line in expected.splitlines(True)
                              if not line.startswith("ERROR "))
    success = True
    for path, reference in ((stdout_path, expected_output), (stderr_path, expected_errors)):
        actual = without_statement_echo(path.read_text())
        if actual != reference:
            success = False
            sys.stdout.writelines(difflib.unified_diff(
                reference.splitlines(True), actual.splitlines(True),
                fromfile=str(expected_path), tofile=str(path)))
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())

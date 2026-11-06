#!/usr/bin/env python3
"""Golden-file tests for h2c.

  tests/parse/*.h2   -> `h2c --ast` must succeed and match the .ast file
  tests/lex/*.h2     -> `h2c --tokens` must match the .tokens file
  tests/errors/*.h2  -> `h2c --check` must fail and stderr must match the .err file
  tests/resolve/<case>/main.h2 -> `h2c --check` stderr must match <case>/expected.err
                                 (an empty file means the project must check cleanly)
  examples/**/*.h2, lib/**/*.h2 -> must pass `h2c --check`

Usage: run_tests.py <h2c> <repo-root> [--update]
"""
import subprocess
import sys
from pathlib import Path


def run(h2c, mode, path, root):
    return subprocess.run([h2c, mode, str(path.relative_to(root))], cwd=root,
                          capture_output=True, text=True)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    h2c, root = sys.argv[1], Path(sys.argv[2]).resolve()
    update = "--update" in sys.argv[3:]
    failures, count = [], 0

    def golden(path, suffix, actual, ok, why):
        nonlocal count
        count += 1
        expected_file = path.with_suffix(suffix)
        if update:
            expected_file.write_text(actual)
        elif not expected_file.exists():
            failures.append(f"{path}: missing {expected_file.name} (run with --update)")
            return
        elif expected_file.read_text() != actual:
            failures.append(f"{path}: output differs from {expected_file.name}\n--- got ---\n{actual}")
            return
        if not ok:
            failures.append(f"{path}: {why}")

    for p in sorted((root / "tests/parse").glob("*.h2")):
        r = run(h2c, "--ast", p, root)
        golden(p, ".ast", r.stdout + r.stderr, r.returncode == 0, f"exit {r.returncode}")
    for p in sorted((root / "tests/lex").glob("*.h2")):
        r = run(h2c, "--tokens", p, root)
        golden(p, ".tokens", r.stdout + r.stderr, r.returncode == 0, f"exit {r.returncode}")
    for p in sorted((root / "tests/errors").glob("*.h2")):
        r = run(h2c, "--check", p, root)
        golden(p, ".err", r.stderr, r.returncode == 1, f"expected exit 1, got {r.returncode}")
    for p in sorted((root / "tests/resolve").glob("*/main.h2")):
        count += 1
        r = run(h2c, "--check", p, root)
        expected_file = p.parent / "expected.err"
        want_ok = not update and expected_file.exists() and expected_file.read_text() == ""
        if update:
            expected_file.write_text(r.stderr)
        elif not expected_file.exists():
            failures.append(f"{p}: missing expected.err (run with --update)")
        elif expected_file.read_text() != r.stderr:
            failures.append(f"{p}: diagnostics differ from expected.err\n--- got ---\n{r.stderr}")
        elif r.returncode != (0 if want_ok else 1):
            failures.append(f"{p}: unexpected exit code {r.returncode}")
    for d in ("examples", "lib"):
        for p in sorted((root / d).rglob("*.h2")):
            count += 1
            r = run(h2c, "--check", p, root)
            if r.returncode != 0:
                failures.append(f"{p}: does not parse\n{r.stderr}")

    for f in failures:
        print("FAIL", f)
    print(f"{count - len(failures)}/{count} passed" + (" (goldens updated)" if update else ""))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

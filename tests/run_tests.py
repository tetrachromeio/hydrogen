#!/usr/bin/env python3
"""Golden-file tests for h2c.

  tests/parse/*.h2   -> `h2c --ast` must succeed and match the .ast file
  tests/lex/*.h2     -> `h2c --tokens` must match the .tokens file
  tests/errors/*.h2  -> `h2c --ast` must fail (syntax errors)
  tests/types/*.h2   -> `h2c --types` output (inferred local types + errors) must match .out and stderr must match the .err file
  tests/resolve/<case>/main.h2 -> `h2c --check` stderr must match <case>/expected.err
                                 (an empty file means the project must check cleanly)
  tests/projects/<case>/app/     -> `hy check` output must match <case>/expected.out
  tests/run/*.h2     -> compiled with clang and run (stdin from .in if present, in a temp dir):
                        stdout + exit code must match .out
                        (under valgrind when available: no leaks or memory errors)
  tests/freestanding/*.h2 -> must not reference any C function (only llvm.* built-ins);
                        run, and stdout + exit code must match .out
  tests/boron/<example>.in -> boron/examples/<example> is built with `hy build` and run
                        headless (stdin from the .in file); stdout + exit code must match .out
  tests/boron/test_page.js -> Boron's page (JS/CSS) driven in Chromium (needs node + Playwright)
  tests/carbon/      -> Carbon (carbon/examples/generate) on a tiny random model must print exactly
                        what the independent numpy implementation (reference.py) prints (needs numpy)
  tests/lsp/test_lsp.py -> drives `hy lsp` like an editor (diagnostics, definition, hover)
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
    h2c, root = str(Path(sys.argv[1]).resolve()), Path(sys.argv[2]).resolve()
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
        r = run(h2c, "--ast", p, root)
        golden(p, ".err", r.stderr, r.returncode == 1, f"expected exit 1, got {r.returncode}")
    for p in sorted((root / "tests/types").glob("*.h2")):
        r = run(h2c, "--types", p, root)
        golden(p, ".out", r.stdout + r.stderr, True, "")
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
    hy = str(Path(h2c).with_name("hy"))
    for app in sorted((root / "tests/projects").glob("*/app")):
        count += 1
        r = subprocess.run([hy, "check"], cwd=app, capture_output=True, text=True)
        got = r.stdout + r.stderr + f"[exit {r.returncode}]\n"
        expected_file = app.parent / "expected.out"
        if update:
            expected_file.write_text(got)
        elif not expected_file.exists() or expected_file.read_text() != got:
            failures.append(f"{app}: hy check output differs from expected.out\n--- got ---\n{got}")
    import shutil, tempfile
    clang = shutil.which("clang")
    valgrind = shutil.which("valgrind")
    runtime = root / "lib/runtime/h2rt.c"
    import re, shutil, tempfile
    clang = shutil.which("clang")
    for p in sorted((root / "tests/freestanding").glob("*.h2")):
        count += 1
        r = run(h2c, "--emit-llvm", p, root)
        if r.returncode != 0:
            failures.append(f"{p}: h2c failed\n{r.stderr}")
            continue
        outside = [l for l in r.stdout.splitlines()
                   if l.startswith("declare") and not re.search(r"@(\"?)llvm\.", l)]
        if outside:
            failures.append(f"{p}: not freestanding; it calls out to:\n" + "\n".join(outside))
            continue
        if not clang:
            continue
        with tempfile.TemporaryDirectory() as tmp:
            ll, exe = Path(tmp) / "prog.ll", Path(tmp) / "prog"
            ll.write_text(r.stdout)
            b = subprocess.run([clang, "-O1", "-w", str(ll), "-o", str(exe)], capture_output=True, text=True)
            if b.returncode != 0:
                failures.append(f"{p}: clang failed\n{b.stderr[-2000:]}")
                continue
            x = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            got = x.stdout + f"[exit {x.returncode}]\n"
            expected_file = p.with_suffix(".out")
            if update:
                expected_file.write_text(got)
            elif not expected_file.exists() or expected_file.read_text() != got:
                failures.append(f"{p}: output differs from {expected_file.name}\n--- got ---\n{got}")
    import platform
    for p in sorted((root / "tests/run").glob("*.h2")):
        first_line = p.read_text().split("\n", 1)[0]
        if first_line.startswith("// requires: linux") and platform.system() != "Linux":
            continue  # platform-specific test
        if first_line.startswith("// requires: x86_64") and platform.machine().lower() not in ("x86_64", "amd64"):
            continue
        count += 1
        if not clang:
            failures.append(f"{p}: clang not found; cannot run code generation tests")
            continue
        with tempfile.TemporaryDirectory() as tmp:
            ll = Path(tmp) / "prog.ll"
            exe = Path(tmp) / "prog"
            r = run(h2c, "--emit-llvm", p, root)
            if r.returncode != 0:
                failures.append(f"{p}: h2c failed\n{r.stderr}")
                continue
            ll.write_text(r.stdout)
            b = subprocess.run([clang, "-O1", "-g", "-w", str(ll), str(runtime), "-lm", "-lpthread", "-o", str(exe)],
                               capture_output=True, text=True)
            if b.returncode != 0:
                failures.append(f"{p}: clang failed\n{b.stderr[-2000:]}")
                continue
            cmd = [str(exe)]
            if valgrind:
                cmd = [valgrind, "-q", "--leak-check=full", "--errors-for-leak-kinds=definite,indirect",
                       "--error-exitcode=99"] + cmd
            stdin_file = p.with_suffix(".in")
            stdin_text = stdin_file.read_text() if stdin_file.exists() else ""
            x = subprocess.run(cmd, capture_output=True, text=True, timeout=60, input=stdin_text, cwd=tmp)
            if x.returncode == 99:
                failures.append(f"{p}: memory errors or leaks\n{x.stderr[-3000:]}")
                continue
            got = x.stdout + f"[exit {x.returncode}]\n"
            expected_file = p.with_suffix(".out")
            if update:
                expected_file.write_text(got)
            elif not expected_file.exists() or expected_file.read_text() != got:
                failures.append(f"{p}: program output differs from {expected_file.name}\n--- got ---\n{got}")
    # Boron: the generated page header is current, the examples run headless, the page works in a browser.
    count += 1
    r = subprocess.run([sys.executable, str(root / "boron/native/page/gen_page.py"), "--check"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        failures.append(r.stdout)
    import os
    for stdin_file in sorted((root / "tests/boron").glob("*.in")):
        count += 1
        app = root / "boron/examples" / stdin_file.stem
        b = subprocess.run([hy, "build"], cwd=app, capture_output=True, text=True)
        if b.returncode != 0:
            failures.append(f"{app}: hy build failed\n{b.stdout}{b.stderr}")
            continue
        cmd = [str(app / "build" / stdin_file.stem)]
        if valgrind:
            cmd = [valgrind, "-q", "--leak-check=full", "--errors-for-leak-kinds=definite,indirect",
                   "--error-exitcode=99"] + cmd
        x = subprocess.run(cmd, capture_output=True, text=True, timeout=60, input=stdin_file.read_text(),
                           env={**os.environ, "BORON_HEADLESS": "1"})
        if x.returncode == 99:
            failures.append(f"{app}: memory errors or leaks\n{x.stderr[-3000:]}")
            continue
        got = x.stdout + f"[exit {x.returncode}]\n"
        expected_file = stdin_file.with_suffix(".out")
        if update:
            expected_file.write_text(got)
        elif not expected_file.exists() or expected_file.read_text() != got:
            failures.append(f"{app}: output differs from {expected_file.name}\n--- got ---\n{got}")
    node = shutil.which("node")
    if node:
        count += 1
        r = subprocess.run([node, str(root / "tests/boron/test_page.js"), str(root)], capture_output=True,
                           text=True, timeout=120)
        if r.returncode != 0:
            failures.append(f"tests/boron/test_page.js: failed\n{r.stdout}{r.stderr}")

    # Carbon: same text as the numpy reference, greedy and sampled, on a tiny random model.
    try:
        import numpy  # noqa: F401
        have_numpy = True
    except ImportError:
        have_numpy = False
    if have_numpy:
        app = root / "carbon/examples/generate"
        b = subprocess.run([hy, "build"], cwd=app, capture_output=True, text=True)
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run([sys.executable, "-I", str(root / "tests/carbon/make_tiny.py"), tmp], check=True)
            model, tok = str(Path(tmp) / "tiny.bin"), str(Path(tmp) / "tiny_tok.bin")
            cases = [("the cat", "40", "0", "0.9", "1"), ("café on the", "48", "1.0", "0.9", "5"),
                     ("xyz", "30", "0.7", "0", "9"), ("", "48", "1.0", "0.5", "3")]
            for prompt, steps, temp, topp, seed in cases:
                count += 1
                what = f"carbon: prompt {prompt!r}, -t {temp} -p {topp} -s {seed}"
                if b.returncode != 0:
                    failures.append(f"{app}: hy build failed\n{b.stdout}{b.stderr}")
                    break
                ref = subprocess.run([sys.executable, "-I", str(root / "tests/carbon/reference.py"), model, tok,
                                      prompt, steps, temp, topp, seed], capture_output=True)
                cmd = [str(app / "build/generate"), model, "-z", tok, "-i", prompt, "-n", steps, "-t", temp,
                       "-p", topp, "-s", seed]
                if valgrind:
                    cmd = [valgrind, "-q", "--leak-check=full", "--errors-for-leak-kinds=definite,indirect",
                           "--error-exitcode=99"] + cmd
                got = subprocess.run(cmd, capture_output=True, timeout=120)
                if got.returncode != 0:
                    failures.append(f"{what}: exit {got.returncode}\n{got.stderr.decode(errors='replace')[-3000:]}")
                elif got.stdout != ref.stdout:
                    failures.append(f"{what}: differs from reference.py\n--- carbon ---\n{got.stdout!r}"
                                    f"\n--- reference ---\n{ref.stdout!r}")

    count += 1
    r = subprocess.run([sys.executable, str(root / "tests/lsp/test_lsp.py"), hy], capture_output=True, text=True,
                       timeout=120)
    if r.returncode != 0:
        failures.append(f"tests/lsp/test_lsp.py: language server test failed\n{r.stdout}{r.stderr}")
    for d in ("examples", "lib"):
        # In lib/, only module roots (init.h2) are programs; other files are included parts.
        for p in sorted((root / d).rglob("*.h2" if d == "examples" else "init.h2")):
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

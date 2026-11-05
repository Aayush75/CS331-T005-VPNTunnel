#!/usr/bin/env python3
"""Run Clang's warnings over the kernel modules' sources, on a tree configured for GCC.

A module cannot be built with Clang against a GCC-configured kernel tree: Clang rejects flags such
as -mrecord-mcount and -fconserve-stack. This takes the exact GCC command that kbuild used for each
object (the .cmd files left by `make -C kernel`), swaps in clang, drops the arguments Clang rejects
and runs -fsyntax-only with Clang's own warnings. It checks the sources, not the generated code.

  make -C kernel [VPT1_DEBUG=1]
  python3 tools/clang_check.py [--kernel-dir kernel] [--tests-dir tests/unit]

Exit status is 1 if Clang printed any warning or error for a project source file.
"""
import argparse
import os
import re
import shlex
import subprocess
import sys

EXTRA = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-missing-field-initializers",
         "-Wno-type-limits", "-Wsometimes-uninitialized", "-Wimplicit-fallthrough", "-Wshadow"]
REJECTED = re.compile(r"(?:unknown argument|unsupported option|unknown warning option)[^']*'([^']+)'")


def commands(*roots):
    for root, _dirs, files in (w for r in roots for w in os.walk(r)):
        for name in files:
            if not (name.startswith(".") and name.endswith(".o.cmd")) or name.endswith(".mod.o.cmd"):
                continue
            for line in open(os.path.join(root, name), errors="replace"):
                m = re.match(r"(?:saved)?cmd_\S+\s*:?=\s*(.*)$", line.strip()) or re.match(r"savedcmd_\S+ := (.*)$", line.strip())
                if m and " -c " in m.group(1) and (".mod." not in m.group(1)):
                    yield m.group(1).split(" ; ")[0]
                    break


def adapt(command):
    argv = shlex.split(command)
    out = ["clang"]
    skip = False
    for a in argv[1:]:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a.startswith("-Wp,-M") or a.startswith("-Wp,-MD") or a == "-pg" or a.startswith("-fplugin"):
            continue
        out.append(a)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel-dir", default="kernel")
    ap.add_argument("--tests-dir", default="tests/unit")
    args = ap.parse_args()
    moddir = os.path.realpath(args.kernel_dir)  # kbuild ran the compiler from here
    project = os.path.realpath(os.path.join(args.kernel_dir, ".."))
    bad = checked = failed = 0
    for cmd in sorted(set(commands(args.kernel_dir, args.tests_dir))):
        argv = adapt(cmd)
        dropped = set()
        for _ in range(60):
            run = [a for a in argv if a not in dropped] + ["-fsyntax-only", "-o", "/dev/null"] + EXTRA
            r = subprocess.run(run, cwd=moddir, capture_output=True, text=True)
            new = {m.group(1) for m in REJECTED.finditer(r.stderr)} - dropped
            if not new:
                break
            dropped |= new
        source = [a for a in argv if a.endswith(".c")][-1]
        name = os.path.relpath(os.path.realpath(os.path.join(moddir, source)), project)
        # Only diagnostics in this project's own files count; the kernel's headers are not ours.
        mine = [ln for ln in r.stderr.splitlines()
                if re.match(r"\S+:\d+:\d+: (warning|error)", ln) and os.path.realpath(ln.split(":")[0]).startswith(project)]
        checked += 1
        errors = [ln for ln in r.stderr.splitlines() if re.search(r": error:", ln)]
        if r.returncode != 0 and not mine:
            # Clang could not compile the file at all, so it said nothing about it: that is a failure too.
            failed += 1
            print(f"== {name}: clang failed")
            print("\n".join(errors[:3]) or r.stderr[-300:])
        if mine:
            bad += len(mine)
            print(f"== {name}")
            print("\n".join(mine))
    print(f"clang_check: {checked} sources, {bad} warnings or errors in project files, {failed} not compiled")
    return 1 if bad or failed else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Fail if a Markdown file has a broken relative link. Fenced code blocks are skipped."""
import os
import re
import sys

LINK = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
# "../CS331-" points at sibling course repositories (DESIGN_PLAN.md), outside this checkout.
SKIP_PREFIXES = ("http://", "https://", "mailto:", "../CS331-")


def main():
    errors = 0
    for root, dirs, files in os.walk("."):
        dirs[:] = [d for d in dirs if d not in (".git", "target", "node_modules")]
        for name in files:
            if not name.endswith(".md"):
                continue
            path = os.path.join(root, name)
            in_fence = False
            with open(path, encoding="utf-8", errors="ignore") as md:
                for number, line in enumerate(md, 1):
                    if line.lstrip().startswith("```"):
                        in_fence = not in_fence
                    if in_fence:
                        continue
                    for match in LINK.finditer(line):
                        target = match.group(2).split("#")[0]
                        if not target or target.startswith(SKIP_PREFIXES):
                            continue
                        resolved = os.path.normpath(os.path.join(root, target))
                        if not os.path.exists(resolved):
                            print(f"Broken link in {path}:{number}: {target} -> {resolved}")
                            errors += 1
    if errors:
        sys.exit(1)
    print("All relative documentation links resolved.")


if __name__ == "__main__":
    main()

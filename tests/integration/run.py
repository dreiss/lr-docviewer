#!/usr/bin/env python3
"""Runs pytest, preloading the ASan runtime if the core under test needs it.

An ASan-instrumented core can only be loaded into a process
whose first library is the ASan runtime.
Python isn't instrumented, so this preloads the runtime that the core links.
"""

import os
import subprocess
import sys


def asan_runtime(core: str) -> str | None:
    """Returns the path of the ASan runtime that the core links, if any.

    GCC links the runtime as a shared library dependency.
    Clang leaves it to the executable, so this finds nothing for Clang builds.
    """
    linked = subprocess.run(["ldd", core], check=True, capture_output=True, text=True).stdout
    for line in linked.splitlines():
        name, _, location = line.strip().partition(" => ")
        if name.startswith("libasan.so"):
            return location.split(" (")[0]
    return None


def main() -> None:
    environment = dict(os.environ)
    core = environment.get("DOCVIEWER_CORE")
    runtime = asan_runtime(core) if core else None
    if runtime:
        preload = [runtime, environment.get("LD_PRELOAD", "")]
        environment["LD_PRELOAD"] = " ".join(p for p in preload if p)
        # CPython's own allocations would swamp LeakSanitizer's report.
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    command = [sys.executable, "-m", "pytest", *sys.argv[1:]]
    os.execve(sys.executable, command, environment)


if __name__ == "__main__":
    main()

gwatch — Global Variable Watcher
=================================

A small command-line utility that launches a process and prints every read and/or write access to a specified global integer variable.

This repository contains:
- `gwatch.cpp` — Linux ptrace-based watcher (uses hardware debug registers, DR0/DR7).
- `sample_target.cpp` — small sample program that reads/writes a global `watched` integer (used by tests).
- `autotest.sh` — autotest script that builds both programs and runs functional + perf checks.

Notes and limitations
---------------------
- Current implementation targets x86_64 Linux (including WSL). It uses `ptrace` + hardware breakpoints (DR registers).
- Tracing every access to a variable has significant inherent overhead (context switches and kernel traps). For very short target workloads the tracer can be many× slower than the direct run. This is expected for ptrace-based tracing.
- You need standard GNU tooling available: `g++`, `nm`, `timeout`, `readlink`, etc.
- You may need kernel ptrace permissions. On some Linux systems you must either run the tracer as the same user as the target and/or adjust `/proc/sys/kernel/yama/ptrace_scope`. Running under WSL generally works.

Build
-----
From the repository root:

```bash
# Build gwatch and the sample program
g++ -std=c++14 -O2 -Wall -g -o gwatch gwatch.cpp
g++ -std=c++14 -O2 -Wall -g -o sample_target sample_target.cpp
```

Running gwatch
--------------
Usage:

```text
gwatch --var <symbol> --exec <path> [-- arg1 arg2 ...]
```

Examples:

```bash
# Launch the sample target and print every access to "watched"
./gwatch --var watched --exec ./sample_target

# Pass arguments to the target program after `--`
./gwatch --var some_global --exec ./myprog -- --option value
```

Output format (space-separated, numbers in decimal):

```
<symbol>    write    <old-val> -> <new-val>
<symbol>    read     <val>
```

Running tests / autotest
------------------------
The repository contains `autotest.sh` that performs a functional check and a simple performance comparison. By default it builds both programs, runs the sample target directly to measure baseline time, then runs `gwatch` (timed) and checks the slowdown ratio.

Make the script executable and run it:

```bash
chmod +x autotest.sh
./autotest.sh
```

Notes about the autotest:
- The autotest uses `nm` to find the symbol in the sample program; the `sample_target` must be compiled with symbol information (default build in the script preserves symbols).
- The timed `gwatch` invocation is wrapped with `timeout` to avoid very long runs. You can configure the timeout by setting the environment variable `GWATCH_TIMEOUT` (seconds). For example, to allow 5 minutes:

```bash
export GWATCH_TIMEOUT=300
./autotest.sh
```

Troubleshooting
---------------
- "gwatch appears to hang during the timed run": this usually means the target is being traced and the traced run is legitimately slow because every access generates a trap. Try:
	- Running `./gwatch --var watched --exec ./sample_target` without redirect to see diagnostic messages.
	- Running the tracer with `timeout` or a larger test workload (increase `ITER` in `sample_target.cpp`).
	- Using `ps`, `top`, or `strace -f -p <pid>` on `gwatch` to inspect activity.

- "Symbol not found" from `nm`:
	- Ensure the executable has a global symbol with exactly that name (C++ name mangling can change symbol names — use `nm -C` for demangled output and verify the symbol exact text). The included `sample_target` has a plain `volatile int watched` global to make testing easy.

- Permissions and ptrace:
	- If you get permission errors or PTRACE failures, check `/proc/sys/kernel/yama/ptrace_scope` (value 1 restricts ptrace). On some systems you may need to run as root or adjust that setting.
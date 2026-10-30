#!/usr/bin/env python3
"""
Runs every configuration of Lab 1, saves the results to results/results.csv
and draws the charts for the report.

  python3 run_all.py hw                         # hardware description -> results/hardware.txt
  python3 run_all.py data                       # build numbers.txt + the 8 parts
  python3 run_all.py 1a 1b 1c                   # Task 1 (or: python3 run_all.py task1)
  python3 run_all.py 2 --usb /Volumes/USBNAME   # Task 2, interactive (replug between runs)
  python3 run_all.py charts                     # PNG charts + results/tables.md

Every configuration is a separate process, so the peak memory belongs to that
configuration only. Task 1a/1c are run --repeats times (default 3) and the
median is used for the charts; all raw runs stay in the CSV.
"""
import argparse
import csv
import os
import platform
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RESULTS = HERE / "results"
CSV_PATH = RESULTS / "results.csv"
THREADS = [1, 2, 4, 8, 16, 32, 64]
USB_THREADS = [1, 2, 4]
FIELDS = ["task", "version", "run", "threads", "time_ms", "peak_mb", "neg", "pos", "zero",
          "total", "check", "lost", "reader_push_wait_ms", "max_queue", "files",
          "sum_read_ms", "sum_parse_ms"]


def exe_path(args):
    if args.exe:
        return str(Path(args.exe).resolve())
    name = "lab1.exe" if os.name == "nt" else "lab1"
    p = HERE / name
    if not p.exists():
        sys.exit(f"{p} not found - build it first (see README.md)")
    return str(p)


def run(cmd):
    print("  $", " ".join(cmd), flush=True)
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout, out.stderr)
        sys.exit("command failed")
    rows = []
    for line in out.stdout.splitlines():
        if line.startswith("RESULT "):
            row = dict(kv.split("=", 1) for kv in line.split()[1:])
            rows.append(row)
            print("   ", line[7:], flush=True)
            if row.get("check") == "FAIL" and not (row.get("task") == "1b" and row.get("version") == "1"):
                print("    !!! WRONG TOTALS - this run does not count !!!")
    return rows


def save(rows):
    RESULTS.mkdir(exist_ok=True)
    new = not CSV_PATH.exists()
    with open(CSV_PATH, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS, extrasaction="ignore")
        if new:
            w.writeheader()
        for r in rows:
            w.writerow(r)


def clear_task(task):
    """Remove old rows of a task so re-running it replaces them."""
    if not CSV_PATH.exists():
        return
    with open(CSV_PATH, newline="") as f:
        rows = [r for r in csv.DictReader(f) if r["task"] != task]
    with open(CSV_PATH, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)


# --------------------------------------------------------------------------- tasks

def task_data(args):
    exe = exe_path(args)
    if not Path(args.data).exists():
        subprocess.run([exe, "gen", "--out", args.data], check=True)
    else:
        print(f"{args.data} already exists, not regenerating")
    Path(args.parts).mkdir(exist_ok=True)
    subprocess.run([exe, "split", "--in", args.data, "--outdir", args.parts], check=True)
    print(f"\nNow copy everything from '{args.parts}/' (8 part files + expected.counts) to the USB drive.")


def task_1a(args):
    exe = exe_path(args)
    clear_task("1a")
    print("Task 1a - warm-up run (fills the file cache, not saved)")
    run([exe, "1a", "--in", args.data, "--threads", "1"])
    for t in THREADS:
        rows = []
        for r in range(args.repeats):
            rows += run([exe, "1a", "--in", args.data, "--threads", str(t)])
        for i, row in enumerate(rows):
            row["run"] = str(i + 1)
        save(rows)


def task_1b(args):
    exe = exe_path(args)
    clear_task("1b")
    print(f"Task 1b - {os.cpu_count()} logical processors")
    save(run([exe, "1b", "--in", args.data, "--version", "all", "--runs1", "3"]))


def task_1c(args):
    exe = exe_path(args)
    clear_task("1c")
    print("Task 1c")
    for t in THREADS:
        rows = []
        for r in range(args.repeats):
            rows += run([exe, "1c", "--in", args.data, "--threads", str(t)])
        for i, row in enumerate(rows):
            row["run"] = str(i + 1)
        save(rows)


def task_2(args):
    if not args.usb:
        sys.exit("give the folder on the USB drive: --usb /Volumes/NAME  (Windows: --usb E:\\)")
    exe = exe_path(args)
    clear_task("2")
    print("Task 2 - files are read ONLY from", args.usb)
    if platform.system() == "Darwin":
        print("macOS: disable Spotlight for the drive once:  sudo mdutil -i off", args.usb)
    for t in USB_THREADS:
        input(f"\n>>> Eject the USB drive, UNPLUG it, plug it back in, then press Enter "
              f"(next run: {t} thread(s))...")
        while not Path(args.usb).exists():
            input(f"    {args.usb} is not mounted yet - press Enter when it is...")
        save(run([exe, "2", "--dir", args.usb, "--threads", str(t)]))


def task_hw(args):
    RESULTS.mkdir(exist_ok=True)
    lines = [f"OS: {platform.platform()}",
             f"Python (runner only): {platform.python_version()}",
             f"Logical processors (os.cpu_count): {os.cpu_count()}"]
    sysname = platform.system()

    def sh(cmd):
        try:
            return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=20).stdout.strip()
        except Exception as e:  # noqa
            return f"(failed: {e})"

    if sysname == "Darwin":
        disk_cmd = 'diskutil info / | grep -E "Device / Media Name|Solid State|Protocol"'
        disk_info = sh(disk_cmd)
        lines += [f"CPU: {sh('sysctl -n machdep.cpu.brand_string')}",
                  f"Physical cores: {sh('sysctl -n hw.physicalcpu')}",
                  f"Logical processors: {sh('sysctl -n hw.logicalcpu')}",
                  f"Performance levels (P/E cores): {sh('sysctl hw.perflevel0.physicalcpu hw.perflevel1.physicalcpu 2>/dev/null')}",
                  f"RAM bytes: {sh('sysctl -n hw.memsize')}",
                  "Disk: " + disk_info,
                  f"macOS: {sh('sw_vers')}",
                  "USB drives:\n" + sh("system_profiler SPUSBDataType 2>/dev/null | head -60")]
    elif sysname == "Linux":
        lines += [sh("lscpu | grep -E 'Model name|^CPU\\(s\\)|Thread|Core|Socket'"),
                  sh("free -h | head -2"),
                  sh("lsblk -d -o NAME,MODEL,ROTA,TRAN,SIZE"),
                  sh("lsusb 2>/dev/null")]
    elif sysname == "Windows":
        lines += [sh('powershell -NoProfile "Get-CimInstance Win32_Processor | Select Name,NumberOfCores,NumberOfLogicalProcessors | Format-List"'),
                  sh('powershell -NoProfile "Get-CimInstance Win32_ComputerSystem | Select TotalPhysicalMemory | Format-List"'),
                  sh('powershell -NoProfile "Get-PhysicalDisk | Select FriendlyName,MediaType,BusType,Size | Format-Table"')]
    try:
        lines.append(subprocess.run([exe_path(args), "info"], capture_output=True, text=True).stdout.strip())
    except SystemExit:
        pass
    lines.append("\nFILL IN BY HAND: USB drive model/capacity and its USB version (2.0 / 3.x), "
                 "and the USB port version.")
    text = "\n".join(lines)
    (RESULTS / "hardware.txt").write_text(text)
    print(text)
    print(f"\nSaved to {RESULTS / 'hardware.txt'}")


# --------------------------------------------------------------------------- charts

def load_rows():
    if not CSV_PATH.exists():
        sys.exit("no results yet")
    with open(CSV_PATH, newline="") as f:
        return list(csv.DictReader(f))


def medians(rows, task):
    by_t = {}
    for r in rows:
        if r["task"] == task:
            by_t.setdefault(int(r["threads"]), []).append(r)
    out = {}
    for t, rs in sorted(by_t.items()):
        out[t] = {"time": statistics.median(float(r["time_ms"]) for r in rs),
                  "mem": statistics.median(float(r["peak_mb"]) for r in rs),
                  "rows": rs}
    return out


def three_charts(plt, data, title, fname, xs_label="Number of threads"):
    ts = list(data)
    times = [data[t]["time"] for t in ts]
    speed = [times[0] / x for x in times]
    mems = [data[t]["mem"] for t in ts]
    fig, ax = plt.subplots(1, 3, figsize=(15, 4.2))
    for a, ys, yl, col in [(ax[0], times, "Time (ms)", "#2a6fdb"),
                           (ax[1], speed, "Speed-up vs 1 thread", "#1a9e5c"),
                           (ax[2], mems, "Peak memory (MiB)", "#d9822b")]:
        a.plot(ts, ys, marker="o", color=col, linewidth=2)
        a.set_xscale("log", base=2)
        a.set_xticks(ts)
        a.set_xticklabels([str(t) for t in ts])
        a.set_xlabel(xs_label)
        a.set_ylabel(yl)
        a.grid(alpha=0.3)
        for x, y in zip(ts, ys):
            a.annotate(f"{y:.2f}" if yl.startswith("Speed") else f"{y:.0f}", (x, y),
                       textcoords="offset points", xytext=(0, 7), ha="center", fontsize=8)
    ax[1].plot(ts, ts, "--", color="grey", alpha=0.5, label="ideal")
    ax[1].set_ylim(0, max(max(speed) * 1.3, 2))
    ax[1].legend()
    fig.suptitle(title, fontsize=13)
    fig.tight_layout()
    fig.savefig(RESULTS / fname, dpi=150)
    plt.close(fig)
    return ts, times, speed, mems


def task_charts(args):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    rows = load_rows()
    md = ["# Results tables (generated by run_all.py charts)\n"]

    tasks_present = {r["task"] for r in rows}

    for task, title, fname in [("1a", "Task 1a - counting with threads (file split into parts)", "task1a.png"),
                               ("1c", "Task 1c - producer and consumers (bounded queue)", "task1c.png"),
                               ("2", "Task 2 - 8 files on the USB drive", "task2.png")]:
        if task not in tasks_present:
            continue
        data = medians(rows, task)
        ts, times, speed, mems = three_charts(plt, data, title, fname,
                                              "Number of workers" if task == "1c" else "Number of threads")
        md.append(f"## Task {task}\n")
        md.append(f"![Task {task}]({fname})\n")
        extra_h = " Reader wait (ms) |" if task == "1c" else (" Σ read (ms) | Σ parse (ms) |" if task == "2" else "")
        md.append(f"| Threads | Time (ms) | Speed-up | Peak memory (MiB) |{extra_h} Totals |")
        md.append("|---:|---:|---:|---:|" + ("---:|" * extra_h.count("|")) + "---|")
        for t, tm, sp, mm in zip(ts, times, speed, mems):
            r = data[t]["rows"][0]
            ok = all(x["check"] in ("OK", "TOTAL_OK") for x in data[t]["rows"])
            extra = ""
            if task == "1c":
                extra = f" {statistics.median(float(x['reader_push_wait_ms']) for x in data[t]['rows']):.0f} |"
            if task == "2":
                extra = f" {float(r['sum_read_ms']):.0f} | {float(r['sum_parse_ms']):.0f} |"
            md.append(f"| {t} | {tm:.0f} | {sp:.2f} | {mm:.1f} |{extra} "
                      f"{r['neg']} / {r['pos']} / {r['zero']} = {r['total']} {'✔' if ok else '✘'} |")
        md.append("")

    # 1a vs 1c on one chart
    if {"1a", "1c"} <= tasks_present:
        a, c = medians(rows, "1a"), medians(rows, "1c")
        fig, ax = plt.subplots(figsize=(7, 4.2))
        ax.plot(list(a), [v["time"] for v in a.values()], marker="o", label="1a: split file")
        ax.plot(list(c), [v["time"] for v in c.values()], marker="s", label="1c: reader + queue")
        ax.set_xscale("log", base=2)
        ax.set_xticks(list(a))
        ax.set_xticklabels([str(t) for t in a])
        ax.set_xlabel("Number of threads / workers")
        ax.set_ylabel("Time (ms)")
        ax.set_ylim(bottom=0)
        ax.grid(alpha=0.3)
        ax.legend()
        ax.set_title("Task 1a vs Task 1c")
        fig.tight_layout()
        fig.savefig(RESULTS / "task1a_vs_1c.png", dpi=150)
        plt.close(fig)
        md.append("## Task 1a vs 1c\n\n![1a vs 1c](task1a_vs_1c.png)\n")

    # 1b bar chart
    b = [r for r in rows if r["task"] == "1b"]
    if b:
        labels = [f"V{r['version']}" + (f" run {r['run']}" if r["version"] == "1" else "") for r in b]
        names = {"1": "no sync", "2": "mutex", "3": "per-thread"}
        labels = [f"{l}\n({names[r['version']]})" for l, r in zip(labels, b)]
        times = [float(r["time_ms"]) for r in b]
        colors = ["#d64545" if r["check"] == "FAIL" else "#1a9e5c" for r in b]
        fig, ax = plt.subplots(1, 2, figsize=(13, 4.5))
        bars = ax[0].bar(labels, times, color=colors)
        ax[0].set_yscale("log")
        ax[0].set_ylabel("Time (ms, log scale)")
        ax[0].set_title(f"Task 1b - time ({b[0]['threads']} threads); red = wrong totals")
        for bar, t in zip(bars, times):
            ax[0].annotate(f"{t:.0f}", (bar.get_x() + bar.get_width() / 2, t), ha="center",
                           xytext=(0, 3), textcoords="offset points", fontsize=8)
        totals = [int(r["total"]) / 1e6 for r in b]
        bars = ax[1].bar(labels, totals, color=colors)
        ax[1].axhline(50, color="black", linestyle="--", linewidth=1)
        ax[1].set_ylabel("Counted numbers (millions)")
        ax[1].set_title("Task 1b - totals (dashed line = 50 000 000)")
        for bar, t in zip(bars, totals):
            ax[1].annotate(f"{t:.2f}M", (bar.get_x() + bar.get_width() / 2, t), ha="center",
                           xytext=(0, 3), textcoords="offset points", fontsize=8)
        fig.tight_layout()
        fig.savefig(RESULTS / "task1b.png", dpi=150)
        plt.close(fig)
        md.append(f"## Task 1b ({b[0]['threads']} threads)\n\n![Task 1b](task1b.png)\n")
        md.append("| Version | Run | Time (ms) | neg | pos | zero | total | lost updates | correct |")
        md.append("|---|---:|---:|---:|---:|---:|---:|---:|---|")
        for r in b:
            md.append(f"| {r['version']} ({names[r['version']]}) | {r['run']} | {float(r['time_ms']):.0f} | "
                      f"{r['neg']} | {r['pos']} | {r['zero']} | {r['total']} | {r['lost']} | "
                      f"{'yes' if r['check'] != 'FAIL' else 'NO'} |")
        md.append("")

    (RESULTS / "tables.md").write_text("\n".join(md))
    print(f"Charts and {RESULTS / 'tables.md'} written.")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("steps", nargs="+",
                   help="hw | data | 1a | 1b | 1c | task1 | 2 | charts")
    p.add_argument("--exe", help="path to the compiled lab1 program")
    p.add_argument("--data", default=str(HERE / "numbers.txt"))
    p.add_argument("--parts", default=str(HERE / "parts"))
    p.add_argument("--usb", help="folder on the USB drive that holds numbers_part_*.txt")
    p.add_argument("--repeats", type=int, default=3)
    args = p.parse_args()

    steps = []
    for s in args.steps:
        steps += ["1a", "1b", "1c"] if s == "task1" else [s]
    table = {"hw": task_hw, "data": task_data, "1a": task_1a, "1b": task_1b, "1c": task_1c,
             "2": task_2, "charts": task_charts}
    for s in steps:
        if s not in table:
            sys.exit(f"unknown step {s}")
        table[s](args)


if __name__ == "__main__":
    main()

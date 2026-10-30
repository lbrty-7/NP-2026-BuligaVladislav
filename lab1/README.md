# Lab 1 — Concurrency in Practice

Buliga Vladislav, FAF-243. Language: **C++17** with `std::thread` (OS threads).

## Build

| OS | Command |
|---|---|
| macOS | `clang++ -O2 -std=c++17 -pthread lab1.cpp -o lab1` |
| Linux | `g++ -O2 -std=c++17 -pthread lab1.cpp -o lab1` |
| Windows (MSVC, Developer Prompt) | `cl /O2 /std:c++17 /EHsc lab1.cpp psapi.lib` |
| Windows (MinGW) | `g++ -O2 -std=c++17 lab1.cpp -o lab1.exe -lpsapi` |

## Run one configuration by hand

```
./lab1 gen   --out numbers.txt                 # 50 000 000 numbers + numbers.txt.counts
./lab1 split --in numbers.txt --outdir parts   # 8 x 6 250 000 numbers, in order
./lab1 1a    --in numbers.txt --threads 8
./lab1 1b    --in numbers.txt                  # threads = logical processors; V1 x3, V2, V3
./lab1 1c    --in numbers.txt --threads 8      # 1 reader + 8 workers
./lab1 2     --dir /Volumes/USB --threads 4    # reads only from the USB drive
```

Each run prints a line `RESULT task=... time_ms=... peak_mb=... neg=... pos=... zero=... total=... check=OK`.
`check=OK` means the counts equal the ones recorded when the file was generated
(the correctness check: neg + pos + zero = 50 000 000, same in every task).

## Run everything (recommended)

Requires Python 3 + matplotlib (`pip install matplotlib`).

```
python3 run_all.py hw            # hardware -> results/hardware.txt
python3 run_all.py data          # numbers.txt + parts/ (copy parts/* to the USB drive)
python3 run_all.py task1         # 1a, 1b, 1c; 1a and 1c 3 times each, median is plotted
python3 run_all.py 2 --usb /Volumes/USBNAME   # asks you to replug the drive before each run
python3 run_all.py charts        # results/*.png + results/tables.md
```

Close other heavy programs and keep the laptop on the charger while measuring.

## What each part measures

* **Time** — `steady_clock` around the counting work (1a: from opening the file to the final sum,
  including thread creation; 1b: only the counting, loading the array is not measured;
  1c: the whole pipeline; 2: reading + parsing all 8 files).
* **Peak memory** — the RSS high-water mark of the process (Linux `VmHWM`, macOS
  `resident_size_max`, Windows `PeakWorkingSetSize`). Each configuration is a separate process.

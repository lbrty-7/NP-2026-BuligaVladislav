# Laboratory Work 1: Concurrency in Practice

**Student:** Buliga Vladislav, group FAF-243
**Course:** Network Programming (PR), Technical University of Moldova, FCIM
**Checked by:** ⟨name⟩
**Date:** 30.10.2026

---

## 1. Hardware and software

| Item | Value |
|---|---|
| Machine | MacBook Pro |
| CPU | Apple M4 Pro |
| Physical cores / logical processors | 12 / 12 — **8 performance (P) + 4 efficiency (E) cores**, no simultaneous multithreading (1 logical processor per core) |
| RAM | 24 GB unified memory |
| Internal disk | Apple SSD (Apple Fabric, solid state) |
| USB flash drive | Kingston, 64 GB (volume `KINGSTON`); measured sequential read ≈ 90 MB/s, i.e. a USB 3.x drive |
| Operating system | macOS 26.6.2 (build 25G83), arm64 |
| Language / compiler | C++17, Apple clang 17.0.0 (clang-1700.6.4.2), `-O2 -pthread` |
| Threads | `std::thread` — native OS threads (POSIX threads) |
| Measurement scripts | Python 3.11.9 + matplotlib (runs the program and draws the charts only) |

---

## 2. Implementation

All tasks are in one program, `lab1.cpp`; the task and its configuration are chosen on the command line
(`lab1 1a --threads 8`, `lab1 2 --dir /Volumes/KINGSTON/PR-lab1 --threads 4`, …). `run_all.py` runs every
configuration as a **separate process**, writes `results/results.csv` and draws the charts.

* **Time** — `std::chrono::steady_clock` around the measured work.
* **Peak memory** — the peak resident set size of the process (`resident_size_max` from the Mach task info).
  Every configuration is a separate process, so the value belongs to that configuration only.
* Tasks 1a and 1c were run 3 times per configuration; the charts and tables use the **median**.
  The very first run of 1a was a warm-up, so that all runs read `numbers.txt` from the same (cached) state.

**Data.** `lab1 gen` wrote 50 000 000 integers in [−1 000 000, 1 000 000] (`std::mt19937_64`, seed 42,
`uniform_int_distribution`) to `numbers.txt` (≈ 370 MB) and saved the expected counts to
`numbers.txt.counts`. `lab1 split` split the file, in order, into 8 files of 6 250 000 lines.

**Correctness check.** Every run compares its counts with the saved ones and prints `check=OK`.
Expected (and obtained in every correct run of every task):

| Negative | Positive | Zero | Total |
|---:|---:|---:|---:|
| 24 997 972 | 25 002 011 | 17 | **50 000 000** |

**Task 1a.** The file of size *S* is divided into *N* byte ranges `[S·t/N, S·(t+1)/N)`. Each thread opens
its own file handle, seeks to its start and reads only its range with a 1 MiB buffer, parsing as it reads.
A thread owns every line that **starts** inside its range (details in control question 2).

**Task 1b.** The 50 M numbers are loaded once into a `std::vector<int32_t>` (not measured) and counted with
12 threads (= logical processors), each on an equal slice of the array:
1. shared counters incremented with no synchronisation (`*pos = *pos + 1`; the pointer is `volatile`
   only so that the compiler cannot keep the counter in a register for the whole loop and hide the race);
2. shared counters, one `std::mutex` locked around every increment;
3. local counters in every thread, written once into a per-thread slot and added after `join()`.

**Task 1c.** One reader thread reads `numbers.txt` line by line (`std::getline`) and pushes batches of
10 000 lines (`std::vector<std::string>`) into my own `BoundedQueue` (a `std::deque` protected by a
`std::mutex`, with two `std::condition_variable`s, capacity 100). `push` waits while the queue is full,
`pop` waits while it is empty. At end of file the reader calls `close()`; `pop` returns "nothing" once the
queue is closed **and** empty, and the worker stops. Each worker counts into local counters.

**Task 2.** The 8 part files are on the USB drive. Files are given out round-robin (thread *t* takes files
*t, t+N, …*: 8 / 4+4 / 2+2+2+2). A thread reads a whole file with **one** `fread` into one
`std::vector<char>` and then parses it. Read time and parse time are measured separately.
Before every run the drive was ejected, unplugged and plugged back in; Spotlight indexing was disabled
for the drive (`mdutil -i off`).

---

## 3. Task 1 — one file

### 3.1 Task 1a — counting with threads

![Task 1a](results/task1a.png)

| Threads | Time (ms) | Speed-up | Peak memory (MiB) | Totals |
|---:|---:|---:|---:|---|
| 1 | 424 | 1.00 | 2.5 | 24 997 972 / 25 002 011 / 17 = 50 000 000 ✔ |
| 2 | 222 | 1.91 | 3.5 | ✔ |
| 4 | 129 | 3.28 | 5.5 | ✔ |
| **8** | **92** | **4.62** | 9.7 | ✔ |
| 16 | 106 | 4.02 | 18.0 | ✔ |
| 32 | 110 | 3.86 | 34.5 | ✔ |
| 64 | 111 | 3.82 | 63.4 | ✔ |

### 3.2 Task 1b — three ways to count (12 threads)

![Task 1b](results/task1b.png)

| Version | Run | Time (ms) | Negative | Positive | Zero | Total | Lost updates | Correct |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| 1 — no synchronisation | 1 | 15 | 3 074 471 | 3 069 663 | 17 | 6 144 151 | 43 855 849 | **no** |
| 1 — no synchronisation | 2 | 12 | 2 579 040 | 2 582 229 | 17 | 5 161 286 | 44 838 714 | **no** |
| 1 — no synchronisation | 3 | 15 | 3 490 203 | 3 482 884 | 17 | 6 973 104 | 43 026 896 | **no** |
| 2 — mutex | 1 | 797 | 24 997 972 | 25 002 011 | 17 | 50 000 000 | 0 | yes |
| 3 — per-thread counters | 1 | 3.6 | 24 997 972 | 25 002 011 | 17 | 50 000 000 | 0 | yes |

### 3.3 Task 1c — producer and consumers

![Task 1c](results/task1c.png)

| Workers | Time (ms) | Speed-up | Peak memory (MiB) | Reader waited on full queue (ms) | Max. batches in queue | Totals |
|---:|---:|---:|---:|---:|---:|---|
| 1 | 1117 | 1.00 | 3.0 | 3 | 1 | ✔ |
| 2 | 1114 | 1.00 | 3.5 | 3 | 1 | ✔ |
| 4 | 1119 | 1.00 | 3.1 | 3 | 1 | ✔ |
| 8 | 1120 | 1.00 | 3.2 | 3 | 1 | ✔ |
| 16 | 1114 | 1.00 | 3.3 | 3 | 1 | ✔ |
| 32 | 1125 | 0.99 | 3.6 | 3 | 1 | ✔ |
| 64 | 1128 | 0.99 | 4.2 | 3 | 1 | ✔ |

![Task 1a vs Task 1c](results/task1a_vs_1c.png)

### 3.4 Answers

**1. At which number of threads does the speed-up stop growing?**
At **8 threads** (92 ms, speed-up 4.62). The M4 Pro has 12 cores and 12 logical processors (no SMT),
but only **8 of them are performance cores**; the other 4 are slower efficiency cores. Up to 8 threads every
thread gets its own fast core and the time keeps falling (1.91× at 2, 3.28× at 4, 4.62× at 8). With 16
threads the work no longer fits on the P-cores: some threads run on E-cores or share a core, and the time
goes up again (106 ms).
The speed-up at 8 threads (4.62) is below the ideal 8 because the threads are not fully independent: all of
them copy data from the same file cache through the same memory system (370 MB in 92 ms ≈ 4 GB/s), and
creating the threads, opening the file 8 times and summing the results is a fixed cost that does not shrink.

**2. Why do 32 and 64 threads not help? Are they slower, and why?**
Yes, they are slower than 8 threads: 110 ms and 111 ms against 92 ms. Only 12 threads can run at the same
moment, so the rest wait for a core, and the amount of work is the same — extra threads only add overhead:
creating and joining threads, context switches, a 1 MiB buffer and an extra file handle per thread, and a
partial line to skip at every boundary. The most important reason on this CPU is the **static division of the
work**: every thread gets an equal part, and the program ends only when the *slowest* thread ends. Once there
are more threads than P-cores, some parts are processed on the slower E-cores (or wait for a core), and every
thread waits for them.

**3. Does the peak memory depend on the number of threads? Why?**
Yes, linearly: 2.5 MiB with 1 thread, 9.7 MiB with 8, 63.4 MiB with 64 — about **+0.97 MiB per thread**.
Each thread allocates its own 1 MiB read buffer (plus the used part of its stack). The numbers themselves are
never stored — each thread streams through its part — so the memory depends on the number of threads, not on
the size of the file.

**4. Rank the three versions of Task 1b by time. Which of them are correct?**

| Rank | Version | Time | Correct |
|---|---|---|---|
| 1 | 3 — per-thread counters | 3.6 ms | yes |
| 2 | 1 — no synchronisation | 12–15 ms | **no** |
| 3 | 2 — mutex | 797 ms | yes |

* **Version 3** is correct and the fastest: during the loop nothing is shared, every thread counts in its own
  registers, and the 12 partial results are added once at the end.
* **Version 1** is wrong: it lost 43–45 million of the 50 million increments (86–90 %), and the result is
  different in every run (6.14 M, 5.16 M, 6.97 M). `counter = counter + 1` is three steps — read, add,
  write — and when two threads interleave them, one increment overwrites the other (control question 3).
  With 12 threads hitting the same three counters almost every increment collides. It is also ~4× slower
  than version 3 although it never waits for a lock: the three counters lie in **one cache line**, and every
  write by one core invalidates that line in the other 11 cores, so the line keeps moving between cores
  (cache-line ping-pong / contention).
* **Version 2** is correct but ~220× slower than version 3: it performs 50 million lock/unlock pairs, and since
  every increment needs the same lock, the 12 threads mostly wait for each other — the work becomes
  sequential, plus the cost of the atomic operations and of putting waiting threads to sleep and waking them.

**5. Compare Task 1c with Task 1a. Which one is faster, and why? At which number of workers does Task 1c
stop getting faster?**
Task 1a is much faster: 92 ms at its best against ≈ 1115 ms for 1c (12×); even 1a on **one** thread (424 ms)
is 2.6× faster than 1c with any number of workers.
In 1c all the reading is done by **one** thread, and it is the expensive part: for each of the 50 M lines
`std::getline` checks the stream, copies the characters into a `std::string` and the string is moved into the
batch. Workers only parse short strings, which is much cheaper. So the reader is the bottleneck, which the
measurements show directly: the queue never held more than **1** batch (out of 100) and the reader waited for
free space only ≈ 3 ms in total — the workers always take a batch as soon as it appears and then wait for the
next one.
Task 1c **does not get faster at all**: the time is ≈ 1115 ms already with 1 worker and stays the same up to
64 (speed-up 0.99–1.00). One worker is enough to keep up with the reader; more workers only wait on an
empty queue. In 1a, by contrast, both reading and parsing are divided among the threads.
Memory in 1c stays at 3–4 MiB for any number of workers, because only one or two batches
(10 000 short strings ≈ 240 KB each) exist at any time.

**6. What would happen with batches of a single line? And with a queue that has no size limit?**
* **Batches of one line:** the queue would carry 50 million items instead of 5 000. Each `push`/`pop` locks the
  mutex and signals a condition variable, and a sleeping worker has to be woken up. That is the situation of
  Task 1b version 2 (50 M lock operations = 797 ms) plus the wake-ups, so synchronisation would dominate and
  1c would become several times slower. Batching amortises the cost of one synchronisation over 10 000 lines.
* **Unbounded queue:** the bound is what stops a fast producer (back-pressure). On my machine the reader is the
  slower side, so the queue stays almost empty and nothing would change. But if the workers were slower (more
  work per number, or a faster source), the reader would put the whole file into memory —
  50 M `std::string` objects ≈ 1.2 GB plus vector overhead — so the peak memory would grow with the size of
  the input and could exhaust RAM. With the limit of 100 batches the queue can never hold more than
  1 000 000 lines.

---

## 4. Task 2 — many files on slow storage

![Task 2](results/task2.png)

| Threads | Time (ms) | Speed-up | Peak memory (MiB) | Σ read time over threads (ms) | Σ parse time (ms) | Totals |
|---:|---:|---:|---:|---:|---:|---|
| 1 | 4372 | 1.00 | 45.6 | 4112 | 260 | 24 997 972 / 25 002 011 / 17 = 50 000 000 ✔ |
| 2 | 4127 | 1.06 | 89.7 | 7542 | 260 | ✔ |
| 4 | 4108 | 1.06 | 178.0 | 13 473 | 260 | ✔ |

*Note.* A first series gave 9 138 ms for 2 threads, while 1 and 4 threads gave the same values as above.
The whole series was repeated (with the same procedure: eject, unplug, plug in before every run) and the
repeated values, shown in the table, are consistent with each other. The outlier shows that a single
measurement on a USB flash drive can be disturbed by the drive itself.

### Answers

**1. Compare the time on one thread with Task 1a on one thread. On the USB drive, is the work CPU-bound or
I/O-bound?**

| One thread | Total time | Reading | Parsing |
|---|---:|---:|---:|
| Task 1a, internal SSD (file cache) | 424 ms | — (interleaved) | — |
| Task 2, USB drive | 4372 ms | 4112 ms (**94 %**) | 260 ms (6 %) |

The same 50 M numbers take **10.3× longer** from the USB drive. Parsing all of them takes only 260 ms
(less than the whole of Task 1a), so the extra ≈ 3.9 s is spent entirely **waiting for the drive**:
94 % of the time is reading, 6 % is computing. ≈ 370 MB in 4.1 s ≈ 90 MB/s, which is the sequential read
speed of the drive (this also shows it is a USB 3.x drive — USB 2.0 cannot exceed ≈ 40 MB/s in practice).
Making the CPU part faster could save at most 6 % — so on the USB drive the work is **I/O-bound**.

**2. On the USB drive, do more threads help? Why not, when lecture 1 showed that I/O-bound work benefits
from concurrency?**
Practically no: 4372 → 4127 → 4108 ms, a speed-up of only 1.06. The only gain (≈ 250 ms) is that parsing of
one file now overlaps with reading another file — exactly the 260 ms of parse time.
The reading itself does not get faster, because all threads read from **one device through one USB link**
whose throughput (≈ 90 MB/s) is fixed. Two or four parallel reads just share this throughput. The
measurements show it clearly: the total time stays ≈ 4.1 s, but the **sum** of the read times of all threads
grows almost proportionally with the number of threads (4.1 → 7.5 → 13.5 s) — every thread waits longer
because it waits for the others.
Concurrency helps I/O-bound work when the waiting is **latency of independent operations**, as in lecture 1
(e.g. many requests to different servers: while one waits for its answer, the others can be sent, and the
waits overlap). Here the limit is not latency but the **bandwidth of a single device**, which is already fully
used by one thread. More threads only cost memory: one ≈ 46 MB buffer per thread
(45.6 → 89.7 → 178.0 MiB).

---

## 5. Conclusion

* **CPU-bound:** Tasks 1a, 1b and 1c, where the data came from the internal SSD (after the first read, from the
  operating system's file cache) or from memory. I know it because the time decreased almost in proportion
  to the number of threads as long as there were free fast cores (Task 1a: 4.62× at 8 threads = number of
  P-cores), and stopped improving — even got worse — once all performance cores were busy. In Task 1c the
  limit was also the CPU, but of a single thread: the reader.
* **I/O-bound:** Task 2, reading from the USB flash drive. I know it because 94 % of the time was spent
  reading and only 6 % parsing, the same work took 10× longer than from the SSD, and adding threads did not
  reduce the time (speed-up 1.06), while the threads' waiting time grew with their number.
* Synchronisation has a price: shared counters without synchronisation lost up to 90 % of the updates; a
  mutex fixed it but made the program ~220× slower; giving every thread its own counters was both correct
  and the fastest. The best design is to share as little as possible and combine results once at the end.

---

## 6. Control questions

**1. What is the difference between a physical core and a logical processor? Where did it show in your
charts?**
A physical core is a real, independent set of execution units (ALUs, caches, pipeline). A logical processor is
what the operating system sees as a CPU it can schedule a thread on. With simultaneous multithreading
(Intel Hyper-Threading, AMD SMT) one physical core appears as two logical processors: two threads share the
same execution units and only fill each other's idle cycles, so they give typically +10–30 %, not 2×.
On the Apple M4 Pro there is no SMT: 12 physical cores = 12 logical processors. The difference that matters
here is between the **8 performance and the 4 efficiency cores**. It shows in the Task 1a chart: the
speed-up grows up to **8 threads** (4.62×) and then falls (4.02× at 16), although the OS reports 12
logical processors — the extra threads end up on slower cores.

**2. How did you make sure that no line was counted twice or skipped at the boundary between two parts?**
The rule is: **a thread owns every line that starts inside its byte range** `[start, end)`.
* If `start > 0`, the thread looks at the byte just before `start`. If it is not `\n`, the thread is in the
  middle of a line that started in the previous range, so it skips everything up to and including the next
  `\n`. (If it is `\n`, a line starts exactly at `start` and belongs to this thread.)
* While reading, before starting a **new** line it checks its position: if it is `≥ end`, it stops. A line
  that started before `end` is finished even if it continues past `end`.
So a line crossing a boundary is counted only by the thread in which it starts, and skipped by the next one.
The first range starts at 0 and the last one ends at the end of the file, so nothing is left out.
Verified by the correctness check: the totals are exactly 50 000 000 and identical for 1, 2, 4, 8, 16, 32 and
64 threads (and also for odd numbers like 3 and 7 during testing).

**3. Explain step by step how unsynchronised shared counters lose updates.**
`pos = pos + 1` is not one operation but three: (1) load `pos` from memory into a register, (2) add 1,
(3) store the register back to memory. Example with `pos = 100`:
1. Thread A loads 100.
2. Thread B loads 100 (A has not stored yet).
3. A computes 101 and stores 101.
4. B computes 101 and stores 101.

Two numbers were counted, but `pos` grew by 1 — one update is lost. With 12 threads incrementing the same
counter millions of times, this happens constantly (in my runs 86–90 % of the updates were lost), and the
result depends on the timing, so it is different on every run.

**4. Why are per-thread counters faster than shared counters protected by a lock?**
With a lock, every one of the 50 M increments needs a lock and an unlock (atomic read-modify-write
instructions and memory barriers); only one thread can hold the lock, so the others wait — the work becomes
sequential; the lock's cache line keeps moving between cores; and waiting threads may be put to sleep and
woken up by the OS. With per-thread counters there is nothing shared during the loop: each thread works only in
its registers and its own cache, without waiting for anybody, and synchronises only once, at the end
(`join()` and 12 additions). Measured: 797 ms against 3.6 ms.

**5. What does "I/O-bound" mean for a USB drive, and for a server waiting on the network? Why does concurrency
help in only one of the two cases?**
I/O-bound means that the program spends most of its time waiting for input/output, not computing, so a faster
CPU would not make it faster.
* **USB drive:** the limit is the **throughput** of one device (≈ 90 MB/s here). One thread reading sequentially
  already uses it fully; more threads only divide the same bandwidth (and may make the access less sequential).
* **Server waiting on the network:** each request spends most of its time waiting for **latency** — the
  round trip and the remote side's processing — while the network link and the CPU are almost idle. Many
  requests can be in flight at the same time and their waiting overlaps, so 10 concurrent requests take about
  as long as one.

Concurrency helps when the resource we wait for can serve many requests at once (independent latencies); it
does not help when one shared resource is already saturated.

**6. What is the difference between dividing the work into equal parts in advance and taking it from a queue?
When is each one better?**
* **Equal parts in advance (static division, Task 1a/2):** each thread knows its part from the beginning; there
  is no coordination while working, so the overhead is minimal. But the total time is the time of the
  **slowest** thread: if some parts are harder, or some threads run on slower cores (E-cores) or are delayed by
  other programs, the others finish and sit idle. Better for **uniform work on data that can be split
  directly** (array, file with random access) on a machine with identical, free cores.
* **Queue (dynamic division, Task 1c):** a thread that finishes takes the next batch, so the load balances
  itself automatically — fast threads simply do more batches. It costs synchronisation on every batch, needs
  batches large enough to amortise it, and a single producer can become the bottleneck (as in Task 1c).
  Better for **uneven tasks, unknown amount of work, or data that arrives as a stream** (network, line-by-line
  reading, requests to a server).

---

## 7. Build and run

Source code: `lab1.cpp`, `run_all.py`. Detailed instructions: `README.md`.

```
clang++ -O2 -std=c++17 -pthread lab1.cpp -o lab1                 # macOS (Linux: g++ ...)
python3 run_all.py hw data                                        # hardware info, numbers.txt, 8 parts
python3 run_all.py task1                                          # Task 1a, 1b, 1c
python3 run_all.py 2 --usb /Volumes/KINGSTON/PR-lab1              # Task 2 (replug before each run)
python3 run_all.py charts                                         # charts + tables

./lab1 1a --in numbers.txt --threads 8                            # any single configuration
./lab1 1b --in numbers.txt
./lab1 1c --in numbers.txt --threads 4
./lab1 2  --dir /Volumes/KINGSTON/PR-lab1 --threads 2
```

# franztime

**A lightweight Windows command-line timer that measures CPU, memory, and I/O usage.**

`franztime` is a small, standalone Windows utility designed to measure (approximately) the resource footprint of file archivers and other command-line tools.

It runs a command, waits for it to finish, and reports its execution time and resource usage to `stderr`. It is particularly useful for comparing the performance and resource consumption of compression and archiving software under real-world workloads (of course WITH the overhead of franztime too!).

![franztime screenshot](https://github.com/user-attachments/assets/58b4ba60-8f4b-4152-ae44-45af0ff86422)

## Features

- **Wall-clock time** total elapsed execution time.
- **CPU usage** user time, kernel time, average and peak core usage.
- **CPU cycles** total processor cycles consumed.
- **Memory usage** peak and final working set, private bytes, and private working set.
- **Page faults** number of page faults.
- **I/O statistics** I/O activity, including that of child processes.

CPU and I/O measurements include the command's child processes, thanks to a Windows Job Object. 
The reported exit code is the exit code of the command itself.

## Measurement details

Some metrics are exact Windows-maintained counters, while others are sampled or calculated estimates.

- **Peak working set and peak private bytes** are maintained by Windows and are reported without sampling-based peak estimation.
- **Final values** represent the last sample taken while the process was still known to be running, rather than a sample taken during process termination.
- **Private working set** : the memory metric shown as *Memory* in Windows Task Manager is available only as a sampled value. Its peak is therefore an observed peak, not a guaranteed maximum.
- **Average and peak CPU core usage** are derived from the CPU time consumed during execution. Peak usage depends on the measurement interval and may not capture very brief spikes.
- **Child-process accounting** applies to CPU and I/O measurements through the Windows Job Object.

These measurements are intended for practical comparisons, not as a replacement for a dedicated profiler or a comprehensive system-monitoring tool.

## Usage

Run `franztime` followed by the command and its arguments:

```console
franztime.exe command [arguments...]
```

For example, to measure an archiver:

```console
franztime.exe 7z.exe a archive.7z files\
```

`franztime` waits for the command to finish, prints the measurements to `stderr`, and returns the command's exit code.

Because the report is written to `stderr`, the command's standard output remains separate and can be redirected independently.

## Output colors

When writing to a console, `franztime` uses colors to make the report easier to read:

- **Green**  main measurements.
- **Yellow**  warnings.
- **Red**  errors.

Colors are enabled only when output is directed to a console.

## Building

No external libraries are required for linking, and no DLLs are needed beyond those provided by Windows.

Compile with MinGW-w64 GCC:

```bash
g++ -O2 -Wall franztime.cpp -o franztime.exe
```

## Requirements

- Windows 7 or later.
- A C++ compiler compatible with the source code, such as MinGW-w64 GCC.
- Private working set measurements require a newer version of Windows 10 or Windows 11.

## License

See the repository's license information.

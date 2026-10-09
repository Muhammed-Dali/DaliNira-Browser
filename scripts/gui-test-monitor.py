#!/usr/bin/env python3
"""Lightweight resource monitor for scripts/safe-gui-test.sh.

Samples once per second from /proc (no profiler, no extra load) for every
process in the test's session:

  * CPU (percent of one core), per role: test (UI), WebKitWebProcess,
    WebKitNetworkProcess, other WebKit helpers, QtWebEngine, other;
  * PSS and private memory (smaps_rollup);
  * GPU engine time from DRM fdinfo (i915/amdgpu/xe/...), and whether any
    test process has an NVIDIA device (/dev/nvidia*) open;
  * number of WebKitWebProcess instances and of top-level windows owned by
    the test (_NET_WM_PID, every 5 s);
  * system: MemAvailable, swap in use, PSI (memory/cpu/io avg10), and any
    running systemd-coredump.

It writes one CSV row per sample and prints a summary at the end. When a
limit is crossed it writes the reason to --stop-file and exits 3; the runner
then terminates the test. Limits (override with the environment):

  DALINIRA_GUI_MAX_WEB_PROCESSES   default 2 (one view; a navigation may
                                   briefly overlap an old and a new process)
  DALINIRA_GUI_MIN_AVAILABLE_MB    default 1500
  DALINIRA_GUI_MAX_PSS_MB          default 3000 (whole test tree)
  DALINIRA_GUI_MAX_MEM_PSI         default 20 (memory "some" avg10, percent)
"""

import argparse
import csv
import os
import re
import signal
import subprocess
import sys
import time

CLK_TCK = os.sysconf("SC_CLK_TCK")
ROLES = ("test", "web", "network", "webkit_other", "qtwebengine", "other")


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def session_processes(sid):
    procs = {}
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        text = read(f"/proc/{entry}/stat")
        close = text.rfind(")")
        if close < 0:
            continue
        fields = text[close + 2:].split()
        if len(fields) < 13 or fields[0] == "Z" or int(fields[3]) != sid:
            continue
        procs[int(entry)] = {
            "comm": text[text.find("(") + 1:close],
            "cpu_ticks": int(fields[11]) + int(fields[12]),
        }
    return procs


def role(comm, pid, root):
    if comm.startswith("WebKitWebProces"):
        return "web"
    if comm.startswith("WebKitNetworkPr"):
        return "network"
    if comm.startswith("WebKit"):
        return "webkit_other"
    if comm.startswith("QtWebEngine") or comm.startswith("dalinira-webeng"):
        return "qtwebengine"
    if comm.startswith("dalinira-") or pid == root:
        return "test"
    return "other"


def memory(pid):
    pss = private = 0
    for line in read(f"/proc/{pid}/smaps_rollup").splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        if parts[0] == "Pss:":
            pss = int(parts[1])
        elif parts[0] in ("Private_Clean:", "Private_Dirty:"):
            private += int(parts[1])
    return pss, private  # kB


def gpu(pid):
    """(engine ns per driver, NVIDIA device paths open)."""
    engines = {}
    nvidia = set()
    fd_dir = f"/proc/{pid}/fd"
    try:
        fds = os.listdir(fd_dir)
    except OSError:
        return engines, nvidia
    for fd in fds:
        try:
            target = os.readlink(f"{fd_dir}/{fd}")
        except OSError:
            continue
        if target.startswith("/dev/nvidia"):
            nvidia.add(target)
        if not target.startswith("/dev/dri/"):
            continue
        info = read(f"/proc/{pid}/fdinfo/{fd}")
        driver = re.search(r"^drm-driver:\s*(\S+)", info, re.M)
        client = re.search(r"^drm-client-id:\s*(\S+)", info, re.M)
        if not driver:
            continue
        key = (driver.group(1), client.group(1) if client else fd)
        total = sum(int(ns) for ns in re.findall(r"^drm-engine-[\w-]+:\s*(\d+) ns", info, re.M))
        engines[key] = max(engines.get(key, 0), total)  # dup'd fds share a client
    per_driver = {}
    for (driver, _), ns in engines.items():
        per_driver[driver] = per_driver.get(driver, 0) + ns
    return per_driver, nvidia


def nvidia_libraries(pid):
    libs = set()
    for line in read(f"/proc/{pid}/maps").splitlines():
        path = line.split()[-1] if line.split() else ""
        name = os.path.basename(path)
        if re.search(r"nvidia|cuda|nvcuvid", name):
            libs.add(name)
    return sorted(libs)


def system():
    mem = {}
    for line in read("/proc/meminfo").splitlines():
        name, _, value = line.partition(":")
        mem[name] = int(value.split()[0]) if value.split() else 0
    psi = {}
    for kind in ("memory", "cpu", "io"):
        match = re.search(r"some avg10=([\d.]+)", read(f"/proc/pressure/{kind}"))
        psi[kind] = float(match.group(1)) if match else 0.0
    coredump = 0
    for entry in os.listdir("/proc"):
        if entry.isdigit() and read(f"/proc/{entry}/comm").startswith("systemd-coredum"):
            coredump += 1
    return {
        "available_mb": mem.get("MemAvailable", 0) // 1024,
        "swap_used_mb": (mem.get("SwapTotal", 0) - mem.get("SwapFree", 0)) // 1024,
        "psi_mem": psi["memory"], "psi_cpu": psi["cpu"], "psi_io": psi["io"],
        "coredump": coredump,
    }


def windows(pids):
    try:
        out = subprocess.run(["xprop", "-root", "_NET_CLIENT_LIST"], capture_output=True, text=True, timeout=2).stdout
        count = 0
        for wid in re.findall(r"0x[0-9a-f]+", out):
            prop = subprocess.run(["xprop", "-id", wid, "_NET_WM_PID"], capture_output=True, text=True, timeout=2).stdout
            match = re.search(r"= (\d+)", prop)
            if match and int(match.group(1)) in pids:
                count += 1
        return count
    except (OSError, subprocess.SubprocessError):
        return -1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--session", type=int, required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--stop-file", required=True)
    args = parser.parse_args()

    limits = {
        "web": int(os.environ.get("DALINIRA_GUI_MAX_WEB_PROCESSES", "2")),
        "available": int(os.environ.get("DALINIRA_GUI_MIN_AVAILABLE_MB", "1500")),
        "pss": int(os.environ.get("DALINIRA_GUI_MAX_PSS_MB", "3000")),
        "psi": float(os.environ.get("DALINIRA_GUI_MAX_MEM_PSI", "20")),
    }
    running = [True]
    signal.signal(signal.SIGTERM, lambda *_: running.__setitem__(0, False))

    columns = ["t"] + [f"cpu_{r}" for r in ROLES] + ["cpu_total", "pss_mb", "private_mb", "pss_web_mb",
               "web_processes", "webkit_processes", "windows", "gpu_busy", "gpu_drivers", "nvidia_open",
               "available_mb", "swap_used_mb", "psi_mem", "psi_cpu", "psi_io", "coredump"]
    peaks = {"cpu_total": 0.0, "pss_mb": 0, "private_mb": 0, "web_processes": 0, "webkit_processes": 0,
             "windows": 0, "gpu_busy": 0.0, "psi_mem": 0.0, "psi_cpu": 0.0, "psi_io": 0.0, "coredump": 0}
    low_available = None
    nvidia_seen = False
    nvidia_openers = {}  # pid -> description, first time seen
    drivers_seen = set()
    stop_reason = ""
    prev_cpu, prev_gpu, prev_time = {}, {}, time.monotonic()
    start = prev_time
    sample = 0
    last_windows = 0

    with open(args.out, "w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(columns)
        while running[0]:
            time.sleep(1.0)
            now = time.monotonic()
            dt = max(now - prev_time, 1e-3)
            procs = session_processes(args.session)
            cpu = dict.fromkeys(ROLES, 0.0)
            pss = private = pss_web = 0
            gpu_ns = {}
            nvidia = False
            web = webkit = 0
            for pid, p in procs.items():
                r = role(p["comm"], pid, args.session)
                ticks = p["cpu_ticks"]
                # A process first seen after the first sample started within
                # this interval (views are short-lived): count all its CPU.
                baseline = prev_cpu.get(pid, ticks if sample == 0 else 0)
                cpu[r] += 100.0 * (ticks - baseline) / CLK_TCK / dt
                prev_cpu[pid] = ticks
                ps, pr = memory(pid)
                pss += ps
                private += pr
                if r == "web":
                    web += 1
                    pss_web += ps
                if p["comm"].startswith("WebKit"):
                    webkit += 1
                drivers, nv = gpu(pid)
                nvidia |= bool(nv)
                if nv and pid not in nvidia_openers:
                    nvidia_openers[pid] = (f"t={round(now - start, 1)}s pid {pid} ({p['comm']}): "
                                           f"{' '.join(sorted(nv))}; libraries: {' '.join(nvidia_libraries(pid)) or 'none'}")
                for driver, ns in drivers.items():
                    key = (pid, driver)
                    gpu_ns[driver] = gpu_ns.get(driver, 0) + max(0, ns - prev_gpu.get(key, ns if sample == 0 else 0))
                    prev_gpu[key] = ns
            if sample % 5 == 0:
                last_windows = windows(set(procs))
            gpu_busy = max((100.0 * ns / (dt * 1e9) for ns in gpu_ns.values()), default=0.0)
            sysinfo = system()
            row = {
                "t": round(now - start, 1), **{f"cpu_{r}": round(v, 1) for r, v in cpu.items()},
                "cpu_total": round(sum(cpu.values()), 1), "pss_mb": pss // 1024, "private_mb": private // 1024,
                "pss_web_mb": pss_web // 1024, "web_processes": web, "webkit_processes": webkit,
                "windows": last_windows, "gpu_busy": round(gpu_busy, 1), "gpu_drivers": "+".join(sorted(gpu_ns)),
                "nvidia_open": int(nvidia), **sysinfo,
            }
            writer.writerow([row[c] for c in columns])
            out.flush()
            for key in peaks:
                peaks[key] = max(peaks[key], row[key])
            low_available = row["available_mb"] if low_available is None else min(low_available, row["available_mb"])
            nvidia_seen |= nvidia
            drivers_seen.update(gpu_ns)
            prev_time = now
            sample += 1

            if web > limits["web"]:
                stop_reason = f"{web} WebKit web processes at once (limit {limits['web']})"
            elif row["available_mb"] < limits["available"]:
                stop_reason = f"system memory available fell to {row['available_mb']} MB (limit {limits['available']})"
            elif row["pss_mb"] > limits["pss"]:
                stop_reason = f"test processes use {row['pss_mb']} MB PSS (limit {limits['pss']})"
            elif row["psi_mem"] > limits["psi"]:
                stop_reason = f"memory pressure {row['psi_mem']}% (limit {limits['psi']}%)"
            if stop_reason:
                with open(args.stop_file, "w") as f:
                    f.write(stop_reason + "\n")
                break

    print(f"monitor: {sample} samples over {round(time.monotonic() - start)}s -> {args.out}")
    print(f"monitor: peak CPU {peaks['cpu_total']}% of one core; peak PSS {peaks['pss_mb']} MB "
          f"(private {peaks['private_mb']} MB); lowest MemAvailable {low_available} MB")
    print(f"monitor: peak web processes {peaks['web_processes']}, WebKit processes {peaks['webkit_processes']}, "
          f"test windows {peaks['windows']}")
    print(f"monitor: peak GPU engine busy {peaks['gpu_busy']}% (drivers: {'+'.join(sorted(drivers_seen)) or 'none'}); "
          f"NVIDIA device opened: {'yes' if nvidia_seen else 'no'}")
    for description in nvidia_openers.values():
        print(f"monitor: NVIDIA device opened at {description}")
    print(f"monitor: peak PSI some avg10 memory {peaks['psi_mem']}% cpu {peaks['psi_cpu']}% io {peaks['psi_io']}%; "
          f"systemd-coredump seen: {'yes' if peaks['coredump'] else 'no'}")
    if stop_reason:
        print(f"monitor: STOP: {stop_reason}")
        sys.exit(3)


if __name__ == "__main__":
    main()

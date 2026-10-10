#!/usr/bin/env python3
"""Proves from /proc which WebKitGTK every DaliNira process really runs.

Samples /proc/<pid>/maps and /proc/<pid>/exe of DaliNira processes (the
browser and its test programs) and of WebKitGTK's helper processes
(WebKitWebProcess, WebKitNetworkProcess, WebKitGPUProcess) until the
duration ends or --until-pid exits. Each mapping of libwebkit2gtk,
libjavascriptcoregtk and the injected bundle is recorded by path and by
inode, so a copy reached through another path is still recognised. (Inode
only: on btrfs, stat() reports a subvolume's anonymous device while maps
reports the filesystem's, so device numbers do not compare.)

A process fails the check when it maps one of these libraries from outside
the private runtime prefix, maps the system library's inode, or (for a
helper) runs an executable outside the prefix.

Usage:
  verify-runtime-maps.py [--prefix /usr/lib/dalinira-browser/webkit]
                         [--seconds N | --until-pid PID] [--require-helpers]
                         [--json FILE]
Exit status: 0 all private, 1 a violation, 2 nothing WebKit was seen
(with --require-helpers).
"""
import argparse
import json
import os
import re
import sys
import time

LIBS = re.compile(r'/(libwebkit2gtk-4\.1\.so[^/]*|libjavascriptcoregtk-4\.1\.so[^/]*|libwebkit2gtkinjectedbundle\.so)$')
HELPERS = ('WebKitWebProcess', 'WebKitNetworkProcess', 'WebKitGPUProcess')
WATCH = re.compile(r'^(WebKitWebProces|WebKitNetworkPr|WebKitGPUProces|dalinira-)')
SYSTEM_LIBS = ('/usr/lib/libwebkit2gtk-4.1.so.0', '/usr/lib/libjavascriptcoregtk-4.1.so.0',
               '/usr/lib/x86_64-linux-gnu/libwebkit2gtk-4.1.so.0',
               '/usr/lib/x86_64-linux-gnu/libjavascriptcoregtk-4.1.so.0',
               '/usr/lib64/libwebkit2gtk-4.1.so.0', '/usr/lib64/libjavascriptcoregtk-4.1.so.0')


def file_id(path):
    return os.stat(path).st_ino


def maps_id(dev, inode):
    return int(inode)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--prefix', default='/usr/lib/dalinira-browser/webkit')
    ap.add_argument('--seconds', type=float, default=0)
    ap.add_argument('--until-pid', type=int, default=0)
    ap.add_argument('--require-helpers', action='store_true')
    ap.add_argument('--json')
    ap.add_argument('--interval', type=float, default=0.1)
    args = ap.parse_args()
    prefix = os.path.realpath(args.prefix)

    system_ids = {}
    for path in SYSTEM_LIBS:
        try:
            system_ids[file_id(path)] = os.path.realpath(path)
        except OSError:
            pass
    private_ids = {}
    for root, _, files in os.walk(prefix):
        for name in files:
            path = os.path.join(root, name)
            if LIBS.search(path):
                private_ids[file_id(path)] = path

    procs = {}  # pid -> record
    end = time.time() + args.seconds if args.seconds else None
    while True:
        for entry in os.listdir('/proc'):
            if not entry.isdigit():
                continue
            pid = int(entry)
            try:
                with open(f'/proc/{pid}/comm') as f:
                    comm = f.read().strip()
                if not WATCH.match(comm):
                    continue
                exe = os.readlink(f'/proc/{pid}/exe')
                with open(f'/proc/{pid}/maps') as f:
                    lines = f.readlines()
            except OSError:
                continue
            rec = procs.setdefault(pid, {'pid': pid, 'comm': comm, 'exe': exe, 'mappings': {}})
            for line in lines:
                parts = line.split(None, 5)
                if len(parts) < 6:
                    continue
                path = parts[5].strip()
                if not LIBS.search(path.replace(' (deleted)', '')):
                    continue
                rec['mappings'][path] = maps_id(parts[3], parts[4])
        if args.until_pid and not os.path.exists(f'/proc/{args.until_pid}'):
            break
        if end and time.time() >= end:
            break
        if not end and not args.until_pid:
            break
        time.sleep(args.interval)

    violations = []
    helpers_seen = 0
    for rec in procs.values():
        is_helper = os.path.basename(rec['exe'].replace(' (deleted)', '')) in HELPERS
        if is_helper:
            helpers_seen += 1
            if not rec['exe'].startswith(prefix + '/'):
                violations.append(f"{rec['comm']}[{rec['pid']}] runs {rec['exe']}")
        for path, fid in rec['mappings'].items():
            if fid in system_ids:
                violations.append(f"{rec['comm']}[{rec['pid']}] maps the system {system_ids[fid]} ({path})")
            elif not path.startswith(prefix + '/') or fid not in private_ids:
                violations.append(f"{rec['comm']}[{rec['pid']}] maps {path} (not the private runtime)")

    by_kind = {}
    for rec in procs.values():
        kind = os.path.basename(rec['exe'].replace(' (deleted)', ''))
        k = by_kind.setdefault(kind, {'processes': 0, 'with_webkit': 0, 'paths': set(), 'exes': set()})
        k['processes'] += 1
        if rec['mappings']:
            k['with_webkit'] += 1
        k['paths'].update(rec['mappings'])
        k['exes'].add(rec['exe'])
    for kind, k in sorted(by_kind.items()):
        print(f"== {kind}: {k['processes']} process(es), {k['with_webkit']} mapping WebKitGTK")
        for exe in sorted(k['exes']):
            print(f"   exe  {exe}")
        for path in sorted(k['paths']):
            print(f"   maps {path}")
    print(f"system WebKitGTK inodes known: {len(system_ids)}; private library files: {len(private_ids)}")
    if args.json:
        with open(args.json, 'w') as f:
            json.dump({'prefix': prefix, 'helpers_seen': helpers_seen, 'violations': violations,
                       'processes': [{**r, 'mappings': dict(r['mappings'])}
                                     for r in procs.values()]}, f, indent=1)
    if violations:
        print('VIOLATIONS:')
        for v in violations:
            print('  ' + v)
        return 1
    if args.require_helpers and helpers_seen == 0:
        print('no WebKit helper process was seen')
        return 2
    print(f'OK: {len(procs)} process(es), {helpers_seen} WebKit helper(s), all on the private runtime')
    return 0


if __name__ == '__main__':
    sys.exit(main())

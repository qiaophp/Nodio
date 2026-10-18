import os
import sys
import subprocess
import concurrent.futures
from pathlib import Path
from collections import Counter

LIBDIR = r"D:\aardio\lib"

def file_to_module(rel_path):
    parts = list(Path(rel_path).parts)
    if parts[-1].endswith(".aardio"):
        parts[-1] = parts[-1][:-7]
    if parts[-1] == "_":
        parts.pop()
    if not parts:
        return None
    return ".".join(parts)

def test_file(nodio_exe, rel_path, timeout=2.5):
    mod = file_to_module(rel_path)
    if not mod:
        return rel_path, "SKIP", 0, "empty module"
    cmd = [nodio_exe, "-e", f"import {mod};"]
    env = os.environ.copy()
    env["NODIO_LIBDIR"] = LIBDIR
    try:
        res = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout, encoding="utf-8", errors="replace")
        if res.returncode == 0:
            return rel_path, "PASS", 0, ""
        else:
            err = res.stderr.strip() or res.stdout.strip()
            return rel_path, "FAIL", res.returncode, err
    except subprocess.TimeoutExpired:
        return rel_path, "HANG", -1, "timeout > 2.5s"
    except Exception as e:
        return rel_path, "ERROR", -2, str(e)

def run_sweep(nodio_exe, arch_name):
    files = []
    for root, _, filenames in os.walk(LIBDIR):
        for f in filenames:
            if f.endswith(".aardio"):
                full = os.path.join(root, f)
                rel = os.path.relpath(full, LIBDIR)
                files.append(rel)
    files.sort()

    print(f"=== Sweeping {len(files)} stdlib files on {arch_name} ({nodio_exe}) ===")
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        futures = {ex.submit(test_file, nodio_exe, f): f for f in files}
        for fut in concurrent.futures.as_completed(futures):
            rel_path, status, code, msg = fut.result()
            results[rel_path] = (status, code, msg)

    counts = Counter(status for status, _, _ in results.values())
    print(f"[{arch_name}] Summary: Total={len(files)}, PASS={counts['PASS']}, FAIL={counts['FAIL']}, HANG={counts['HANG']}")
    return results

if __name__ == "__main__":
    arch = sys.argv[1] if len(sys.argv) > 1 else "x64"
    exe = r"D:\Nodio\build-x64\Release\nodio.exe" if arch == "x64" else r"D:\Nodio\build-x86\Release\nodio.exe"
    run_sweep(exe, arch)

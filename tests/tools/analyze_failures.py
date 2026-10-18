import os
import sys
import subprocess
import concurrent.futures
from pathlib import Path
from collections import defaultdict, Counter

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
            # take first 2 lines
            lines = [l.strip() for l in err.splitlines() if l.strip()]
            summary_err = " | ".join(lines[:2])
            return rel_path, "FAIL", res.returncode, summary_err
    except subprocess.TimeoutExpired:
        return rel_path, "HANG", -1, "timeout > 2.5s"
    except Exception as e:
        return rel_path, "ERROR", -2, str(e)

def analyze(arch):
    exe = r"D:\Nodio\build-x64\Release\nodio.exe" if arch == "x64" else r"D:\Nodio\build-x86\Release\nodio.exe"
    files = []
    for root, _, filenames in os.walk(LIBDIR):
        for f in filenames:
            if f.endswith(".aardio"):
                full = os.path.join(root, f)
                rel = os.path.relpath(full, LIBDIR)
                files.append(rel)
    files.sort()

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        futures = {ex.submit(test_file, exe, f): f for f in files}
        for fut in concurrent.futures.as_completed(futures):
            rel, status, code, msg = fut.result()
            results[rel] = (status, code, msg)

    categories = defaultdict(list)
    for rel, (status, code, msg) in results.items():
        if status == "PASS":
            categories["PASS"].append(rel)
        elif status == "HANG":
            categories["HANG"].append((rel, msg))
        else:
            # classify failure
            if "无法加载 DLL" in msg or "找不到指定的模块" in msg or "LoadLibrary" in msg or "zlibwapi.dll" in msg:
                categories["FAIL_DLL_LOAD"].append((rel, msg))
            elif "禁止直接导入" in msg or "不能直接导入" in msg or "请导入" in msg or "请改用" in msg:
                categories["FAIL_DISALLOWED_DIRECT_IMPORT"].append((rel, msg))
            elif "编译失败" in msg or "语法错误" in msg:
                categories["FAIL_SYNTAX"].append((rel, msg))
            elif "未能找到程序集" in msg or "程序集" in msg or "dotNet" in msg or "CLR" in msg:
                categories["FAIL_DOTNET"].append((rel, msg))
            elif "Python" in msg or "py3" in msg or "py2" in msg:
                categories["FAIL_PYTHON"].append((rel, msg))
            else:
                categories["FAIL_RUNTIME_OTHER"].append((rel, msg))

    print(f"\n==================== {arch.upper()} DETAILED BREAKDOWN ====================")
    print(f"Total files: {len(files)}")
    print(f"PASS: {len(categories['PASS'])} ({len(categories['PASS'])/len(files)*100:.1f}%)")
    print(f"HANG: {len(categories['HANG'])}")
    for rel, msg in categories['HANG']:
        print(f"  HANG: {rel}")
    for cat in ["FAIL_DLL_LOAD", "FAIL_DISALLOWED_DIRECT_IMPORT", "FAIL_SYNTAX", "FAIL_DOTNET", "FAIL_PYTHON", "FAIL_RUNTIME_OTHER"]:
        items = categories[cat]
        print(f"\n--- {cat} ({len(items)}) ---")
        for rel, msg in items[:15]:
            print(f"  {rel}: {msg[:100]}")
        if len(items) > 15:
            print(f"  ... and {len(items)-15} more")

if __name__ == "__main__":
    analyze("x86")
    analyze("x64")

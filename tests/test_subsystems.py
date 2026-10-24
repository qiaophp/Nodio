import subprocess
import os

modules = [
    # Core & Win
    "win", "win.ui", "win.reg", "win.clip", "win.cur", "win.image", "winex", "winex.key", "mouse", "key",
    # GDI & GDI+
    "gdi", "gdip", "gdip.graphics", "gdip.bitmap", "gdip.font", "gdip.family", "fonts",
    # COM & System
    "com", "com.picture", "com.interface", "dotNet", "sys.cpu", "sys.mem", "sys.volume",
    # Filesystem
    "fsys", "fsys.file", "fsys.dir", "fsys.ini", "fsys.config", "fsys.log", "fsys.tar", "fsys.zip", "fsys.openXml.xlsx",
    # Network & Internet
    "inet", "inet.http", "inet.url", "inet.downBox", "inet.whttp", "wsock", "wsock.tcp.client", "wsock.tcp.server",
    # Cryptography
    "crypt", "crypt.bin", "crypt.aes", "crypt.rsa", "crypt.jwt",
    # Process & Thread
    "process", "process.popen", "process.job", "process.admin", "process.command", "thread", "thread.command", "thread.manage", "thread.event", "thread.table",
    # Data & Formats
    "JSON", "web.json", "string.xml", "string.regex", "string.html", "math", "table", "time", "raw",
    # Native C-Extension Libraries
    "sqlite", "zlib", "qrencode", "py3", "golang", "nodeJs", "sevenZip.lzma", "string.markdown", "web.view", "web.layout", "py2"
]

env = os.environ.copy()
env["NODIO_PAUSE"] = "0"
env["NODIO_LIBDIR"] = "D:\\aardio\\lib"

for arch, exe in [("x86 (32-bit)", "build-x86/Release/nodio.exe"), ("x64 (64-bit)", "build-x64/Release/nodio.exe")]:
    # 锚定到仓库根（脚本所在目录的上一级），跑法不限 CWD
    exe_p = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), exe.replace("/", os.sep))
    passed = []
    failed = []
    for mod in modules:
        try:
            p = subprocess.run([exe_p, "-e", f'import {mod}; print("OK");'], capture_output=True, text=True, timeout=5, env=env)
            if p.returncode == 0 and "OK" in p.stdout:
                passed.append(mod)
            else:
                err = (p.stderr.strip() or p.stdout.strip()).replace("\r", " ").replace("\n", " ")
                failed.append((mod, err[:60]))
        except Exception as e:
            failed.append((mod, str(e)))
    print(f"\n=== {arch} Core Subsystems ({len(modules)} tested) ===")
    print(f"Passed: {len(passed)}/{len(modules)}")
    if failed:
        print("Issues:")
        for m, err in failed:
            print(f"  - {m}: {err}")

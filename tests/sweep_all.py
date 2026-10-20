import sys
import os
import subprocess
import time

nodio_exe = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("build-x86/Release/nodio.exe")
modules_file = os.path.abspath("tests/public_lib_modules.txt")
log_file = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.abspath("tests/sweep_x86.log")

with open(modules_file, "r", encoding="utf-8") as f:
    modules = [line.strip() for line in f if line.strip() and not line.startswith("#")]

print(f"Testing {len(modules)} modules using: {nodio_exe}", flush=True)

env = os.environ.copy()
env["NODIO_PAUSE"] = "0"
env["NODIO_LIBDIR"] = "D:\\aardio\\lib"

with open(log_file, "w", encoding="utf-8") as out_f:
    for idx, mod in enumerate(modules):
        code = f'import {mod}; print("MOD_OK");'
        t_start = time.time()
        try:
            p = subprocess.Popen(
                [nodio_exe, "-e", code],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env,
                creationflags=subprocess.CREATE_NO_WINDOW
            )
            try:
                stdout, stderr = p.communicate(timeout=2.0)
                if p.returncode == 0 and "MOD_OK" in stdout:
                    status = "OK"
                    detail = ""
                else:
                    status = f"FAIL({p.returncode})"
                    detail = (stderr.strip() or stdout.strip()).replace("\r", " ").replace("\n", " ")
                    if len(detail) > 100:
                        detail = detail[:100] + "..."
            except subprocess.TimeoutExpired:
                p.kill()
                p.communicate()
                status = "TIMEOUT"
                detail = "exceeded 2.0s"
        except Exception as ex:
            status = "EX"
            detail = str(ex)

        msg = f"[{idx+1}/{len(modules)}] {mod}: {status} {detail}".strip()
        print(msg, flush=True)
        out_f.write(msg + "\n")
        out_f.flush()

print("SWEEP COMPLETE!", flush=True)

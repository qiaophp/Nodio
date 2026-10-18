import os, subprocess, json

arch = "x86"
exe = rf"D:\Nodio\build-{arch}\Release\nodio.exe"
lib_dir = r"D:\aardio\lib"

files = []
for root, dirs, fnames in os.walk(lib_dir):
    for f in fnames:
        if f.endswith(".aardio"):
            files.append(os.path.relpath(os.path.join(root, f), lib_dir))

for rel in sorted(files):
    full_path = os.path.join(lib_dir, rel)
    cmd = [exe, full_path]
    env = os.environ.copy()
    env["NODIO_LIBDIR"] = lib_dir
    try:
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=3)
        if p.returncode != 0:
            err = (p.stderr or p.stdout).strip()
            # Filter out expected guards
            if any(k in err for k in ["禁止直接导入", "请导入", "请直接导入", "请先导入", "必须先导入", "该类仅供", "只能用于开发环境", "字节码", "请不要在程序中导入多个"]):
                continue
            if "py2" in rel or "py3" in rel:
                continue
            if "BouncyCastle" in err or "sevenZip" in err:
                continue
            if "意外的 with" in err or "意外的 ." in err:
                continue
            print(f"[{rel}] (code {p.returncode}):")
            print(f"  {err[:200]}")
    except subprocess.TimeoutExpired:
        pass

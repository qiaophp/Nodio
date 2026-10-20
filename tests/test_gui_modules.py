import sys
import subprocess
import os

nodio_exe = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("build-x86/Release/nodio.exe")
arch = "x64" if "x64" in nodio_exe else "x86"

gui_mods = [
    "win.ui",
    "win.ui.accelerator",
    "win.ui.atom",
    "win.ui.background",
    "win.ui.explorer",
    "win.ui.fade",
    "win.ui.grid",
    "win.ui.layered",
    "win.ui.layeredRound",
    "win.ui.listEdit",
    "win.ui.listPopup",
    "win.ui.loadingMask",
    "win.ui.localization",
    "win.ui.mask",
    "win.ui.menu",
    "win.ui.minmax",
    "win.ui.orphanPlus",
    "win.ui.resizeBorder",
    "win.ui.scrollbar",
    "win.ui.shadow",
    "win.ui.simpleWindow",
    "win.ui.simpleWindow2",
    "win.ui.simpleWindow3",
    "win.ui.statusbar",
    "win.ui.tabs",
    "win.ui.toolbar",
    "win.ui.tooltip",
    "win.ui.tracker",
    "win.ui.ctrl",
    "win.ui.ctrl.atlax",
    "win.ui.ctrl.bk",
    "win.ui.ctrl.bkplus",
    "win.ui.ctrl.button",
    "win.ui.ctrl.calendar",
    "win.ui.ctrl.canvas",
    "win.ui.ctrl.checklist",
    "win.ui.ctrl.close",
    "win.ui.ctrl.combobox",
    "win.ui.ctrl.custom",
    "win.ui.ctrl.datetimepick",
    "win.ui.ctrl.edit",
    "win.ui.ctrl.hotkey",
    "win.ui.ctrl.ipaddress",
    "win.ui.ctrl.listbox",
    "win.ui.ctrl.listview",
    "win.ui.ctrl.picturebox",
    "win.ui.ctrl.plus",
    "win.ui.ctrl.progress",
    "win.ui.ctrl.richedit",
    "win.ui.ctrl.scrollbar",
    "win.ui.ctrl.spin",
    "win.ui.ctrl.splitter",
    "win.ui.ctrl.static",
    "win.ui.ctrl.syslink",
    "win.ui.ctrl.tab",
    "win.ui.ctrl.textbox",
    "win.ui.ctrl.thread",
    "win.ui.ctrl.trackbar",
    "win.ui.ctrl.treeview",
    "win.ui.ctrl.vlistview",
    "win.ui.ctrl.metaProperty",
    "win.ui.ctrl.pick"
]

env = os.environ.copy()
env["NODIO_PAUSE"] = "0"
env["NODIO_LIBDIR"] = "D:\\aardio\\lib"

passed = []
failed = []

for mod in gui_mods:
    code = f'import {mod}; print("GUI_MOD_OK");'
    try:
        p = subprocess.run(
            [nodio_exe, "-e", code],
            capture_output=True,
            text=True,
            timeout=5,
            env=env
        )
        if p.returncode == 0 and "GUI_MOD_OK" in p.stdout:
            passed.append(mod)
        else:
            err = (p.stderr.strip() or p.stdout.strip()).replace("\r", " ").replace("\n", " ")
            failed.append((mod, p.returncode, err))
    except subprocess.TimeoutExpired:
        failed.append((mod, -1, "TIMEOUT"))

print(f"[{arch}] Total GUI modules: {len(gui_mods)}")
print(f"[{arch}] Passed: {len(passed)}")
print(f"[{arch}] Failed: {len(failed)}")

if failed:
    print(f"\nFAILED GUI MODULES on {arch}:")
    for mod, rc, err in failed:
        print(f"  {mod} -> [{rc}] {err}")

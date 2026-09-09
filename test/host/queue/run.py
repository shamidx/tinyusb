"""Exercise actual USBH queue state transitions with a stub HCD."""
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BUILD = ROOT / "build/queue-test"
BUILD.mkdir(parents=True, exist_ok=True)
for depth in (1, 2):
    exe = BUILD / (f"test_queue_{depth}" + (".exe" if os.name == "nt" else ""))
    # Leave depth undefined in the first build to exercise the stack default.
    defines = [] if depth == 1 else ["-DCFG_TUH_XFER_QUEUE_DEPTH=2"]
    subprocess.run([os.environ.get("CC", "gcc"), "-std=c11", "-O2", "-g",
        "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections", *defines,
        "-I" + str(HERE), "-I" + str(ROOT / "src"), str(HERE / "test_queue.c"),
        str(ROOT / "src/tusb.c"), str(ROOT / "src/common/tusb_fifo.c"),
        "-Wl,--gc-sections", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)

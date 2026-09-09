"""Compile and run actual EHCI driver logic with static simulated DMA memory."""
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BUILD = ROOT / "build/ehci-test"
BUILD.mkdir(parents=True, exist_ok=True)
exe = BUILD / ("test_iso.exe" if os.name == "nt" else "test_iso")
flags = (["-Wl,--image-base,0x400000,--disable-dynamicbase"] if os.name == "nt"
         else ["-fno-pie", "-no-pie"])
for count, depth in ((0, 1), (4, 1), (4, 2)):
    subprocess.run([os.environ.get("CC", "gcc"),
        "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
        f"-DCFG_TUH_EHCI_ISO_EP_MAX={count}", f"-DCFG_TUH_XFER_QUEUE_DEPTH={depth}",
        "-I" + str(HERE), "-I" + str(ROOT / "src"), str(HERE / "test_iso.c"),
        "-o", str(exe), *flags,
    ], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)

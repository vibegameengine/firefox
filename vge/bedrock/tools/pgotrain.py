import argparse
import ctypes
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from bench import HOST, PROBE, slot
from windowwatch import main_window

WM_CLOSE = 0x0010
user32 = ctypes.WinDLL("user32", use_last_error=True)


def run(url, seconds, profile_dir, index):
    env = dict(os.environ)
    env["LLVM_PROFILE_FILE"] = str(Path(profile_dir) / f"vge-{index}-%p.profraw")
    env["VGE_FRAME_PROBE"] = str(PROBE).replace("/", "\\")
    process = subprocess.Popen([str(HOST), "--window", url], cwd=HOST.parent, env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(seconds)
    hwnd = main_window(process.pid)
    if hwnd:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    try:
        code = process.wait(timeout=60)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        code = None
    return code


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("profile_dir")
    parser.add_argument("--seconds", type=int, default=25)
    parser.add_argument("urls", nargs="+")
    args = parser.parse_args()
    Path(args.profile_dir).mkdir(parents=True, exist_ok=True)
    slot("acquire", "pgo-train")
    try:
        for index, url in enumerate(args.urls):
            code = run(url, args.seconds, args.profile_dir, index)
            print(f"trained {url} exit {code}")
    finally:
        slot("release", "pgo-train")


if __name__ == "__main__":
    main()

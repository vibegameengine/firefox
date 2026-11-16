import ctypes
import hashlib
import threading
import time
from ctypes import wintypes as W

from PIL import ImageGrab

user32 = ctypes.WinDLL("user32", use_last_error=True)
ENUM = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
user32.WindowFromPoint.argtypes = [W.POINT]
user32.WindowFromPoint.restype = W.HWND
user32.GetAncestor.argtypes = [W.HWND, W.UINT]
user32.GetAncestor.restype = W.HWND
GA_ROOT = 2
HWND_TOPMOST = W.HWND(-1)
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010
SWP_NOZORDER = 0x0004
user32.SetWindowPos.argtypes = [W.HWND, W.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, W.UINT]


def main_window(pid):
    found = []

    def visit(hwnd, _):
        owner = W.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        rect = W.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        if owner.value == pid and user32.IsWindowVisible(hwnd) and rect.right - rect.left > 200:
            found.append(hwnd)
        return True

    user32.EnumWindows(ENUM(visit), 0)
    return found[0] if found else None


def client_rect(hwnd):
    rect = W.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    origin = W.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(origin))
    return origin.x, origin.y, origin.x + rect.right, origin.y + rect.bottom


class WindowWatch:
    def __init__(self, pid, period=2.0, shots=None, topmost=True):
        self.topmost = topmost
        self.shots = shots
        self.pid = pid
        self.period = period
        self.samples = []
        self.stop_flag = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.rate_thread = threading.Thread(target=self.rate_run, daemon=True)
        self.changes_per_second = []

    def start(self):
        self.thread.start()
        self.rate_thread.start()

    def stop(self):
        self.stop_flag.set()
        self.thread.join(timeout=10)
        self.rate_thread.join(timeout=10)

    def rate_run(self):
        previous = None
        second_start = time.time()
        grabs = 0
        changes = 0
        while not self.stop_flag.is_set():
            hwnd = main_window(self.pid)
            if not hwnd:
                time.sleep(0.2)
                continue
            box = client_rect(hwnd)
            cx, cy = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2
            try:
                data = ImageGrab.grab(bbox=(cx - 100, cy - 100, cx + 100, cy + 100), all_screens=True).tobytes()
            except OSError:
                continue
            grabs += 1
            if previous is not None and data != previous:
                changes += 1
            previous = data
            now = time.time()
            if now - second_start >= 1.0:
                self.changes_per_second.append({"changes": changes, "grabs": grabs})
                second_start = now
                grabs = 0
                changes = 0

    def run(self):
        start = time.time()
        previous = None
        while self.topmost and time.time() - start < 15 and not self.stop_flag.is_set():
            hwnd = main_window(self.pid)
            if hwnd:
                user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
                rect = W.RECT()
                user32.GetWindowRect(hwnd, ctypes.byref(rect))
                user32.SetWindowPos(hwnd, None, rect.left + 1, rect.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER)
                user32.SetWindowPos(hwnd, None, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER)
                break
            time.sleep(0.1)
        while not self.stop_flag.wait(self.period):
            hwnd = main_window(self.pid)
            if not hwnd:
                continue
            if self.topmost:
                user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
            box = client_rect(hwnd)
            center = W.POINT((box[0] + box[2]) // 2, (box[1] + box[3]) // 2)
            top = user32.WindowFromPoint(center)
            root = user32.GetAncestor(top, GA_ROOT) if top else None
            digest = None
            try:
                full = ImageGrab.grab(bbox=box, all_screens=True)
                if self.shots and len(self.samples) % 5 == 0:
                    full.convert("RGB").resize((640, 360)).save(f"{self.shots}-{len(self.samples):03d}.jpg")
                image = full.resize((160, 90))
                digest = hashlib.md5(image.tobytes()).hexdigest()
            except OSError:
                pass
            self.samples.append(
                {
                    "t": round(time.time() - start, 1),
                    "changed": digest is not None and digest != previous,
                    "foreground": user32.GetForegroundWindow() == hwnd,
                    "on_top_at_center": root == hwnd,
                    "iconic": bool(user32.IsIconic(hwnd)),
                }
            )
            previous = digest

    def summary(self):
        visible = [s for s in self.samples if s["on_top_at_center"] and not s["iconic"]]
        frozen = [s["t"] for s in visible if not s["changed"]]
        return {
            "samples": len(self.samples),
            "visible_samples": len(visible),
            "frozen_visible_samples": len(frozen),
            "first_frozen_at": frozen[0] if frozen else None,
            "frozen_times": frozen[:40],
            "foreground_samples": sum(s["foreground"] for s in self.samples),
            "visible_changes_per_s": [c["changes"] for c in self.changes_per_second],
            "grabs_per_s": [c["grabs"] for c in self.changes_per_second],
        }


if __name__ == "__main__":
    import json
    import sys

    watch = WindowWatch(int(sys.argv[1]))
    watch.start()
    time.sleep(float(sys.argv[2]))
    watch.stop()
    with open(sys.argv[3], "w") as output:
        json.dump(watch.summary(), output)

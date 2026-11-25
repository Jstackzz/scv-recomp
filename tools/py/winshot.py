"""Save a picture of a window (by title prefix): python tools/py/winshot.py <title prefix> <out.png> [--screen] [--pid <n>]

PrintWindow with PW_RENDERFULLCONTENT (DWM renders the window's DirectX content even when Windows shows the
swap chain directly); --screen grabs that part of the screen instead; --pid takes only that process's window (one game of
several). For looking at the game during bring-up."""
import ctypes
import ctypes.wintypes as wt
import sys

from PIL import Image, ImageGrab

user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
user32.SetProcessDPIAware()
found = []
pid = int(sys.argv[sys.argv.index("--pid") + 1]) if "--pid" in sys.argv else None


@ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
def enum(hwnd, _):
    n = user32.GetWindowTextLengthW(hwnd)
    if n and user32.IsWindowVisible(hwnd):
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if buf.value.startswith(sys.argv[1]) and (pid is None or owner.value == pid):
            found.append(hwnd)
    return True


user32.EnumWindows(enum, 0)
if not found:
    raise SystemExit(f"no visible window titled {sys.argv[1]!r}*")
hwnd = found[0]
rect = wt.RECT()
user32.GetClientRect(hwnd, ctypes.byref(rect))
w, h = rect.right, rect.bottom
if "--screen" in sys.argv:
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    img = ImageGrab.grab(bbox=(pt.x, pt.y, pt.x + w, pt.y + h), all_screens=True)
else:
    hdc = user32.GetDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    PW_CLIENTONLY, PW_RENDERFULLCONTENT = 1, 2
    ok = user32.PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bih = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bih), 0)
    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    if not ok:
        print("PrintWindow failed")
img.save(sys.argv[2])
print(f"saved {sys.argv[2]} {img.size} extrema {img.getextrema()}")

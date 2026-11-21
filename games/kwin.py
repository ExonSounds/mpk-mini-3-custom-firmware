"""KDE Plasma (KWin) helpers for the Joystick Mouse magnifier: where the pointer is, and a picture of a screen area.

Wayland lets no app ask for the pointer position, so a tiny KWin script reports it to us over D-Bus whenever it
moves (any mouse, not just ours); screen areas come from KWin's ScreenShot2 interface as raw pixels.
"""
import os
import tempfile

import numpy as np
from gi.repository import Gio, GLib

SCRIPT = "mpk3pointer"                    # KWin script name; a leftover one from a crash is replaced
IFACE = """<node><interface name="local.mpk3.Pointer">
  <method name="Pos"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
</interface></node>"""
BGRA = {4, 5, 6}                          # QImage RGB32 / ARGB32 (premultiplied): bytes B, G, R, A; others R, G, B, A


def _kwin(bus, path, iface, method, args=None, ret=None):
    return bus.call_sync("org.kde.KWin", path, iface, method, args, GLib.VariantType(ret) if ret else None,
                         Gio.DBusCallFlags.NONE, 3000, None)


class PointerTracker:
    """pos = the pointer's (x, y) in logical screen pixels, kept current while poll() is called."""

    def __init__(self):
        self.pos, self.reg, self.loaded = None, None, False
        self.dir = tempfile.mkdtemp(prefix="mpk3-kwin-")
        self.ctx = GLib.MainContext.new()     # our own, so poll() can run from the game's thread
        self.ctx.push_thread_default()
        try:
            self.bus = Gio.bus_get_sync(Gio.BusType.SESSION)
            info = Gio.DBusNodeInfo.new_for_xml(IFACE).interfaces[0]
            self.reg = self.bus.register_object("/mpk3/pointer", info, self._pos, None, None)
        finally:
            self.ctx.pop_thread_default()
        js = (f'function send() {{ callDBus("{self.bus.get_unique_name()}", "/mpk3/pointer", "local.mpk3.Pointer",'
              ' "Pos", Math.round(workspace.cursorPos.x), Math.round(workspace.cursorPos.y)); }\n'
              "workspace.cursorPosChanged.connect(send);\nsend();\n")
        path = os.path.join(self.dir, "pointer.js")
        with open(path, "w") as f:
            f.write(js)
        try:
            self._unload()                    # a leftover from an earlier run would keep the name taken
            sid = _kwin(self.bus, "/Scripting", "org.kde.kwin.Scripting", "loadScript",
                        GLib.Variant("(ss)", (path, SCRIPT)), "(i)")[0]
            _kwin(self.bus, f"/Scripting/Script{sid}", "org.kde.kwin.Script", "run")
            self.loaded = True
        except GLib.Error as e:
            self.close()
            raise OSError(f"KWin did not run the pointer script ({e.message}) - the magnifier needs KDE Plasma") \
                from None

    def _pos(self, conn, sender, path, iface, method, params, invocation):
        self.pos = params.unpack()
        invocation.return_value(None)

    def poll(self):
        while self.ctx.pending():
            self.ctx.iteration(False)
        return self.pos

    def _unload(self):
        _kwin(self.bus, "/Scripting", "org.kde.kwin.Scripting", "unloadScript", GLib.Variant("(s)", (SCRIPT,)), "(b)")

    def close(self):
        if self.loaded:
            try:
                self._unload()
            except GLib.Error:
                pass
            self.loaded = False
        if self.reg:
            self.bus.unregister_object(self.reg)
            self.reg = None
        if os.path.isdir(self.dir):
            for f in os.listdir(self.dir):
                os.remove(os.path.join(self.dir, f))
            os.rmdir(self.dir)


def to_gray(data, meta):
    """KWin's raw image -> 2-D array of brightness 0..1 (rows x columns, in device pixels)."""
    w, h, stride = int(meta["width"]), int(meta["height"]), int(meta["stride"])
    px = np.frombuffer(data, dtype=np.uint8)[:h * stride].reshape(h, stride)[:, :w * 4].reshape(h, w, 4)
    if meta.get("format") in BGRA:
        r, g, b = px[..., 2], px[..., 1], px[..., 0]
    else:
        r, g, b = px[..., 0], px[..., 1], px[..., 2]
    return (0.299 * r + 0.587 * g + 0.114 * b) / 255


def capture(x, y, w, h, cursor=True):
    """Brightness of the logical screen area (x, y, w, h), resampled to h x w; None if KWin refuses."""
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    r, wfd = os.pipe()
    fds = Gio.UnixFDList.new()
    idx = fds.append(wfd)
    os.close(wfd)                             # KWin has its own copy; ours would keep the pipe from ending
    try:
        res, _ = bus.call_with_unix_fd_list_sync(
            "org.kde.KWin", "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2", "CaptureArea",
            GLib.Variant("(iiuua{sv}h)", (int(x), int(y), int(w), int(h),
                                          {"include-cursor": GLib.Variant("b", cursor)}, idx)),
            GLib.VariantType("(a{sv})"), Gio.DBusCallFlags.NONE, 2000, fds, None)
        del fds
        data = b""
        while chunk := os.read(r, 1 << 16):
            data += chunk
    except GLib.Error:
        return None
    finally:
        os.close(r)
    gray = to_gray(data, res.unpack()[0])
    ys = np.arange(h) * gray.shape[0] // h    # HiDPI screens give scale x more pixels: back to the asked size
    xs = np.arange(w) * gray.shape[1] // w
    return gray[ys][:, xs]

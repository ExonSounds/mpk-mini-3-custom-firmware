#!/bin/sh
# Build build/MPK3-Emulator-x86_64.AppImage: the emulator core (with the Unicorn library it links), the front
# panel, the launcher and the custom firmware with our own boot stub (mpk3_emu.py --stub-image: no Akai code).
# Akai's stock firmware is not included; run it from your own copy: MPK3-Emulator-x86_64.AppImage /path/to/AT.bin
# Needs: make, cc, clang + lld (fw/), unicorn, alsa-lib, appimagetool. The AppImage itself needs python3 with Tk.
#
# Licences: our code is MIT (LICENSE). The AppImage also carries libunicorn (GPL-2.0-only), so the AppImage as a
# whole is distributed under GPL-2.0; its notice says where Unicorn's source is.
set -e
cd "$(dirname "$0")/.."
OUT=build
APP=$OUT/MPK3-Emulator.AppDir
SHARE=$APP/usr/share/mpk3-emu
UNICORN=$(ldd emu/mpk3emu 2>/dev/null | awk '/libunicorn/ {print $3}')

make -s -C emu
[ -n "$UNICORN" ] || UNICORN=$(ldd emu/mpk3emu | awk '/libunicorn/ {print $3}')
rm -rf "$APP"
mkdir -p "$APP/usr/bin" "$APP/usr/lib" "$SHARE/emu" "$APP/usr/share/licenses/mpk3-emu"

cp emu/mpk3emu "$APP/usr/bin/"
cp "$UNICORN" "$APP/usr/lib/"
python3 mpk3_emu.py --stub-image "$SHARE/custom.bin"
cp mpk3_emu.py "$SHARE/"
cp emu/panel.py emu/emu_client.py "$SHARE/emu/"

L=$APP/usr/share/licenses/mpk3-emu
cp LICENSE "$L/LICENSE-mpk3"
cp /usr/share/licenses/spdx/GPL-2.0-only.txt "$L/LICENSE-unicorn-GPL-2.0" 2>/dev/null || true
UV=$(pacman -Q unicorn 2>/dev/null | awk '{print $2}' | cut -d- -f1)
cat > "$L/NOTICE" <<EOF
MPK mini 3 emulator - https://github.com/ExonSounds/mpk-mini-3-custom-firmware (MIT, see LICENSE-mpk3).

This AppImage includes libunicorn ${UV:-2.x}, the Unicorn CPU emulator engine, unmodified, licensed under the
GNU General Public License version 2 only (LICENSE-unicorn-GPL-2.0). Because it links Unicorn, this AppImage as a
whole is distributed under GPL-2.0. Unicorn's complete source: https://github.com/unicorn-engine/unicorn
(release tag ${UV:-2.x}); the rest of the source: the repository above.

No Akai firmware is included. The custom firmware starts from a small boot stub written for this project, in
place of Akai's bootloader; run Akai's own firmware from your own copy of it.
EOF

cat > "$APP/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
if ! python3 -c "import tkinter" 2>/dev/null; then
    echo "The MPK mini 3 emulator needs python3 with Tk (e.g. Arch: tk, Debian/Ubuntu: python3-tk)." >&2
    exit 1
fi
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export MPK3_EMU_BIN="$HERE/usr/bin/mpk3emu" MPK3_EMU_CUSTOM="$HERE/usr/share/mpk3-emu/custom.bin"
exec python3 "$HERE/usr/share/mpk3-emu/mpk3_emu.py" "$@"
EOF
chmod +x "$APP/AppRun"

cat > "$APP/mpk3-emulator.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=MPK mini 3 emulator
Comment=Akai MPK mini 3 keyboard emulator running its real firmware
Exec=AppRun
Icon=mpk3-emulator
Categories=AudioVideo;Audio;Midi;
Terminal=false
EOF
cp emu/mpk3-emulator.svg "$APP/mpk3-emulator.svg"

ARCH=x86_64 appimagetool --no-appstream "$APP" "$OUT/MPK3-Emulator-x86_64.AppImage"
echo "built $OUT/MPK3-Emulator-x86_64.AppImage"

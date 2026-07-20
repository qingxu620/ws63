# -*- mode: python ; coding: utf-8 -*-
from pathlib import Path

project_root = Path(SPEC).resolve().parent
icon_path = project_root / "assets" / "app_icon.ico"
version_path = project_root / "build" / "windows_version_info.txt"

datas = [(str(icon_path), "assets")]
binaries = []
# PyInstaller's built-in hooks collect imported PySide6/requests modules, Qt
# plugins, and the CA bundle. pyserial selects its platform implementation at
# runtime, so keep the Windows backend as an explicit frozen-build fallback.
hiddenimports = [
    "serial.tools.list_ports_windows",
    "serial.win32",
]

a = Analysis(
    [str(project_root / "main.py")],
    pathex=[str(project_root)],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        "OpenSSL",
        "cryptography",
        "urllib3.contrib.pyopenssl",
        "serial.tools.list_ports_linux",
        "serial.tools.list_ports_osx",
        "serial.tools.list_ports_posix",
    ],
    noarchive=False,
    optimize=1,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="WS63_Laser_Host",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=str(icon_path),
    version=str(version_path),
    contents_directory="internal",
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    name="WS63_Laser_Host",
)

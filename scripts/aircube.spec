# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec file for AirCube application.

To build:
    pyinstaller aircube.spec

Or use the build script:
    python build_exe.py
"""

import sys
from pathlib import Path

block_cipher = None

# Get the directory containing this spec file
spec_dir = Path(SPECPATH)

a = Analysis(
    ['aircube_app.py'],
    pathex=[str(spec_dir)],
    binaries=[],
    datas=[],
    hiddenimports=[
        'PyQt6.QtWidgets',
        'PyQt6.QtCore', 
        'PyQt6.QtGui',
        'PyQt6.sip',
        'matplotlib.backends.backend_qtagg',
        'matplotlib.figure',
        'serial.tools.list_ports',
        'serial.tools.list_ports_common',
        'serial.tools.list_ports_windows',
        'serial.tools.list_ports_linux',
        'serial.tools.list_ports_osx',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        'tkinter',
        'unittest',
        'email',
        'html',
        'http',
        'xml',
        'pydoc',
    ],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name='AirCube',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,  # No console window for GUI app
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon='aircube.ico' if (spec_dir / 'aircube.ico').exists() else None,
)

# For macOS, create an app bundle
if sys.platform == 'darwin':
    app = BUNDLE(
        exe,
        name='AirCube.app',
        icon='aircube.icns' if (spec_dir / 'aircube.icns').exists() else None,
        bundle_identifier='com.stuckatprototype.aircube',
        info_plist={
            'CFBundleName': 'AirCube',
            'CFBundleDisplayName': 'AirCube',
            'CFBundleVersion': '1.0.0',
            'CFBundleShortVersionString': '1.0.0',
            'NSHighResolutionCapable': True,
        },
    )

"""
Build script for creating AirCube Tray standalone executable.
Uses PyInstaller to package the lightweight system tray app.

Usage:
    python build_tray.py

The executable will be created in the 'dist' folder.
"""
import os
import sys
import subprocess

APP_NAME = "AirCubeTray"
MAIN_SCRIPT = "aircube_tray.py"
ICON_FILE = "aircube_tray.ico"


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    
    print(f"Building {APP_NAME} executable...")
    print(f"Working directory: {script_dir}")
    
    # Check if PyInstaller is installed
    try:
        import PyInstaller
        print(f"PyInstaller version: {PyInstaller.__version__}")
    except ImportError:
        print("PyInstaller not found. Installing...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "pyinstaller"])
    
    # Build PyInstaller command
    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--name", APP_NAME,
        "--onefile",           # Single executable file
        "--windowed",          # No console window
        "--noconfirm",         # Overwrite without asking
        "--clean",             # Clean cache before building
    ]
    
    # Add icon if present
    icon_path = os.path.join(script_dir, ICON_FILE)
    if os.path.exists(icon_path):
        cmd.extend(["--icon", icon_path])
        print(f"Using icon: {icon_path}")
    else:
        # Try the main app icon
        alt_icon = os.path.join(script_dir, "aircube.ico")
        if os.path.exists(alt_icon):
            cmd.extend(["--icon", alt_icon])
            print(f"Using icon: {alt_icon}")
        else:
            print("No icon file found, building without icon")
    
    # Hidden imports
    hidden_imports = [
        "PyQt6.QtWidgets",
        "PyQt6.QtCore",
        "PyQt6.QtGui",
        "matplotlib.backends.backend_qtagg",
        "serial.tools.list_ports",
    ]
    for imp in hidden_imports:
        cmd.extend(["--hidden-import", imp])
    
    # Add the main script
    cmd.append(MAIN_SCRIPT)
    
    print("\nRunning PyInstaller...")
    print(f"Command: {' '.join(cmd)}\n")
    
    # Run PyInstaller
    result = subprocess.run(cmd)
    
    if result.returncode == 0:
        if sys.platform == "win32":
            exe_name = f"{APP_NAME}.exe"
        else:
            exe_name = APP_NAME
        
        dist_path = os.path.join(script_dir, "dist", exe_name)
        
        print("\n" + "=" * 50)
        print("BUILD SUCCESSFUL!")
        print("=" * 50)
        print(f"\nExecutable location: {dist_path}")
        print(f"Size: {os.path.getsize(dist_path) / (1024*1024):.1f} MB")
        print("\nThis lightweight tray app shows AQI in your taskbar.")
        print("Right-click the tray icon for options.")
    else:
        print("\n" + "=" * 50)
        print("BUILD FAILED!")
        print("=" * 50)
        sys.exit(1)


if __name__ == "__main__":
    main()

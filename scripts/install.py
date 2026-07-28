"""
AirCube desktop app installer.

Creates an isolated virtual environment and installs every runtime
dependency needed to run aircube_app.py (and the other helper scripts).

Usage:
    python install.py                 # create .venv and install runtime deps
    python install.py --with-build    # also install PyInstaller (for build_exe.py)
    python install.py --no-venv       # install into the current environment
    python install.py --venv-dir DIR  # use a custom venv location (default: .venv)

After it finishes it prints exactly how to launch the app.
"""

import argparse
import os
import subprocess
import sys
import venv

MIN_PYTHON = (3, 8)
APP_SCRIPT = "aircube_app.py"
REQUIREMENTS = "requirements.txt"
BUILD_PACKAGE = "pyinstaller>=6.0"

# Import names used to verify the install actually works.
VERIFY_IMPORTS = ["serial", "matplotlib", "PyQt6.QtWidgets"]


def fail(msg):
    print(f"\n[ERROR] {msg}", file=sys.stderr)
    sys.exit(1)


def venv_python(venv_dir):
    """Return the path to the python executable inside a venv."""
    if os.name == "nt":
        return os.path.join(venv_dir, "Scripts", "python.exe")
    return os.path.join(venv_dir, "bin", "python")


def run(cmd):
    print(f"  $ {' '.join(cmd)}")
    result = subprocess.run(cmd)
    if result.returncode != 0:
        fail(f"command failed ({result.returncode}): {' '.join(cmd)}")


def main():
    parser = argparse.ArgumentParser(description="Install AirCube desktop app dependencies.")
    parser.add_argument("--no-venv", action="store_true",
                        help="Install into the current Python environment instead of a venv.")
    parser.add_argument("--venv-dir", default=".venv",
                        help="Virtual environment directory (default: .venv).")
    parser.add_argument("--with-build", action="store_true",
                        help="Also install PyInstaller for building a standalone executable.")
    args = parser.parse_args()

    if sys.version_info < MIN_PYTHON:
        fail(f"Python {MIN_PYTHON[0]}.{MIN_PYTHON[1]}+ is required, "
             f"but you are running {sys.version.split()[0]}.")

    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    req_path = os.path.join(script_dir, REQUIREMENTS)
    if not os.path.exists(req_path):
        fail(f"{REQUIREMENTS} not found next to this installer.")

    print("=" * 60)
    print(" AirCube desktop app installer")
    print("=" * 60)
    print(f" Python:      {sys.version.split()[0]}")
    print(f" Working dir: {script_dir}")

    # 1. Decide which python to install into.
    if args.no_venv:
        py = sys.executable
        print(" Target:      current environment (no venv)")
    else:
        venv_dir = os.path.abspath(args.venv_dir)
        print(f" Target:      virtual environment at {venv_dir}")
        if not os.path.exists(venv_python(venv_dir)):
            print("\n[1/4] Creating virtual environment...")
            venv.EnvBuilder(with_pip=True, clear=False).create(venv_dir)
        else:
            print("\n[1/4] Reusing existing virtual environment.")
        py = venv_python(venv_dir)
        if not os.path.exists(py):
            fail(f"venv python not found at {py}")

    # 2. Upgrade pip tooling.
    print("\n[2/4] Upgrading pip / setuptools / wheel...")
    run([py, "-m", "pip", "install", "--upgrade", "pip", "setuptools", "wheel"])

    # 3. Install requirements.
    print("\n[3/4] Installing runtime requirements...")
    run([py, "-m", "pip", "install", "-r", req_path])
    if args.with_build:
        print("\n      Installing build dependency (PyInstaller)...")
        run([py, "-m", "pip", "install", BUILD_PACKAGE])

    # 4. Verify the important imports resolve.
    print("\n[4/4] Verifying installation...")
    check = "import importlib; " + "; ".join(
        f"importlib.import_module('{mod}')" for mod in VERIFY_IMPORTS
    ) + "; print('ok')"
    result = subprocess.run([py, "-c", check])
    if result.returncode != 0:
        fail("dependency verification failed - see errors above.")

    # Done - tell the user how to run.
    print("\n" + "=" * 60)
    print(" INSTALL SUCCESSFUL")
    print("=" * 60)
    app_path = os.path.join(script_dir, APP_SCRIPT)
    if args.no_venv:
        print("\nRun the app with:")
        print(f'  "{py}" "{app_path}"')
    else:
        if os.name == "nt":
            activate = os.path.join(args.venv_dir, "Scripts", "activate")
            print("\nRun the app with either:")
            print(f'  "{py}" {APP_SCRIPT}')
            print("or activate the environment first:")
            print(f"  {activate}")
            print(f"  python {APP_SCRIPT}")
        else:
            activate = os.path.join(args.venv_dir, "bin", "activate")
            print("\nRun the app with either:")
            print(f'  "{py}" {APP_SCRIPT}')
            print("or activate the environment first:")
            print(f"  source {activate}")
            print(f"  python {APP_SCRIPT}")
    if args.with_build:
        print("\nBuild a standalone executable with:")
        print(f'  "{py}" build_exe.py')
    print()


if __name__ == "__main__":
    main()

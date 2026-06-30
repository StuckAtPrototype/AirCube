"""Shared pytest fixtures. Forces Qt offscreen so tests need no display."""
import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest

try:
    from PyQt6.QtWidgets import QApplication
    HAVE_QT = True
except ImportError:  # pragma: no cover
    HAVE_QT = False


@pytest.fixture(scope="session")
def qapp():
    if not HAVE_QT:
        pytest.skip("PyQt6 not available")
    app = QApplication.instance() or QApplication([])
    yield app

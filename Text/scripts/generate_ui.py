#!/usr/bin/env python3
"""Check/regenerate the existing Text catalog without a browser or native build."""
from pathlib import Path
import runpy
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
runpy.run_path(str(Path(__file__).resolve().parents[1] / 'tools/make_catalog.py'), run_name='__main__')

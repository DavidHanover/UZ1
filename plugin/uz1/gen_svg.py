#!/usr/bin/env python3
"""Writes uz1.svg, the screen's SVG: empty. The whole picture (the ratchet, the bolts, the labels, the keyboard) is
drawn by uz1.c with op1_raster_line and filled pixels; the screen object still needs an SVG.
Copyright (C) 2026 David Hanover. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
from pathlib import Path

HERE = Path(__file__).resolve().parent
(HERE / 'uz1.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160"/>\n')
print('uz1.svg', (HERE / 'uz1.svg').stat().st_size, 'bytes')

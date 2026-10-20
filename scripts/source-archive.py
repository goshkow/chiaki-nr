#!/usr/bin/env python3
"""Архив только отслеживаемых исходников, включая submodules."""
from pathlib import Path
import subprocess
import sys
import zipfile
root=Path(__file__).resolve().parents[1]
files=subprocess.run(['git','ls-files','--recurse-submodules','-z'],cwd=root,check=True,capture_output=True).stdout.split(b'\0')
with zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED) as archive:
    for item in files:
        if not item:continue
        name=item.decode('utf-8');path=root/name
        if path.is_file():archive.write(path,'Chiaki-NR-source/'+name)

#!/usr/bin/env python3
"""Чистая упаковка из сборки и MSYS2, без пользовательских данных."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--prefix',required=True,type=Path)
parser.add_argument('--build',default='build',type=Path)
parser.add_argument('--output',required=True,type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
prefix=args.prefix.resolve();output=args.output.resolve()
if output.exists():raise SystemExit('Каталог выпуска уже существует; используйте новый пустой путь.')
output.mkdir(parents=True)
shutil.copy2(args.build/'gui/chiaki.exe',output/'chiaki.exe')
shutil.copy2(args.build/'gui/chiaki_nvngx.dll',output/'chiaki_nvngx.dll')
env=os.environ.copy();env['PATH']=str(prefix/'bin')+os.pathsep+str(prefix/'share/qt6/bin')+os.pathsep+env.get('PATH','')
env['QT_PLUGIN_PATH']=str(prefix/'share/qt6/plugins');env['QML2_IMPORT_PATH']=str(prefix/'share/qt6/qml')
subprocess.run([str(prefix/'bin/windeployqt6.exe'),'--no-translations','--qmldir',str(root/'gui/src/qml'),str(output/'chiaki.exe')],env=env,check=True)
bins={p.name.lower():p for p in (prefix/'bin').glob('*.dll')}
steam=args.build/'third-party/cpp-steam-tools/libcpp-steam-tools.dll'
if steam.is_file():bins[steam.name.lower()]=steam
queue=list(output.rglob('*.dll'))+[output/'chiaki.exe'];seen=set();owners=set()
while queue:
    binary=queue.pop()
    if binary in seen:continue
    seen.add(binary)
    headers=subprocess.run(['objdump','-p',str(binary)],env=env,check=True,capture_output=True,text=True).stdout
    for name in re.findall(r'DLL Name:\s*(\S+)',headers):
        source=bins.get(name.lower())
        if source:
            dest=output/source.name
            if not dest.exists():shutil.copy2(source,dest);queue.append(dest)
            owners.add(str(source))
        elif not name.lower().startswith(('api-ms-','ext-ms-')) and not (Path(os.environ['WINDIR'])/'System32'/name).exists():
            raise SystemExit('Не найдена зависимость: '+name+' для '+str(binary))
# лицензии пакетов и сведения для получения соответствующих исходников.
shutil.copytree(prefix/'share/licenses',output/'Licenses/MSYS2')
for name in ['COPYING','LICENSE-NVIDIA-NIS.txt','LICENSE-OptiScaler-forwarder.txt','README.md','README.upstream.md','CHANGELOG.md','THIRD_PARTY.md','BUILD-NR.md']:
    shutil.copy2(root/name,output/name)
shutil.copytree(root/'LICENSES',output/'Licenses/Chiaki')
shutil.copytree(root/'third-party/nr-sdk',output/'Licenses/SDK',ignore=shutil.ignore_patterns('include'))
for p in (root/'packaging').iterdir():shutil.copy2(p,output/p.name)
(output/'portable.txt').write_text('Chiaki NR portable\n',encoding='utf-8')
packages=subprocess.run(['pacman','-Q'],check=True,capture_output=True,text=True).stdout
(output/'BUILD-DEPENDENCIES.txt').write_text(packages,encoding='utf-8')
for p in output.rglob('*'):
    if p.is_file() and (p.name.lower().startswith('nvngx_') or p.name.lower()=='amd_fidelityfx_vk.dll'):
        raise SystemExit('В публичный архив попала закрытая или неиспользуемая runtime DLL: '+str(p))
manifest=''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(output).as_posix()}\n' for p in sorted(output.rglob('*')) if p.is_file())
(output/'SHA256SUMS.txt').write_text(manifest,encoding='utf-8')
print(output)

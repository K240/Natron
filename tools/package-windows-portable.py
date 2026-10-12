"""Assemble a self-contained Release candidate; run with uv and pefile==2024.8.26."""
import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
import pefile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--qt-root', type=Path, default=Path('D:/Qt/6.11.2/msvc2022_64'))
parser.add_argument('--build', type=Path, default=ROOT/'build/windows-qt6')
parser.add_argument('--destination', type=Path, required=True)
parser.add_argument('--zip', type=Path, required=True)
args = parser.parse_args()
destination = args.destination.resolve()
archive = args.zip.resolve()
assert not destination.exists() and not archive.exists(), 'Use fresh output paths.'
assert 'CMAKE_BUILD_TYPE:STRING=Release' in (args.build/'CMakeCache.txt').read_text(encoding='utf-8')
python = Path(sys.base_prefix)
site = ROOT/'.venv/Lib/site-packages'
qt = args.qt_root.resolve()
runtime = ROOT/'build/ofx-runtime'
destination.mkdir(parents=True)
origins = {}


def sha(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()


def copy(source, relative):
    target = destination/relative
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        assert sha(source) == sha(target), f'Conflicting package file: {relative}'
        return
    shutil.copyfile(source, target)
    origins[target.relative_to(destination).as_posix()] = str(source.resolve())


def tree(source, relative, exclude=()):
    assert source.is_dir(), source
    for path in sorted(source.rglob('*')):
        parts = path.relative_to(source).parts
        if path.is_file() and not any(p in ('__pycache__', '.git') or p in exclude for p in parts) and path.suffix != '.pyc':
            copy(path, Path(relative)/path.relative_to(source))


for directory, executable in (('App','Natron.exe'),('Renderer','NatronRenderer.exe'),('PythonBin','natron-python.exe')):
    copy(args.build/directory/executable, Path('bin')/executable)
tree(python/'Lib', 'Lib', exclude=('site-packages', 'test', 'tests', 'ensurepip', 'idlelib', 'tkinter', 'turtledemo'))
tree(python/'DLLs', 'DLLs')
for name in ('python3.dll','python314.dll'):
    copy(python/name, Path('bin')/name)
copy(python/'LICENSE.txt', 'licenses/Python/LICENSE.txt')
for package in ('qtpy', 'packaging'):
    tree(site/package, Path('Lib/site-packages')/package)
for package in ('PySide6','shiboken6'):
    source = site/package
    for path in source.glob('*.py'):
        copy(path, Path('Lib/site-packages')/package/path.name)
    # The support Python package is needed by the binding signature machinery.
    if (source/'support').is_dir():
        tree(source/'support', Path('Lib/site-packages')/package/'support')
for name in ('QtCore','QtGui','QtWidgets','QtNetwork','QtXml','QtConcurrent','QtSvg','QtPrintSupport','QtTest'):
    copy(site/'PySide6'/f'{name}.pyd', Path('Lib/site-packages/PySide6')/f'{name}.pyd')
copy(site/'shiboken6/Shiboken.pyd', 'Lib/site-packages/shiboken6/Shiboken.pyd')
copy(site/'shiboken6/shiboken6.abi3.dll', 'Lib/site-packages/shiboken6/shiboken6.abi3.dll')
copy(qt/'plugins/platforms/qwindows.dll', 'qtplugins/platforms/qwindows.dll')
for name in ('qgif','qico','qjpeg','qsvg','qtga','qtiff','qwbmp','qwebp'):
    copy(qt/'plugins/imageformats'/f'{name}.dll', Path('qtplugins/imageformats')/f'{name}.dll')
for category in ('tls','networkinformation'):
    for path in (qt/'plugins'/category).glob('*.dll'):
        if not path.stem.endswith('d'):
            copy(path, Path('qtplugins')/category/path.name)
tree(runtime, 'ofx-runtime')
for name in ('Misc','CImg','Arena','GMIC','IO'):
    tree(ROOT/'build/Plugins'/f'{name}.ofx.bundle', Path('Plugins/OFX/Natron')/f'{name}.ofx.bundle')
tree(ROOT/'OpenColorIO-Configs', 'Resources/OpenColorIO-Configs')
tree(ROOT/'build/vcpkg_installed/x64-windows/etc/fonts', 'Resources/etc/fonts')
tree(ROOT/'tools/license', 'licenses/Natron-components')
copy(ROOT/'LICENSE.txt', 'licenses/Natron/LICENSE.txt')
tree(ROOT/'build/msys64/ucrt64/share/licenses', 'licenses/MSYS2')
for package in (ROOT/'build/vcpkg_installed/x64-windows/share').iterdir():
    if (package/'copyright').is_file():
        copy(package/'copyright', Path('licenses/vcpkg')/package.name/'copyright')
for path in site.glob('*.dist-info/licenses/**/*'):
    if path.is_file() and path.parts[-3] != '__pycache__':
        copy(path, Path('licenses/Python-packages')/path.relative_to(site))
for name in ('LICENSE','LICENSE.txt','COPYING'):
    for source in (ROOT/'build/ofx-src/libultrahdr', qt, qt/'share'):
        if (source/name).is_file():
            copy(source/name, Path('licenses')/source.name/name)

# Resolve both ordinary and delay imports. Keep the SDK Qt DLLs together,
# rather than mixing their implementation with the wheel's bundled Qt DLLs.
search = [qt/'bin', args.build/'App', ROOT/'build/vcpkg_installed/x64-windows/bin',
          runtime/'bin', python, python/'DLLs', site/'PySide6', site/'shiboken6']
index = {}
for folder in search:
    for path in folder.glob('*.dll'):
        index.setdefault(path.name.casefold(), path)
system = Path(os.environ['WINDIR'])/'System32'
forbidden = {'opengl32.dll','opengl32sw.dll','libegl.dll','libglesv2.dll','qt6opengl.dll','qt6openglwidgets.dll'}
queue = deque(p for p in destination.rglob('*') if p.suffix.casefold() in ('.exe','.dll','.pyd','.ofx'))
seen, records = set(), []
while queue:
    path = queue.popleft()
    if path in seen:
        continue
    seen.add(path)
    with pefile.PE(str(path), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
                                              pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT']])
        imports = sorted({entry.dll.decode('ascii') for field in ('DIRECTORY_ENTRY_IMPORT','DIRECTORY_ENTRY_DELAY_IMPORT')
                          for entry in getattr(pe, field, [])})
    assert not forbidden.intersection(n.casefold() for n in imports), (path, imports)
    records.append({'file':path.relative_to(destination).as_posix(), 'imports':imports})
    for name in imports:
        lower = name.casefold()
        if lower.startswith(('api-ms-','ext-ms-')):
            continue
        # App-local runtime libraries take precedence over System32 copies.
        if lower in index:
            target = destination/'bin'/name
            if not target.exists():
                copy(index[lower], Path('bin')/name)
                queue.append(target)
        elif not (system/name).is_file():
            raise RuntimeError(f'Unresolved dependency {name} of {path}')

(destination/'bin/qt.conf').write_text('[Paths]\nPrefix=..\nPlugins=qtplugins\n', encoding='utf-8')
environment = r'''@echo off
setlocal
set "NATRON_PORTABLE_ROOT=%~dp0"
set "PATH=%~dp0bin;%~dp0ofx-runtime\bin;%~dp0Lib\site-packages\PySide6;%~dp0Lib\site-packages\shiboken6;%PATH%"
set "PYTHONHOME=%~dp0"
set "PYTHONPATH=%~dp0Lib\site-packages;%~dp0Plugins"
set "PYTHONNOUSERSITE=1"
set "QT_PLUGIN_PATH=%~dp0qtplugins"
set "OCIO=%~dp0Resources\OpenColorIO-Configs\blender\config.ocio"
set "OFX_PLUGIN_PATH=%~dp0Plugins\OFX\Natron"
set "FONTCONFIG_PATH=%~dp0Resources\etc\fonts"
set "FONTCONFIG_FILE=%~dp0Resources\etc\fonts\fonts.conf"
set "MAGICK_CONFIGURE_PATH=%~dp0ofx-runtime\etc\ImageMagick-7"
for /d %%D in ("%~dp0ofx-runtime\lib\ImageMagick-*") do (
  set "MAGICK_CODER_MODULE_PATH=%%~fD\modules-Q16HDRI\coders"
  set "MAGICK_FILTER_MODULE_PATH=%%~fD\modules-Q16HDRI\filters"
)
'''
for label, exe in (('Natron','Natron.exe'),('NatronRenderer','NatronRenderer.exe'),('NatronPython','natron-python.exe')):
    (destination/f'{label}.cmd').write_text(environment+f'"%~dp0bin\\{exe}" %*\nexit /b %errorlevel%\n', encoding='utf-8', newline='\r\n')
(destination/'README.txt').write_text(
    'Windows x64 Qt 6 / Vulkan Release candidate\n\n'
    'Start Natron.cmd. NatronRenderer.cmd and NatronPython.cmd provide command-line tools.\n'
    'Requires Windows 10 1903 or later / Windows 11 x64, and a Vulkan 1.3 driver for native GPU rendering.\n'
    'Qt SDK, Python, uv and Vulkan SDK are not required on the target machine.\n'
    'This is a local acceptance candidate; full migration and redistribution qualification remain open.\n'
    'Components and source revisions are recorded in provenance.json and licenses/.\n', encoding='utf-8')
provenance = {'schemaVersion':1,'buildType':'Release','qtVersion':'6.11.2','pythonVersion':sys.version,
              'packageToolSha256':sha(Path(__file__)),
              'natronCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              'workingTreeModified':True,'ofxSources':json.loads((ROOT/'build-configs/windows-ofx/sources.json').read_text()),
              'inputSourceFiles':origins,'legacyGraphicsImportViolations':0,'peImports':records,
              'scope':'Local portable acceptance candidate; not a signed installer or completed migration'}
(destination/'provenance.json').write_text(json.dumps(provenance,indent=2),encoding='utf-8')
files = {p.relative_to(destination).as_posix():sha(p) for p in sorted(destination.rglob('*')) if p.is_file()}
(destination/'manifest.json').write_text(json.dumps({'passed':True,'files':files,'peFilesChecked':len(records)},indent=2),encoding='utf-8')
archive.parent.mkdir(parents=True,exist_ok=True)
with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as package:
    for path in sorted(destination.rglob('*')):
        if path.is_file():
            info = zipfile.ZipInfo(path.relative_to(destination).as_posix(), date_time=(2020,1,1,0,0,0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            package.writestr(info,path.read_bytes())
print(json.dumps({'passed':True,'directory':str(destination),'zip':str(archive),'zipSha256':sha(archive),
                  'files':len(files)+1,'peFilesChecked':len(records),'bytes':archive.stat().st_size}))

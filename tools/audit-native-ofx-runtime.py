"""Audit every shipped OFX and codec DLL's direct Windows graphics imports."""
import argparse
import hashlib
import json
from pathlib import Path
import pefile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--report', type=Path, default=root / 'build/native-ofx-runtime-audit.json')
args = parser.parse_args()
files = sorted(set((root / 'build/ofx-runtime').rglob('*.dll')) |
               set((root / 'build/Plugins').rglob('*.ofx')) |
               set((root / 'build/Plugins').rglob('*.dll')) |
               {root / 'build/windows-qt6/App/Natron.exe', root / 'build/windows-qt6/Renderer/NatronRenderer.exe'})
records = []
for path in files:
    with pefile.PE(str(path), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
                                              pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT']])
        imports = sorted({entry.dll.decode() for field in ('DIRECTORY_ENTRY_IMPORT', 'DIRECTORY_ENTRY_DELAY_IMPORT')
                          for entry in getattr(pe, field, [])})
    graphics = [name for name in imports if name.lower() in ('opengl32.dll', 'libegl.dll', 'libglesv2.dll',
                                                           'egl.dll', 'glesv2.dll', 'glew32.dll')]
    records.append({'file': str(path.relative_to(root)), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                    'imports': imports, 'legacyGraphicsImports': graphics})
report = {'passed': not any(record['legacyGraphicsImports'] for record in records), 'filesChecked': len(records),
          'runtimeBinDlls': len(list((root / 'build/ofx-runtime/bin').glob('*.dll'))),
          'scope': 'Application, shipped OFX and codec PE normal/delay imports', 'files': records}
args.report.parent.mkdir(parents=True, exist_ok=True)
args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps({key: value for key, value in report.items() if key != 'files'}))
assert report['passed'], [(record['file'], record['legacyGraphicsImports']) for record in records if record['legacyGraphicsImports']]

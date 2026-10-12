"""Audit retired OpenGL sources, active C++ calls and generated build dependencies."""
import argparse
import hashlib
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--report', type=Path, default=root / 'build/native-engine-source-audit.json')
args = parser.parse_args()
retired = ['Engine/' + name for name in (
    'GLShader.cpp', 'GLShader.h', 'GPUContextPool.cpp', 'GPUContextPool.h',
    'Texture.cpp', 'Texture.h', 'OSGLContext.cpp', 'OSGLContext.h',
    'OSGLContext_mac.cpp', 'OSGLContext_mac.h', 'OSGLContext_win.cpp', 'OSGLContext_win.h',
    'OSGLContext_wayland.cpp', 'OSGLContext_wayland.h', 'OSGLContext_x11.cpp',
    'OSGLContext_x11.h', 'OSGLContext_xdg.h')]
retired += ['Global/GLIncludes.h', 'Global/glad_source.c',
            'Global/gladDeb/include/glad/glad.h', 'Global/gladDeb/src/glad.c',
            'Global/gladRel/include/glad/glad.h', 'Global/gladRel/src/glad.c',
            'Tests/OSGLContext_Test.cpp', 'tools/utils/generate_glad.sh']
# Preserve newlines so diagnostics refer to the source file's actual line.
lexical = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
def code_only(source):
    return lexical.sub(lambda m: re.sub(r'[^\n]', ' ', m.group()), source)

forbidden = re.compile(r'\b(?:gl[A-Z]\w*|wgl\w*|glX\w*|egl[A-Z]\w*)\s*\('
                       r'|\b(?:OSGLContext\w*|GLShader|GPUContextPool|QOpenGL\w*|eStorageModeGLTex)\b'
                       r'|\bGL_[A-Z0-9_]+\b|\bOFX_SUPPORTS_OPENGLRENDER\b')
files = {}
violations = []
for directory in ('Engine', 'Gui', 'Global', 'HostSupport', 'Tests', 'App', 'Renderer', 'Gpu', 'PythonBin'):
    for path in sorted((root / directory).rglob('*')):
        if path.suffix not in ('.cpp', '.h', '.c'):
            continue
        source = path.read_text(encoding='utf-8-sig', errors='replace')
        relative = path.relative_to(root).as_posix()
        files[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
        for match in forbidden.finditer(code_only(source)):
            violations.append({'file': relative, 'line': source.count('\n', 0, match.start()) + 1,
                               'match': match.group()})
        if re.search(r'^\s*#\s*include\s*[<"](?:[^>"\n]*/)?(?:GLIncludes\.h|glad/glad\.h|GL/[^>"\n]+|OpenGL/[^>"\n]+)', source, re.M):
            violations.append({'file': relative, 'match': 'legacy OpenGL include'})

build = root / 'build/windows-qt6/build.ninja'
build_text = build.read_text(encoding='utf-8')
build_pattern = re.compile(r'OFX_SUPPORTS_OPENGLRENDER|glad_source|gladDeb|gladRel|OSGLContext'
                           r'|GLShader\.cpp|GPUContextPool\.cpp|Engine[\\/]Texture\.cpp'
                           r'|Qt6OpenGL|OpenGLWidgets|(?i:opengl32\.lib|libegl|libgles|libgl\.so)')
build_violations = sorted(set(build_pattern.findall(build_text)))
host = (root / 'Engine/OfxHost.cpp').read_text(encoding='utf-8')
host_contract = bool(re.search(r'kOfxOpenGLRenderSuite\)\s*==\s*0\)\s*\{\s*return nullptr;', host))
host_contract &= bool(re.search(r'kOfxImageEffectPropOpenGLRenderSupported,.*"false"', host))
present = [name for name in retired if (root / name).exists()]
report = {'passed': not (present or violations or build_violations) and host_contract,
          'scope': 'Owned C++ sources and current Windows Qt 6 build; third-party SDK declarations excluded',
          'retiredFiles': retired, 'retiredFilesStillPresent': present, 'sourceFilesChecked': len(files),
          'legacyApiViolations': violations, 'buildViolations': build_violations,
          'ofxOpenGLSuiteDisabled': host_contract, 'fileSha256': files,
          'buildNinjaSha256': hashlib.sha256(build.read_bytes()).hexdigest()}
args.report.parent.mkdir(parents=True, exist_ok=True)
args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps({key: value for key, value in report.items() if key != 'fileSha256'}))
if not report['passed']:
    raise SystemExit(1)

from pathlib import Path
import struct
import subprocess

root = Path.cwd()
output = root / 'build/reader-vulkan-fixtures'
output.mkdir(parents=True, exist_ok=True)
width, height = 43, 29
pixels = [((x * 3 + y * 7 + c * 13) % 79) / 38.0 - .125
          for y in range(height) for x in range(width) for c in range(3)]
source = output / 'input.pfm'
source.write_bytes(f'PF\n{width} {height}\n-1.0\n'.encode() + struct.pack('<' + 'f' * len(pixels), *pixels))
tool = root / 'build/msys64/ucrt64/bin/oiiotool.exe'
for name, origin, alpha in [('negative', '-7+11', '0.37'), ('zero', '+0+0', '0.37'),
                            ('transparent', '+0+0', '0'), ('epsilon', '+0+0', '1e-8'), ('negative-alpha', '+0+0', '-0.125')]:
    subprocess.run([str(tool), str(source), '--ch', f'R,G,B,A={alpha}', '--origin', origin,
                    '--fullsize', '43x29+0+0', '-d', 'float', '-o', str(output / f'{name}.exr')], check=True)
(output / 'reader.ocio').write_text('''ocio_profile_version: 2
search_path: .
strictparsing: true
roles:
  default: linear
  scene_linear: linear
displays:
  Test:
    - !<View> {name: Linear, colorspace: linear}
active_displays: [Test]
active_views: [Linear]
colorspaces:
  - !<ColorSpace>
    name: linear
    family: Test
    bitdepth: 32f
    isdata: false
    allocation: uniform
  - !<ColorSpace>
    name: graded
    family: Test
    bitdepth: 32f
    isdata: false
    allocation: uniform
    from_scene_reference: !<CDLTransform> {slope: [0.83, 1.07, 0.92], offset: [0.017, 0.025, 0.035], power: [0.85, 1.15, 0.9], sat: 0.8}
''', encoding='utf-8')

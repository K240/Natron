"""Fetch a small, attributed real RGB plate for the optional CameraTracker smoke.

Run with uv run --frozen --with imageio-ffmpeg python
tools/prepare-camera-tracker-test-data.py. No depth or ground-truth poses are used.
"""
import argparse
import hashlib
import json
import subprocess
import urllib.request
from pathlib import Path

import imageio_ffmpeg

URL = 'https://webshare.cvg.cit.tum.de/g/rgbd/dataset/freiburg3/rgbd_dataset_freiburg3_long_office_household-rgb.avi'
SHA256 = '3200d7a76f5b3307e33863d4b38dbad414527ce14084f4c1f6303a0f69eb794c'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('build/camera-tracker-real'))
    parser.add_argument('--frames', type=int, default=45)
    args = parser.parse_args()
    if not 25 <= args.frames <= 300:
        parser.error('--frames must be between 25 and 300')
    args.output.mkdir(parents=True, exist_ok=True)
    movie = args.output / 'tum-fr3-office-rgb.avi'
    if not movie.exists():
        temporary = movie.with_suffix('.download')
        with urllib.request.urlopen(URL, timeout=120) as response, temporary.open('wb') as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != SHA256:
            raise RuntimeError('Downloaded movie checksum mismatch')
        temporary.replace(movie)
    if hashlib.sha256(movie.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('Cached movie checksum mismatch')
    plate = args.output / 'plate'
    plate.mkdir(exist_ok=True)
    subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), '-hide_banner', '-loglevel', 'error',
                    '-y', '-i', str(movie), '-frames:v', str(args.frames),
                    str(plate / 'plate.%04d.png')], check=True)
    attribution = {
        'title': 'TUM RGB-D Dataset — Freiburg3 long office household, RGB preview movie',
        'creator': 'Jürgen Sturm and the TUM Computer Vision Group (TUM RGB-D benchmark contributors)',
        'source': URL, 'sha256': SHA256,
        'dataset': 'https://cvg.cit.tum.de/data/datasets/rgbd-dataset',
        'license': 'Creative Commons Attribution 4.0 International',
        'license_url': 'https://creativecommons.org/licenses/by/4.0/',
        'calibration_source': 'https://cvg.cit.tum.de/data/datasets/rgbd-dataset/file_formats',
        'changes': f'First {args.frames} RGB movie frames decoded to PNG; no depth or pose inputs.',
        'width': 640, 'height': 480, 'undistorted': True,
        'fx': 535.4, 'fy': 539.2,
        'cx_pixel_edge': 320.6, 'cy_pixel_edge': 248.1,
        'coordinate_note': 'Published OpenCV pixel-center principal point (320.1, 247.6) shifted by +0.5 for Natron pixel-edge observations.',
    }
    (args.output / 'plate-attribution.json').write_text(json.dumps(attribution, indent=2, ensure_ascii=False), encoding='utf-8')
    print(f'Prepared {args.frames} frames: {plate.resolve()} (CC BY 4.0; see plate-attribution.json)')


if __name__ == '__main__':
    main()

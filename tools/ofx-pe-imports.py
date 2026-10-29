"""Read PE imports when a stripped OFX binary defeats native inspectors.

Run through uv with pefile==2024.8.26; see audit-ofx-vulkan.ps1.
"""
import argparse
import json

import pefile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary')
args = parser.parse_args()
image = pefile.PE(args.binary, fast_load=True)
try:
    image.parse_data_directories([
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
    ])
    if not hasattr(image, 'DIRECTORY_ENTRY_IMPORT'):
        raise ValueError(f'No PE import directory found: {args.binary}')
    imports = sorted({entry.dll.decode('ascii') for entry in image.DIRECTORY_ENTRY_IMPORT},
                     key=str.casefold)
    print(json.dumps(imports))
finally:
    image.close()

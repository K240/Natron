"""Exercise all eight shipped Generator overlays, including input and Undo."""
import os
import runpy

os.environ['NATRON_OFX_OVERLAY_SET'] = 'generator'
runpy.run_path('tools/io-overlay-gui-smoke.py', init_globals={'app': app})

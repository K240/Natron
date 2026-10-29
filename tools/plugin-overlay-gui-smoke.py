"""Run the shared overlay harness against shipped Misc and Arena plugins."""
import os
import runpy
os.environ['NATRON_OFX_OVERLAY_SET'] = 'remaining'
runpy.run_path('tools/io-overlay-gui-smoke.py', init_globals={'app': app})

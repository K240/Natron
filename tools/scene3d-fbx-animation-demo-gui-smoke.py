"""Prepare and validate the FBX animation example using the shared demo checks."""
import runpy
runpy.run_path('tools/scene3d-fbx-demo-gui-smoke.py',init_globals={'app':app,'animation_demo':True})

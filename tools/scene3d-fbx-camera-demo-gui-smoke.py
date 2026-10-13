"""Attach a Viewer and verify the saved ReadCamera demo and timeline."""
import runpy
runpy.run_path('tools/scene3d-fbx-demo-gui-smoke.py',init_globals={'app':app,'camera_demo':True})

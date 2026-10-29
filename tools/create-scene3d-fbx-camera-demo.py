"""Create the FBX mesh/projection demo using imported animated FBX cameras."""
import runpy
runpy.run_path('tools/create-scene3d-fbx-demo.py',init_globals={'app':app,'camera_demo':True})

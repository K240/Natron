"""Run in NatronRenderer: HDR environment lighting on three PBR spheres."""
import math
import os
import subprocess
from pathlib import Path

directory=Path.cwd()/'build/scene3d-environment-demo'
directory.mkdir(parents=True,exist_ok=True)
oiio=os.environ.get('OIIO_TOOL') or str(Path('build/msys64/ucrt64/bin/oiiotool.exe').resolve())
# A generated studio environment: broad warm/cool panels, dark ceiling and
# floor. Convert linear PPM values to float EXR radiance above 1 with OIIO.
width,height=512,256
rgb=bytearray()
for y in range(height):
    for x in range(width):
        u,v=(x+.5)/width,(y+.5)/height
        left=math.exp(-((u-.25)/.10)**8-((v-.35)/.22)**8)
        right=math.exp(-((u-.68)/.08)**8-((v-.42)/.24)**8)
        top=math.exp(-((u-.50)/.25)**8-((v-.17)/.04)**8)
        for a,b,c in ((1.,.30,.75),(.70,.65,.80),(.38,1.,1.)):
            rgb.append(round(255*min(1.,.008+a*left+b*right+c*top)))
ppm=directory/'studio-linear.ppm';ppm.write_bytes(f'P6\n{width} {height}\n255\n'.encode()+rgb)
subprocess.run([oiio,str(ppm),'--mulc','5','-d','float','-o',str(directory/'studio.exr')],check=True,capture_output=True)

# Triangulated smooth geometry exercises the shared USD mesh adapter; all
# editable surface parameters and the environment light use native nodes.
points=[]
segments,rings=32,16
for ring in range(rings+1):
    theta=math.pi*ring/rings
    for segment in range(segments):
        phi=2*math.pi*segment/segments
        points.append((math.sin(theta)*math.cos(phi),math.cos(theta),math.sin(theta)*math.sin(phi)))
triangles=[]
for ring in range(rings):
    for segment in range(segments):
        a=ring*segments+segment;b=ring*segments+(segment+1)%segments;c=a+segments;d=b+segments
        if ring>0:triangles.append((a,b,c))
        if ring<rings-1:triangles.append((b,d,c))
vectors=', '.join('('+','.join(f'{v:.8f}' for v in point)+')' for point in points)
indices=', '.join(str(v) for triangle in triangles for v in triangle)
mesh_file=directory/'sphere.usda'
mesh_file.write_text('''#usda 1.0
( metersPerUnit = 1
  upAxis = "Y"
)
def Mesh "Sphere" {
 uniform token subdivisionScheme = "none"
 point3f[] points = ['''+vectors+''']
 normal3f[] normals = ['''+vectors+'''] ( interpolation = "vertex" )
 int[] faceVertexCounts = ['''+', '.join('3' for _ in triangles)+''']
 int[] faceVertexIndices = ['''+indices+''']
}
''',encoding='utf-8')

app.addFormat('EnvironmentDemo 640x360 1');app.getProjectParam('outputFormat').setValue('EnvironmentDemo')
app.getProjectParam('frameRange').set(1,25);app.getProjectParam('frameRate').setValue(24.)

def node(kind,name):
    effect=app.createNode('fr.inria.built-in.'+kind);assert effect
    effect.setScriptName(name);effect.setLabel(name);return effect

scene=node('Scene','EnvironmentScene')
white=app.createNode('net.sf.openfx.ConstantPlugin');white.setScriptName('SurfaceWhite');white.setLabel('SurfaceWhite')
white.getParam('color').set(1.,1.,1.,1.)
for index,(name,x,color,metallic,roughness) in enumerate((
    ('SmoothGold',-2.25,(1.,.65,.25),1.,.08),
    ('RoughGold',0.,(1.,.65,.25),1.,.55),
    ('BluePlastic',2.25,(.08,.25,.75),0.,.45))):
    material=node('PBRMaterial',name+'Material');material.getParam('baseColor').set(*color,1.)
    material.getParam('metallic').setValue(metallic);material.getParam('roughness').setValue(roughness)
    assert material.connectInput(0,white)
    mesh=node('ReadUSD',name);mesh.getParam('file').setValue(mesh_file.as_posix());mesh.getParam('translate').set(x,0.,0.)
    assert mesh.connectInput(0,material) and scene.connectInput(index,mesh)
ground_material=node('PBRMaterial','GroundMaterial');ground_material.getParam('baseColor').set(.08,.10,.13,1.)
ground_material.getParam('roughness').setValue(.85)
ground=node('Card','Ground');ground.getParam('size').set(12.,10.);ground.getParam('translate').set(0.,-1.,0.)
ground.getParam('rotate').set(-90.,0.,0.);assert ground.connectInput(0,ground_material) and scene.connectInput(3,ground)
environment=app.createReader((directory/'studio.exr').as_posix());environment.setScriptName('StudioHDR');environment.setLabel('StudioHDR')
for name in ('ocioInputSpace','ocioOutputSpace'):environment.getParam(name).setValue('linear')
environment.getParam('filePremult').setValue(2);environment.getParam('outputPremult').setValue(2)
light=node('Light','EnvironmentLight');light.getParam('lightType').setValue('Environment');light.getParam('environmentSamples').setValue('64')
light.getParam('shadows').setValue(False);light.getParam('rotate').setValueAtTime(-25.,1,1);light.getParam('rotate').setValueAtTime(25.,25,1)
assert light.connectInput(1,environment) and scene.connectInput(4,light)
key=node('Light','ShadowLight');key.getParam('rotate').set(-35.,-30.,0.);key.getParam('intensity').setValue(.6)
assert scene.connectInput(5,key)
camera=node('Camera','RenderCamera');camera.getParam('translate').set(0.,2.5,11.);camera.getParam('rotate').set(-12.,0.,0.)
camera.getParam('focal').setValue(42.)
renderer=node('Render3D','EnvironmentRender3D');renderer.getParam('antialiasing').setValue('2x2');renderer.getParam('depthRange').set(7.,18.)
assert renderer.connectInput(0,scene) and renderer.connectInput(1,camera)
writer=app.createWriter((directory/'color-####.png').as_posix());writer.setScriptName('ColorWriter');assert writer.connectInput(0,renderer)
app.render(writer,1,1);app.getProjectParam('outputFormat').setValue('EnvironmentDemo')
for frame in (1,13,25):app.render(writer,frame,frame)
renderer.getParam('output').setValue('DepthPreview')
depth=app.createWriter((directory/'depth-preview.png').as_posix());depth.setScriptName('DepthWriter');assert depth.connectInput(0,renderer)
app.render(depth,13,13);renderer.getParam('output').setValue('Color')
assert app.saveProjectAs((directory/'environment.ntp').as_posix())
print('SCENE3D_ENVIRONMENT_DEMO',directory/'environment.ntp')

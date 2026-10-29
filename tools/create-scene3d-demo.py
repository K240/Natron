"""Run in NatronRenderer to create a self-contained editable 3D card example."""
from pathlib import Path
from PySide6.QtGui import QImage, QColor, QPainter, QBrush, QPen
from PySide6.QtCore import Qt, QRect

directory = Path.cwd() / 'build' / 'scene3d-demo'
directory.mkdir(parents=True, exist_ok=True)
app.addFormat('CardDemo 640x360 1')
app.getProjectParam('outputFormat').setValue('CardDemo')
app.getProjectParam('frameRange').set(1, 48)

def native(name, label):
    effect = app.createNode('fr.inria.built-in.'+name)
    effect.setScriptName(label)
    effect.setLabel(label)
    return effect

scene = native('Scene', 'ParallaxScene')
camera = native('Camera', 'MovingCamera')
projector = native('Camera', 'ReferenceCamera')
renderer = native('Render3D', 'Render3D')
assert renderer.connectInput(0, scene) and renderer.connectInput(1, camera)
camera.getParam('translate').setValueAtTime(-.35, 1., 0)
camera.getParam('translate').setValueAtTime(.35, 48., 0)

layers = [('Background', -2., QColor(32, 55, 95)),
          ('Middle', -1., QColor(78, 152, 181, 235)),
          ('Foreground', 0., QColor(235, 146, 69, 255))]
for index, (label, depth, color) in enumerate(layers):
    image = QImage(640, 360, QImage.Format_RGBA8888)
    image.fill(Qt.transparent)
    painter = QPainter(image)
    if index == 0:
        for y in range(360):
            painter.setPen(QColor(22+y//16, 36+y//12, 66+y//9))
            painter.drawLine(0, y, 639, y)
        painter.setBrush(QColor(244, 208, 138)); painter.setPen(Qt.NoPen)
        painter.drawEllipse(430, 45, 65, 65)
    else:
        painter.setBrush(color); painter.setPen(Qt.NoPen)
        for x, height in [(45, 120), (140, 175), (260, 145), (390, 210), (520, 100)]:
            x += (index-1)*40
            painter.drawRect(x, 360-height, 65, height)
            painter.setBrush(QColor(240, 221, 179, 170))
            for y in range(360-height+15, 345, 24):
                painter.drawRect(x+12, y, 10, 10); painter.drawRect(x+38, y, 10, 10)
            painter.setBrush(color)
    painter.end()
    path = directory / (label+'.png')
    assert image.save(str(path))
    reader = app.createReader(path.as_posix())
    projection = native('Project3D', label+'Projection')
    assert projection.connectInput(0, reader) and projection.connectInput(1, projector)
    card = native('Card', label+'Card')
    card.getParam('translate').set(0., 0., depth)
    card.getParam('size').set(6., 4.)
    assert card.connectInput(0, projection) and scene.connectInput(index, card)

writer = app.createWriter((directory/'preview.png').as_posix())
assert writer.connectInput(0, renderer)
app.render(writer, 24, 24)
assert app.saveProjectAs((directory/'parallax-cards.ntp').as_posix())
print('SCENE3D_DEMO', directory/'parallax-cards.ntp')

"""Independent pixel oracle for the full-size benchmark source and Viewer."""
from functools import lru_cache
import math
from PySide6.QtGui import QImage


def encoded(value):
    return 12.92 * value if value <= .0031308 else 1.055 * value ** (1 / 2.4) - .055


@lru_cache(maxsize=32)
def reference_bytes(width, height, frame, sw, sh, factor, depth, viewport):
    assert factor in (4, 8)
    assert depth in ('8u', '32f')
    vx, vy, vw, vh = viewport
    assert abs(vx - (width - vw) / 2) < .001 and abs(vy - (height - vh) / 2) < .001
    linear = sw * factor / vw < 1 - 1e-6
    period = 64 // factor

    @lru_cache(None)
    def pattern(x, y):
        x, y = x * factor, y * factor
        values = (sum((x + i + frame) & 63 for i in range(factor)) / factor / 128 * 1.2,
                  sum((y + i + 3 * frame) & 63 for i in range(factor)) / factor / 128 * .8,
                  sum((x + y + i + j + 7 * frame) & 63 for i in range(factor) for j in range(factor)) / factor ** 2 / 128 * 1.1)
        if depth == '8u':
            values = tuple(round(encoded(value) * 255) / 255 for value in values)
        return (*values, 1.)

    def texel(x, y):
        if x < 0 or y < 0 or x >= width // factor or y >= height // factor:
            return (0., 0., 0., 0.)
        return pattern(x % period, y % period)

    def coordinate(value):
        at = value / factor - (.5 if linear else 0)
        base = math.floor(at)
        return base, at - base if linear else 0

    xs = [(vx + (x + .5) / sw * vw) for x in range(sw)]
    ys = [(vy + vh - (y + .5) / sh * vh) for y in range(sh)]
    xlocations, ylocations = [coordinate(x) for x in xs], [coordinate(y) for y in ys]
    result = bytearray()
    for row, (y, (by, fy)) in enumerate(zip(ys, ylocations)):
        for column, (x, (bx, fx)) in enumerate(zip(xs, xlocations)):
            if not (0 <= x < width and 0 <= y < height):
                reference = (0, 0, 0, 255)
            else:
                if linear:
                    a, b, c, d = texel(bx, by), texel(bx + 1, by), texel(bx, by + 1), texel(bx + 1, by + 1)
                    values = tuple((a[i] * (1-fx) + b[i] * fx) * (1-fy) +
                                   (c[i] * (1-fx) + d[i] * fx) * fy for i in range(4))
                else:
                    values = texel(bx, by)
                if depth == '32f':
                    values = (*map(encoded, values[:3]), values[3])
                reference = tuple(round(max(0, min(1, value)) * 255) for value in values)
            result.extend(reference)
    return bytes(result)


def check_pixels(path, source_size, frame, presentation):
    image = QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
    sw, sh = presentation['surfaceSize']
    assert not image.isNull() and (image.width(), image.height()) == (sw, sh), path
    data = bytes(image.constBits())
    depth = presentation['storage0']
    reference = reference_bytes(*source_size, frame, sw, sh, 1 << presentation['mipmap0'], depth,
                                tuple(presentation['viewport']))
    maximum = max(abs(actual - expected) for actual, expected in zip(data, reference))
    # The byte Viewer uses a float high-word LUT and error diffusion. Its
    # quantization and bilinear interpolation may differ by two byte units.
    assert maximum <= (2 if depth == '8u' else 1), (str(path), maximum, depth)
    return data, {'pixels': sw * sh, 'maximum_byte_error': maximum}

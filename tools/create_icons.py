#!/usr/bin/python3

# Goxel 3D voxels editor
#
# copyright (c) 2018 Guillaume Chereau <guillaume@noctua-software.com>
#
# Goxel is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# goxel.  If not, see <http://www.gnu.org/licenses/>.

# ************************************************************************
# Create the icon atlas image from all the icons svg files

# import cairosvg
import itertools
import logging
import os
import re
import subprocess
from shutil import copyfile

import PIL.Image

_debug = bool(os.environ.get("DEBUG", "").strip())
logging.basicConfig(
    level=logging.DEBUG if _debug else logging.INFO,
    format="%(levelname)s %(message)s",
)
_log = logging.getLogger(__name__)
_log.info("create_icons: cwd=%s debug=%s", os.getcwd(), _debug)

# Convert the svg into one big png file.
# cairosvg.svg2png(url='./svg/icons.svg', write_to='/tmp/icons.png',
#                  output_width=370, output_height=352)

src_path = './tmp/icons.png'
_log.info("loading %s", os.path.abspath(src_path))
src_img = PIL.Image.open(src_path)
_log.info("source atlas size %s mode=%s", src_img.size, src_img.mode)
ret_img = PIL.Image.new('RGBA', (512, 512))

for x, y in itertools.product(range(8), range(8)):
    img = src_img.crop((x * 46 + 2, y * 46 + 2, x * 46 + 46, y * 46 + 46))
    # Make the image white in the special range.
    if y >= 2 and y < 5:
        tmp = PIL.Image.new('RGBA', (44, 44), (255, 255, 255, 255))
        tmp.putalpha(img.split()[3])
        img = tmp
    _log.debug("tile x=%d y=%d -> paste (64*x+10, 64*y+10)", x, y)
    ret_img.paste(img, (64 * x + 10, 64 * y + 10))

atlas_out = './data/images/icons.png'
_log.info("saving icon atlas %s", os.path.abspath(atlas_out))
ret_img.save(atlas_out)

# Also create the application icons (in data/icons)
icons_dir = './data/icons'
if not os.path.exists(icons_dir):
    _log.info("mkdir %s", os.path.abspath(icons_dir))
    os.makedirs(icons_dir)
icon_src = 'icon.png'
_log.info("loading %s", os.path.abspath(icon_src))
base = PIL.Image.open(icon_src).convert('RGBA')
_log.info("base icon size %s", base.size)

for size in [16, 24, 32, 48, 64, 128, 256]:
    out = os.path.join(icons_dir, 'icon%d.png' % size)
    _log.info("write %s", os.path.abspath(out))
    img = base.resize((size, size), PIL.Image.BILINEAR)
    img.save(out)

_log.info("done")

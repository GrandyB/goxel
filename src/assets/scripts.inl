/* Goxel 3D voxels editor
 *
 * copyright (c) 2018 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file is autogenerated by tools/create_assets.py */

{.path = "data/scripts/test.js", .size = 1625, .data =
    "\n"
    "import * as std from 'std'\n"
    "\n"
    "/*\n"
    " * Example of file format support in js:\n"
    " */\n"
    "/*\n"
    "goxel.registerFormat({\n"
    "  name: 'Test',\n"
    "  ext: 'test\\0*.test\\0',\n"
    "  import: function(img, path) {\n"
    "    let layer = img.addLayer()\n"
    "    let volume = layer.volume\n"
    "    volume.setAt([0, 0, 0], [255, 0, 0, 255])\n"
    "  },\n"
    "  export: function(img, path) {\n"
    "    try {\n"
    "      console.log(`Save ${path}`)\n"
    "      let out = std.open(path, 'w')\n"
    "      let volume = img.getLayersVolume()\n"
    "      volume.iter(function(p, c) {\n"
    "        out.printf(`${p.x} ${p.y}, ${p.z} => ${c.r}, ${c.g}, ${c.b}\\n`)\n"
    "      })\n"
    "      out.close()\n"
    "      console.log('done')\n"
    "    } catch(e) {\n"
    "      console.log('error', e)\n"
    "    }\n"
    "  },\n"
    "})\n"
    "*/\n"
    "\n"
    "function getRandomColor() {\n"
    "  return [\n"
    "    Math.floor(Math.random() * 256),\n"
    "    Math.floor(Math.random() * 256),\n"
    "    Math.floor(Math.random() * 256),\n"
    "    255\n"
    "  ]\n"
    "}\n"
    "\n"
    "goxel.registerScript({\n"
    "  name: 'FillRandom',\n"
    "  description: 'Fill selection with random voxels',\n"
    "  onExecute: function() {\n"
    "    let box = goxel.selection\n"
    "    if (!box) {\n"
    "      // Todo: actually show a popup.\n"
    "      console.log('Need a selection')\n"
    "      return\n"
    "    }\n"
    "    let volume = goxel.image.activeLayer.volume\n"
    "    box.iterVoxels(function(pos) {\n"
    "      volume.setAt(pos, getRandomColor())\n"
    "    })\n"
    "  }\n"
    "})\n"
    "\n"
    "goxel.registerScript({\n"
    "  name: 'Dilate',\n"
    "  onExecute: function() {\n"
    "    let volume = goxel.image.activeLayer.volume\n"
    "    volume.copy().iter(function(pos, color) {\n"
    "      for (let z = -1; z < 2; z++) {\n"
    "        for (let y = -1; y < 2; y++) {\n"
    "          for (let x = -1; x < 2; x++) {\n"
    "            volume.setAt([pos.x + x, pos.y + y, pos.z + z], color)\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "    })\n"
    "  }\n"
    "})\n"
    ""
},



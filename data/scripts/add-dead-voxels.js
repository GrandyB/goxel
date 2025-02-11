
import * as std from 'std'

let brown = [103,64,40,255];

goxel.registerScript({
  name: 'Add dead voxels (beneath lowest z)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume;
    let lowestZs = new Map();

    // Collect lowest Zs
    volume.copy().iter(function(pos, color) {
      let coord = `${pos.x},${pos.y}`; // can't use array as keys sadly
      if (!lowestZs.get(coord) || pos.z < lowestZs.get(coord)) {
        lowestZs.set(coord, pos.z);
      }
    });

    // Add brown voxels
    for (const [coord, lowestZ] of lowestZs.entries()) {
      let [x, y] = coord.split(',').map(Number);
      for (var curZ = lowestZ-1; curZ >= 0; curZ--) {
        volume.setAt([x, y, curZ], brown);
      }
    }
  }
})
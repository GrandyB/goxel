
import * as std from 'std'

goxel.registerScript({
  name: 'Remove dead voxels',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let isBrown = color.r === 103 && color.g === 64 && color.b === 40;
      let isBlack = color.r === 0 && color.g === 0 && color.b === 0;
      if (isBrown || isBlack) {
        volume.setAt(pos, [0,0,0,0]);
      }
    })
  }
})
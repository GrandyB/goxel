
import * as std from 'std'

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function light(value, diff) {
  return clamp(value + diff, 0, 255);
}

// Below two conversion methods borrowed from https://gist.github.com/mjackson/5311256#file-color-conversion-algorithms-js-L10

/**
 * Converts an RGB color value to HSV. Conversion formula
 * adapted from http://en.wikipedia.org/wiki/HSV_color_space.
 * Assumes r, g, and b are contained in the set [0, 255] and
 * returns h, s, and v in the set [0, 1].
 *
 * @param   Number  r       The red color value
 * @param   Number  g       The green color value
 * @param   Number  b       The blue color value
 * @return  Array           The HSV representation
 */
function rgbToHsv(r, g, b) {
  r /= 255, g /= 255, b /= 255;

  var max = Math.max(r, g, b), min = Math.min(r, g, b);
  var h, s, v = max;

  var d = max - min;
  s = max == 0 ? 0 : d / max;

  if (max == min) {
    h = 0; // achromatic
  } else {
    switch (max) {
      case r: h = (g - b) / d + (g < b ? 6 : 0); break;
      case g: h = (b - r) / d + 2; break;
      case b: h = (r - g) / d + 4; break;
    }

    h /= 6;
  }

  return [ h, s, v ];
}

/**
 * Converts an HSV color value to RGB. Conversion formula
 * adapted from http://en.wikipedia.org/wiki/HSV_color_space.
 * Assumes h, s, and v are contained in the set [0, 1] and
 * returns r, g, and b in the set [0, 255].
 *
 * @param   Number  h       The hue
 * @param   Number  s       The saturation
 * @param   Number  v       The value
 * @return  Array           The RGB representation
 */
function hsvToRgb(h, s, v) {
  var r, g, b;

  var i = Math.floor(h * 6);
  var f = h * 6 - i;
  var p = v * (1 - s);
  var q = v * (1 - f * s);
  var t = v * (1 - (1 - f) * s);

  switch (i % 6) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    case 5: r = v, g = p, b = q; break;
  }

  return [ r * 255, g * 255, b * 255 ];
}

function saturate(col, diff) {
  var hsv = rgbToHsv(col.r, col.g, col.b);
  hsv[1] = clamp(hsv[1] + (diff / 255), 0, 1);
  var rgb = hsvToRgb(hsv[0], hsv[1], hsv[2]);
  return rgb;
}

function adjustContrast(r, g, b, factor) {
  // Adjust each color channel using the provided clamp function
  const newR = clamp(((r - 128) * factor) + 128, 0, 255);
  const newG = clamp(((g - 128) * factor) + 128, 0, 255);
  const newB = clamp(((b - 128) * factor) + 128, 0, 255);

  return { r: newR, g: newG, b: newB };
}

goxel.registerScript({
  name: 'Increase contrast (by 20%)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let col = adjustContrast(color.r, color.g, color.b, 1.2);
      volume.setAt(pos, [col.r, col.g, col.b, color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Increase contrast (by 50%)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let col = adjustContrast(color.r, color.g, color.b, 1.5);
      volume.setAt(pos, [col.r, col.g, col.b, color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Decrease contrast (by 20%)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let col = adjustContrast(color.r, color.g, color.b, 0.8);
      volume.setAt(pos, [col.r, col.g, col.b, color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Decrease contrast (by 50%)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let col = adjustContrast(color.r, color.g, color.b, 0.5);
      volume.setAt(pos, [col.r, col.g, col.b, color.a]);
    })
  }
});


goxel.registerScript({
  name: 'Darken (by 2)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      volume.setAt(pos, [light(color.r, -2), light(color.g, -2), light(color.b, -2), color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Darken (by 5)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      volume.setAt(pos, [light(color.r, -5), light(color.g, -5), light(color.b, -5), color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Darken (by 10)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      volume.setAt(pos, [light(color.r, -10), light(color.g, -10), light(color.b, -10), color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Darken (by 20)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      volume.setAt(pos, [light(color.r, -20), light(color.g, -20), light(color.b, -20), color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Darken (by 50)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      volume.setAt(pos, [light(color.r, -50), light(color.g, -50), light(color.b, -50), color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Lighten (by 2)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      volume.setAt(pos, [light(color.r, 2), light(color.g, 2), light(color.b, 2), color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Lighten (by 5)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      volume.setAt(pos, [light(color.r, 5), light(color.g, 5), light(color.b, 5), color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Lighten (by 10)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      volume.setAt(pos, [light(color.r, 10), light(color.g, 10), light(color.b, 10), color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Lighten (by 20)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      volume.setAt(pos, [light(color.r, 20), light(color.g, 20), light(color.b, 20), color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Lighten (by 50)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      volume.setAt(pos, [light(color.r, 50), light(color.g, 50), light(color.b, 50), color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Saturate (by 5)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let s = saturate(color, 5);
      volume.setAt(pos, [s[0], s[1], s[2], color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Saturate (by 10)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let s = saturate(color, 10);
      volume.setAt(pos, [s[0], s[1], s[2], color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Saturate (by 20)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let s = saturate(color, 20);
      volume.setAt(pos, [s[0], s[1], s[2], color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Desaturate (by 5)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let s = saturate(color, -5);
      volume.setAt(pos, [s[0], s[1], s[2], color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Desaturate (by 10)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let s = saturate(color, -10);
      volume.setAt(pos, [s[0], s[1], s[2], color.a]);
    })
  }
});

goxel.registerScript({
  name: 'Desaturate (by 20)',
  onExecute: function() {
    let volume = goxel.image.activeLayer.volume
    volume.copy().iter(function(pos, color) {
      let s = saturate(color, -20);
      volume.setAt(pos, [s[0], s[1], s[2], color.a]);
    })
  }
});
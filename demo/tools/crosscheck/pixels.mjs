// The page half of the pixel crosscheck: demo/ferric.js's renderer, the one the
// page draws with, run over a picture pixels.py hands it, frame by frame at a
// fixed rate, into an RGBA8 target that is read back -- the same shape as
// `frtest --pipe`, which puts the same picture through the real plugin class.
//
// Rows go in and come out top-down, flipped at the upload and the read-back
// exactly as frtest's pipe flips them, so the two outputs line up.

import { Quad, PassBuffer } from '../../vendor/gl.js';
import { createRenderer, HOST_DEFAULTS, PRESETS, PRESET_COLUMNS } from '../../ferric.js';

function flipRows(bytes, width, height) {
  const row = width * 4;
  const out = new Uint8Array(bytes.length);
  for (let y = 0; y < height; y += 1) out.set(bytes.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
  return out;
}

window.ferricPixels = ({ rgba, width, height, host = {}, preset = 0, frames = 60, fps = 60, keep = [] }) => {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const gl = canvas.getContext('webgl2', { antialias: false, alpha: true, premultipliedAlpha: true });
  if (!gl.getExtension('EXT_color_buffer_float')) throw new Error('no EXT_color_buffer_float');
  gl.getExtension('OES_texture_float_linear');
  const quad = new Quad(gl);

  const bytes = Uint8Array.from(atob(rgba), (c) => c.charCodeAt(0));
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, flipRows(bytes, width, height));
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  const target = new PassBuffer(gl, { filter: 'nearest' }).ensure(width, height, gl.RGBA8);
  const renderer = createRenderer(gl, quad);

  const values = { ...HOST_DEFAULTS };
  const table = PRESETS[preset - 1];
  if (table) PRESET_COLUMNS.forEach((id, j) => { values[id] = table.values[j]; });
  // The host holds floats; JSON hands over doubles.
  for (const [id, v] of Object.entries(host)) values[id] = Math.fround(v);

  const shots = {};
  const pixels = new Uint8Array(width * height * 4);
  for (let f = 0; f < frames; f += 1) {
    renderer.render({ input: { texture, width, height }, params: null, width, height, time: f / fps, output: target.fbo, host: values });
    if (keep.includes(f) || f === frames - 1) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, target.fbo);
      gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
      const top = flipRows(pixels, width, height);
      let s = '';
      for (let i = 0; i < top.length; i += 0x8000) s += String.fromCharCode(...top.subarray(i, i + 0x8000));
      shots[f] = btoa(s);
    }
  }
  const info = gl.getExtension('WEBGL_debug_renderer_info');
  return { shots, renderer: info ? gl.getParameter(info.UNMASKED_RENDERER_WEBGL) : '?' };
};
window.ferricPixelsReady = true;

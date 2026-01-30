// For the record, these distance formulas were taken from the OpenAL
// spec
// (http://connect.creativelabs.com/openal/Documentation/OpenAL%201.1%20Specification.pdf),
// not the code.  The Web Audio spec follows the OpenAL formulas.

function linearDistance(panner, x, y, z) {
  let distance = Math.sqrt(x * x + y * y + z * z);
  let dref = Math.min(panner.refDistance, panner.maxDistance);
  let dmax = Math.max(panner.refDistance, panner.maxDistance);
  distance = Math.max(Math.min(distance, dmax), dref);
  let rolloff = Math.max(Math.min(panner.rolloffFactor, 1), 0);
  if (dref === dmax)
    return 1 - rolloff;

  let gain = (1 - rolloff * (distance - dref) / (dmax - dref));

  return gain;
}

function inverseDistance(panner, x, y, z) {
  let distance = Math.sqrt(x * x + y * y + z * z);
  distance = Math.max(distance, panner.refDistance);
  let rolloff = panner.rolloffFactor;
  let gain = panner.refDistance /
      (panner.refDistance +
       rolloff * (Math.max(distance, panner.refDistance) - panner.refDistance));

  return gain;
}

function exponentialDistance(panner, x, y, z) {
  let distance = Math.sqrt(x * x + y * y + z * z);
  distance = Math.max(distance, panner.refDistance);
  let rolloff = panner.rolloffFactor;
  let gain = Math.pow(distance / panner.refDistance, -rolloff);

  return gain;
}

// Simple implementations of 3D vectors implemented as a 3-element array.

// x - y
function vec3Sub(x, y) {
  let z = new Float32Array(3);
  z[0] = x[0] - y[0];
  z[1] = x[1] - y[1];
  z[2] = x[2] - y[2];

  return z;
}

// x/|x|
function vec3Normalize(x) {
  let mag = Math.hypot(...x);
  return x.map(function(c) {
    return c / mag;
  });
}

// x == 0?
function vec3IsZero(x) {
  return x[0] === 0 && x[1] === 0 && x[2] === 0;
}

// Vector cross product
function vec3Cross(u, v) {
  let cross = new Float32Array(3);
  cross[0] = u[1] * v[2] - u[2] * v[1];
  cross[1] = u[2] * v[0] - u[0] * v[2];
  cross[2] = u[0] * v[1] - u[1] * v[0];
  return cross;
}

// Dot product
function vec3Dot(x, y) {
  return x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
}

// a*x, for scalar a
function vec3Scale(a, x) {
  return x.map(function(c) {
    return a * c;
  });
}

function calculateAzimuth(source, listener, listenerForward, listenerUp) {
  let sourceListener = vec3Sub(source, listener);

  if (vec3IsZero(sourceListener))
    return 0;

  sourceListener = vec3Normalize(sourceListener);

  let listenerRight = vec3Normalize(vec3Cross(listenerForward, listenerUp));
  let listenerForwardNorm = vec3Normalize(listenerForward);

  let up = vec3Cross(listenerRight, listenerForwardNorm);
  let upProjection = vec3Dot(sourceListener, up);

  let projectedSource =
      vec3Normalize(vec3Sub(sourceListener, vec3Scale(upProjection, up)));

  let azimuth =
      180 / Math.PI * Math.acos(vec3Dot(projectedSource, listenerRight));

  // Source in front or behind the listener
  let frontBack = vec3Dot(projectedSource, listenerForwardNorm);
  if (frontBack < 0)
    azimuth = 360 - azimuth;

  // Make azimuth relative to "front" and not "right" listener vector.
  if (azimuth >= 0 && azimuth <= 270)
    azimuth = 90 - azimuth;
  else
    azimuth = 450 - azimuth;

  // We don't need elevation, so we're skipping that computation.
  return azimuth;
}

// Map our position angle to the azimuth angle (in degrees).
//
// An angle of 0 corresponds to an azimuth of 90 deg; pi, to -90 deg.
function angleToAzimuth(angle) {
  return 90 - angle * 180 / Math.PI;
}

// The gain caused by the EQUALPOWER panning model
function equalPowerGain(azimuth, numberOfChannels) {
  let halfPi = Math.PI / 2;

  if (azimuth < -90)
    azimuth = -180 - azimuth;
  else
    azimuth = 180 - azimuth;

  if (numberOfChannels == 1) {
    let panPosition = (azimuth + 90) / 180;

    let gainL = Math.cos(halfPi * panPosition);
    let gainR = Math.sin(halfPi * panPosition);

    return {left: gainL, right: gainR};
  } else {
    if (azimuth <= 0) {
      let panPosition = (azimuth + 90) / 90;

      let gainL = Math.cos(halfPi * panPosition);
      let gainR = Math.sin(halfPi * panPosition);

      return {left: gainL, right: gainR};
    } else {
      let panPosition = azimuth / 90;

      let gainL = Math.cos(halfPi * panPosition);
      let gainR = Math.sin(halfPi * panPosition);

      return {left: gainL, right: gainR};
    }
  }
}

function applyPanner(azimuth, srcL, srcR, numberOfChannels) {
  let length = srcL.length;
  let outL = new Float32Array(length);
  let outR = new Float32Array(length);

  if (numberOfChannels == 1) {
    for (let k = 0; k < length; ++k) {
      let gains = equalPowerGain(azimuth[k], numberOfChannels);

      outL[k] = srcL[k] * gains.left;
      outR[k] = srcR[k] * gains.right;
    }
  } else {
    for (let k = 0; k < length; ++k) {
      let gains = equalPowerGain(azimuth[k], numberOfChannels);

      if (azimuth[k] <= 0) {
        outL[k] = srcL[k] + srcR[k] * gains.left;
        outR[k] = srcR[k] * gains.right;
      } else {
        outL[k] = srcL[k] * gains.left;
        outR[k] = srcR[k] + srcL[k] * gains.right;
      }
    }
  }

  return {left: outL, right: outR};
}

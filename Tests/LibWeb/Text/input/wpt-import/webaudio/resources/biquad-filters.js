// A biquad filter has a z-transform of
// H(z) = (b0 + b1 / z + b2 / z^2) / (1 + a1 / z + a2 / z^2)
//
// The formulas for the various filters were taken from
// http://www.musicdsp.org/files/Audio-EQ-Cookbook.txt.


// Lowpass filter.
function createLowpassFilter(freq, q, gain) {
  let b0;
  let b1;
  let b2;
  let a0;
  let a1;
  let a2;

  if (freq == 1) {
    // The formula below works, except for roundoff.  When freq = 1,
    // the filter is just a wire, so hardwire the coefficients.
    b0 = 1;
    b1 = 0;
    b2 = 0;
    a0 = 1;
    a1 = 0;
    a2 = 0;
  } else {
    let theta = Math.PI * freq;
    let alpha = Math.sin(theta) / (2 * Math.pow(10, q / 20));
    let cosw = Math.cos(theta);
    let beta = (1 - cosw) / 2;

    b0 = beta;
    b1 = 2 * beta;
    b2 = beta;
    a0 = 1 + alpha;
    a1 = -2 * cosw;
    a2 = 1 - alpha;
  }

  return normalizeFilterCoefficients(b0, b1, b2, a0, a1, a2);
}

function createHighpassFilter(freq, q, gain) {
  let b0;
  let b1;
  let b2;
  let a0;
  let a1;
  let a2;

  if (freq == 1) {
    // The filter is 0
    b0 = 0;
    b1 = 0;
    b2 = 0;
    a0 = 1;
    a1 = 0;
    a2 = 0;
  } else if (freq == 0) {
    // The filter is 1.  Computation of coefficients below is ok, but
    // there's a pole at 1 and a zero at 1, so round-off could make
    // the filter unstable.
    b0 = 1;
    b1 = 0;
    b2 = 0;
    a0 = 1;
    a1 = 0;
    a2 = 0;
  } else {
    let theta = Math.PI * freq;
    let alpha = Math.sin(theta) / (2 * Math.pow(10, q / 20));
    let cosw = Math.cos(theta);
    let beta = (1 + cosw) / 2;

    b0 = beta;
    b1 = -2 * beta;
    b2 = beta;
    a0 = 1 + alpha;
    a1 = -2 * cosw;
    a2 = 1 - alpha;
  }

  return normalizeFilterCoefficients(b0, b1, b2, a0, a1, a2);
}

function normalizeFilterCoefficients(b0, b1, b2, a0, a1, a2) {
  let scale = 1 / a0;

  return {
    b0: b0 * scale,
    b1: b1 * scale,
    b2: b2 * scale,
    a1: a1 * scale,
    a2: a2 * scale
  };
}

function createBandpassFilter(freq, q, gain) {
  let b0;
  let b1;
  let b2;
  let a0;
  let a1;
  let a2;
  let coef;

  if (freq > 0 && freq < 1) {
    let w0 = Math.PI * freq;
    if (q > 0) {
      let alpha = Math.sin(w0) / (2 * q);
      let k = Math.cos(w0);

      b0 = alpha;
      b1 = 0;
      b2 = -alpha;
      a0 = 1 + alpha;
      a1 = -2 * k;
      a2 = 1 - alpha;

      coef = normalizeFilterCoefficients(b0, b1, b2, a0, a1, a2);
    } else {
      // q = 0, and frequency is not 0 or 1.  The above formula has a
      // divide by zero problem.  The limit of the z-transform as q
      // approaches 0 is 1, so set the filter that way.
      coef = {b0: 1, b1: 0, b2: 0, a1: 0, a2: 0};
    }
  } else {
    // When freq = 0 or 1, the z-transform is identically 0,
    // independent of q.
    coef = { b0: 0, b1: 0, b2: 0, a1: 0, a2: 0 }
  }

  return coef;
}

function createLowShelfFilter(freq, q, gain) {
  // q not used
  let b0;
  let b1;
  let b2;
  let a0;
  let a1;
  let a2;
  let coef;

  let S = 1;
  let A = Math.pow(10, gain / 40);

  if (freq == 1) {
    // The filter is just a constant gain
    coef = {b0: A * A, b1: 0, b2: 0, a1: 0, a2: 0};
  } else if (freq == 0) {
    // The filter is 1
    coef = {b0: 1, b1: 0, b2: 0, a1: 0, a2: 0};
  } else {
    let w0 = Math.PI * freq;
    let alpha = 1 / 2 * Math.sin(w0) * Math.sqrt((A + 1 / A) * (1 / S - 1) + 2);
    let k = Math.cos(w0);
    let k2 = 2 * Math.sqrt(A) * alpha;
    let Ap1 = A + 1;
    let Am1 = A - 1;

    b0 = A * (Ap1 - Am1 * k + k2);
    b1 = 2 * A * (Am1 - Ap1 * k);
    b2 = A * (Ap1 - Am1 * k - k2);
    a0 = Ap1 + Am1 * k + k2;
    a1 = -2 * (Am1 + Ap1 * k);
    a2 = Ap1 + Am1 * k - k2;
    coef = normalizeFilterCoefficients(b0, b1, b2, a0, a1, a2);
  }

  return coef;
}

function createHighShelfFilter(freq, q, gain) {
  // q not used
  let b0;
  let b1;
  let b2;
  let a0;
  let a1;
  let a2;
  let coef;

  let A = Math.pow(10, gain / 40);

  if (freq == 1) {
    // When freq = 1, the z-transform is 1
    coef = {b0: 1, b1: 0, b2: 0, a1: 0, a2: 0};
  } else if (freq > 0) {
    let w0 = Math.PI * freq;
    let S = 1;
    let alpha = 0.5 * Math.sin(w0) * Math.sqrt((A + 1 / A) * (1 / S - 1) + 2);
    let k = Math.cos(w0);
    let k2 = 2 * Math.sqrt(A) * alpha;
    let Ap1 = A + 1;
    let Am1 = A - 1;

    b0 = A * (Ap1 + Am1 * k + k2);
    b1 = -2 * A * (Am1 + Ap1 * k);
    b2 = A * (Ap1 + Am1 * k - k2);
    a0 = Ap1 - Am1 * k + k2;
    a1 = 2 * (Am1 - Ap1 * k);
    a2 = Ap1 - Am1 * k - k2;

    coef = normalizeFilterCoefficients(b0, b1, b2, a0, a1, a2);
  } else {
    // When freq = 0, the filter is just a gain
    coef = {b0: A * A, b1: 0, b2: 0, a1: 0, a2: 0};
  }

  return coef;
}

function createPeakingFilter(freq, q, gain) {
  let b0;
  let b1;
  let b2;
  let a0;
  let a1;
  let a2;
  let coef;

  let A = Math.pow(10, gain / 40);

  if (freq > 0 && freq < 1) {
    if (q > 0) {
      let w0 = Math.PI * freq;
      let alpha = Math.sin(w0) / (2 * q);
      let k = Math.cos(w0);

      b0 = 1 + alpha * A;
      b1 = -2 * k;
      b2 = 1 - alpha * A;
      a0 = 1 + alpha / A;
      a1 = -2 * k;
      a2 = 1 - alpha / A;

      coef = normalizeFilterCoefficients(b0, b1, b2, a0, a1, a2);
    } else {
      // q = 0, we have a divide by zero problem in the formulas
      // above.  But if we look at the z-transform, we see that the
      // limit as q approaches 0 is A^2.
      coef = {b0: A * A, b1: 0, b2: 0, a1: 0, a2: 0};
    }
  } else {
    // freq = 0 or 1, the z-transform is 1
    coef = {b0: 1, b1: 0, b2: 0, a1: 0, a2: 0};
  }

  return coef;
}

function createNotchFilter(freq, q, gain) {
  let b0;
  let b1;
  let b2;
  let a0;
  let a1;
  let a2;
  let coef;

  if (freq > 0 && freq < 1) {
    if (q > 0) {
      let w0 = Math.PI * freq;
      let alpha = Math.sin(w0) / (2 * q);
      let k = Math.cos(w0);

      b0 = 1;
      b1 = -2 * k;
      b2 = 1;
      a0 = 1 + alpha;
      a1 = -2 * k;
      a2 = 1 - alpha;
      coef = normalizeFilterCoefficients(b0, b1, b2, a0, a1, a2);
    } else {
      // When q = 0, we get a divide by zero above.  The limit of the
      // z-transform as q approaches 0 is 0, so set the coefficients
      // appropriately.
      coef = {b0: 0, b1: 0, b2: 0, a1: 0, a2: 0};
    }
  } else {
    // When freq = 0 or 1, the z-transform is 1
    coef = {b0: 1, b1: 0, b2: 0, a1: 0, a2: 0};
  }

  return coef;
}

function createAllpassFilter(freq, q, gain) {
  let b0;
  let b1;
  let b2;
  let a0;
  let a1;
  let a2;
  let coef;

  if (freq > 0 && freq < 1) {
    if (q > 0) {
      let w0 = Math.PI * freq;
      let alpha = Math.sin(w0) / (2 * q);
      let k = Math.cos(w0);

      b0 = 1 - alpha;
      b1 = -2 * k;
      b2 = 1 + alpha;
      a0 = 1 + alpha;
      a1 = -2 * k;
      a2 = 1 - alpha;
      coef = normalizeFilterCoefficients(b0, b1, b2, a0, a1, a2);
    } else {
      // q = 0
      coef = {b0: -1, b1: 0, b2: 0, a1: 0, a2: 0};
    }
  } else {
    coef = {b0: 1, b1: 0, b2: 0, a1: 0, a2: 0};
  }

  return coef;
}

function filterData(filterCoef, signal, len) {
  let y = new Array(len);
  let b0 = filterCoef.b0;
  let b1 = filterCoef.b1;
  let b2 = filterCoef.b2;
  let a1 = filterCoef.a1;
  let a2 = filterCoef.a2;

  // Prime the pump. (Assumes the signal has length >= 2!)
  y[0] = b0 * signal[0];
  y[1] = b0 * signal[1] + b1 * signal[0] - a1 * y[0];

  // Filter all of the signal that we have.
  for (let k = 2; k < Math.min(signal.length, len); ++k) {
    y[k] = b0 * signal[k] + b1 * signal[k - 1] + b2 * signal[k - 2] -
        a1 * y[k - 1] - a2 * y[k - 2];
  }

  // If we need to filter more, but don't have any signal left,
  // assume the signal is zero.
  for (let k = signal.length; k < len; ++k) {
    y[k] = -a1 * y[k - 1] - a2 * y[k - 2];
  }

  return y;
}

// Map the filter type name to a function that computes the filter coefficents
// for the given filter type.
let filterCreatorFunction = {
  'lowpass': createLowpassFilter,
  'highpass': createHighpassFilter,
  'bandpass': createBandpassFilter,
  'lowshelf': createLowShelfFilter,
  'highshelf': createHighShelfFilter,
  'peaking': createPeakingFilter,
  'notch': createNotchFilter,
  'allpass': createAllpassFilter
};

let filterTypeName = {
  'lowpass': 'Lowpass filter',
  'highpass': 'Highpass filter',
  'bandpass': 'Bandpass filter',
  'lowshelf': 'Lowshelf filter',
  'highshelf': 'Highshelf filter',
  'peaking': 'Peaking filter',
  'notch': 'Notch filter',
  'allpass': 'Allpass filter'
};

function createFilter(filterType, freq, q, gain) {
  return filterCreatorFunction[filterType](freq, q, gain);
}

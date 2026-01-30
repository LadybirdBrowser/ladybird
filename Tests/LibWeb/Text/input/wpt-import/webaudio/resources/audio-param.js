// Define functions that implement the formulas for AudioParam automations.

// AudioParam linearRamp value at time t for a linear ramp between (t0, v0) and
// (t1, v1).  It is assumed that t0 <= t.  Results are undefined otherwise.
function audioParamLinearRamp(t, v0, t0, v1, t1) {
  if (t >= t1)
    return v1;
  return (v0 + (v1 - v0) * (t - t0) / (t1 - t0))
}

// AudioParam exponentialRamp value at time t for an exponential ramp between
// (t0, v0) and (t1, v1). It is assumed that t0 <= t.  Results are undefined
// otherwise.
function audioParamExponentialRamp(t, v0, t0, v1, t1) {
  if (t >= t1)
    return v1;
  return v0 * Math.pow(v1 / v0, (t - t0) / (t1 - t0));
}

// AudioParam setTarget value at time t for a setTarget curve starting at (t0,
// v0) with a final value of vFainal and a time constant of timeConstant.  It is
// assumed that t0 <= t.  Results are undefined otherwise.
function audioParamSetTarget(t, v0, t0, vFinal, timeConstant) {
  return vFinal + (v0 - vFinal) * Math.exp(-(t - t0) / timeConstant);
}

// AudioParam setValueCurve value at time t for a setValueCurve starting at time
// t0 with curve, curve, and duration duration.  The sample rate is sampleRate.
// It is assumed that t0 <= t.
function audioParamSetValueCurve(t, curve, t0, duration) {
  if (t > t0 + duration)
    return curve[curve.length - 1];

  let curvePointsPerSecond = (curve.length - 1) / duration;

  let virtualIndex = (t - t0) * curvePointsPerSecond;
  let index = Math.floor(virtualIndex);

  let delta = virtualIndex - index;

  let c0 = curve[index];
  let c1 = curve[Math.min(index + 1, curve.length - 1)];
  return c0 + (c1 - c0) * delta;
}

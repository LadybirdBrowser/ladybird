// assert_equals can fail when comparing floats due to precision errors, so
// use assert_approx_equals with this constant instead
const FLOAT_EPSILON = 0.001;

// Identity matrix
const IDENTITY_MATRIX = [1, 0, 0, 0,
                         0, 1, 0, 0,
                         0, 0, 1, 0,
                         0, 0, 0, 1];

const IDENTITY_TRANSFORM = {
    position: [0, 0, 0],
    orientation: [0, 0, 0, 1],
};

// A valid pose matrix/transform for  when we don't care about specific values
// Note that these two should be identical, just different representations
const VALID_POSE_MATRIX = [0, 1, 0, 0,
                           0, 0, 1, 0,
                           1, 0, 0, 0,
                           1, 1, 1, 1];

const VALID_POSE_TRANSFORM = {
    position: [1, 1, 1],
    orientation: [0.5, 0.5, 0.5, 0.5]
};

const VALID_PROJECTION_MATRIX = [
    1,  0,  0,  0,
    0,  1,  0,  0,
    3,  2, -1, -1,
    0,  0, -0.2, 0
];

// This is a decomposed version of the above.
const VALID_FIELD_OF_VIEW = {
    upDegrees: 71.565,
    downDegrees: -45,
    leftDegrees:-63.4349,
    rightDegrees: 75.9637
};

// This is roughly equivalent to the above, but with a different near plane.
// The fact that it's the same isn't too concerning, since to be the same
// ViewGeometry it'd also need the same offset.
const VALID_DEPTH_PROJECTION_MATRIX = [
    1,  0,  0,     0,
    0,  1,  0,     0,
    3,  2, -1,    -1,
    0,  0, -0.002, 0
];

// A valid input grip matrix for  when we don't care about specific values
const VALID_GRIP = [1, 0, 0, 0,
                    0, 1, 0, 0,
                    0, 0, 1, 0,
                    4, 3, 2, 1];

const VALID_GRIP_TRANSFORM = {
    position: [4, 3, 2],
    orientation: [0, 0, 0, 1]
};

// A valid input pointer offset for  when we don't care about specific values
const VALID_POINTER = [1, 0, 0, 0,
                       0, 1, 0, 0,
                       0, 0, 1, 0,
                       0, 0, 1, 1];

const VALID_POINTER_TRANSFORM = {
    position: [0, 0, 1],
    orientation: [0, 0, 0, 1]
};

// A Valid Local to floor matrix/transform for when we don't care about specific
// values.  Note that these should be identical, just different representations.
const VALID_FLOOR_ORIGIN_MATRIX = [1, 0,    0,  0,
                                     0, 1,    0,  0,
                                     0, 0,    1,  0,
                                     1, 1.65, -1, 1];

const VALID_FLOOR_ORIGIN = {
    position: [-1.0, -1.65, 1.0],
    orientation: [0, 0, 0, 1]
};

const VALID_BOUNDS = [
    { x: 3.0, z: -2.0 },
    { x: 3.5, z: 0.0 },
    { x: 3.0, z: 2.0 },
    { x: -3.0, z: 2.0 },
    { x: -3.5, z: 0.0 },
    { x: -3.0, z: -2.0 }
];

const VALID_RESOLUTION = {
    width: 200,
    height: 200
};

const LEFT_OFFSET = {
    position: [-0.1, 0, 0],
    orientation: [0, 0, 0, 1]
};

const RIGHT_OFFSET = {
    position: [0.1, 0, 0],
    orientation: [0, 0, 0, 1]
};

// Most depth tests at present are effectively monocular, so just ensure we
// substantially overlap the left eye.
const DEPTH_OFFSET = {
    position: [-0.1, 0.01, 0],
    orientation: [0, 0, 0, 1]
};

const FIRST_PERSON_OFFSET = {
  position: [0, 0.1, 0],
  orientation: [0, 0, 0, 1]
};

const VALID_VIEWS = [{
        eye:"left",
        projectionMatrix: VALID_PROJECTION_MATRIX,
        viewOffset: LEFT_OFFSET,
        resolution: VALID_RESOLUTION
    }, {
        eye:"right",
        projectionMatrix: VALID_PROJECTION_MATRIX,
        viewOffset: RIGHT_OFFSET,
        resolution: VALID_RESOLUTION
    },
];

const VALID_SECONDARY_VIEWS = [{
        eye: "none",
        projectionMatrix: VALID_PROJECTION_MATRIX,
        viewOffset: FIRST_PERSON_OFFSET,
        resolution: VALID_RESOLUTION,
        isFirstPersonObserver: true
    }
];

const NON_IMMERSIVE_VIEWS = [{
        eye: "none",
        projectionMatrix: VALID_PROJECTION_MATRIX,
        viewOffset: IDENTITY_TRANSFORM,
        resolution: VALID_RESOLUTION,
    }
];

const ALL_FEATURES = [
  'viewer',
  'local',
  'local-floor',
  'bounded-floor',
  'unbounded',
  'hit-test',
  'dom-overlay',
  'light-estimation',
  'anchors',
  'depth-sensing',
  'secondary-views',
  'camera-access',
  'layers'
];

const TRACKED_IMMERSIVE_DEVICE = {
    supportsImmersive: true,
    supportedModes: [ "inline", "immersive-vr"],
    views: VALID_VIEWS,
    secondaryViews: VALID_SECONDARY_VIEWS,
    viewerOrigin: IDENTITY_TRANSFORM,
    supportedFeatures: ALL_FEATURES,
    environmentBlendMode: "opaque",
    interactionMode: "world-space"
};

const IMMERSIVE_AR_DEVICE = {
  supportsImmersive: true,
  supportedModes: [ "inline", "immersive-ar"],
  views: VALID_VIEWS,
  viewerOrigin: IDENTITY_TRANSFORM,
  supportedFeatures: ALL_FEATURES,
  environmentBlendMode: "additive",
  interactionMode: "screen-space"
};

const VALID_NON_IMMERSIVE_DEVICE = {
    supportsImmersive: false,
    supportedModes: ["inline"],
    views: NON_IMMERSIVE_VIEWS,
    viewerOrigin: IDENTITY_TRANSFORM,
    supportedFeatures: ALL_FEATURES,
    environmentBlendMode: "opaque",
    interactionMode: "screen-space"
};

const VALID_CONTROLLER = {
    handedness: "none",
    targetRayMode: "tracked-pointer",
    pointerOrigin: VALID_POINTER_TRANSFORM,
    profiles: []
};

const RIGHT_CONTROLLER = {
    handedness: "right",
    targetRayMode: "tracked-pointer",
    pointerOrigin: VALID_POINTER_TRANSFORM,
    profiles: []
};

const SCREEN_CONTROLLER = {
    handedness: "none",
    targetRayMode: "screen",
    pointerOrigin: VALID_POINTER_TRANSFORM,
    profiles: []
};

// From: https://immersive-web.github.io/webxr/#default-features
const DEFAULT_FEATURES = {
  "inline": ["viewer"],
  "immersive-vr": ["viewer", "local"],
  "immersive-ar": ["viewer", "local"],
};

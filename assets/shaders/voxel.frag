#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) flat in uint fragFaceIndex;
layout(location = 2) flat in uint fragMaterialTypeId;
layout(location = 3) flat in uint fragPackedLight;
layout(location = 4) in vec2 fragLocalUv;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    vec4 lightParams;     // sunDir.xyz, time
    vec4 cameraParams;    // underwaterStrength, tintR, tintG, tintB
    vec4 cameraWorldParams; // camera position xyz
    vec4 atmosphereParams;  // fog near, fog far, strength, far-light lift
} pc;

// W1 water tuning. Kept as named constants at the top so they're easy to
// dial without hunting through the shader body.
const float kWaterSeaLevel        = 0.0;   // matches NoiseTerrainSettings.seaLevel default
const float kWaterDeepTintFalloff = 24.0;  // blocks of depth before the deep tint maxes out
const vec3  kWaterDeepTint        = vec3(0.025, 0.075, 0.16);
const vec3  kWaterShallowTint     = vec3(0.18, 0.48, 0.68);
const vec3  kWaterShoreTint       = vec3(0.06, 0.58, 0.68);
const vec3  kWaterFoamColor       = vec3(0.88, 0.98, 1.0);
const vec3  kWaterTroughTint      = vec3(0.012, 0.045, 0.12);
const vec3  kWaterSkyReflection   = vec3(0.24, 0.46, 0.76);
const vec3  kWaterHorizonReflection = vec3(0.46, 0.66, 0.88);
// Animated sky-highlight: bright noise patches on the top face that drift
// with the ripple noise. Cheap, no view-vector needed.
const vec3  kWaterHighlightColor   = vec3(0.78, 0.90, 1.0);
const float kWaterHighlightStrength = 0.36;
const float kWaterHighlightCutoff   = 0.62;
const float kWaterHighlightSoftness = 0.18;

const vec3  kSunLightColor      = vec3(1.00, 0.82, 0.56);
const vec3  kSkyLightColor      = vec3(0.58, 0.72, 1.00);
const vec3  kGroundBounceColor  = vec3(0.34, 0.30, 0.24);
const vec3  kZenithFogColor     = vec3(0.44, 0.66, 0.96);
const vec3  kHorizonFogColor    = vec3(0.70, 0.82, 0.95);
const vec3  kSunHazeColor       = vec3(1.00, 0.76, 0.43);

struct MaterialData {
    vec4 baseColor;
    vec4 secondaryColor;
    vec4 noiseParams;
    vec4 textureParams;
};

layout(set = 1, binding = 0, std430) readonly buffer MaterialTable {
    MaterialData materials[];
} matTable;

// --- Noise ---

float hash3(vec3 p) {
    p = fract(p * vec3(443.8975, 397.2973, 491.1871));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float valueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float a = hash3(i);
    float b = hash3(i + vec3(1, 0, 0));
    float c = hash3(i + vec3(0, 1, 0));
    float d = hash3(i + vec3(1, 1, 0));
    float e = hash3(i + vec3(0, 0, 1));
    float g = hash3(i + vec3(1, 0, 1));
    float h = hash3(i + vec3(0, 1, 1));
    float j = hash3(i + vec3(1, 1, 1));

    return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y),
               mix(mix(e, g, f.x), mix(h, j, f.x), f.y), f.z);
}

vec2 waterWaveSlope(vec2 worldXZ, vec2 direction, float frequency, float speed, float amplitude, float phase, float t) {
    float wavePhase = dot(worldXZ, direction) * frequency + t * speed + phase;
    return direction * (cos(wavePhase) * frequency * amplitude);
}

float waterWaveSignal(vec2 worldXZ, float t) {
    vec2 longA = normalize(vec2(0.86, 0.50));
    vec2 longB = normalize(vec2(-0.42, 0.91));
    vec2 ripple = normalize(vec2(0.17, -0.98));
    float a = sin(dot(worldXZ, longA) * 0.22 + t * 0.92);
    float b = sin(dot(worldXZ, longB) * 0.36 + t * 0.68 + 1.7);
    float c = sin(dot(worldXZ, ripple) * 0.88 + t * 1.42 + 2.9);
    return a * 0.52 + b * 0.34 + c * 0.14;
}

vec3 waterWaveNormal(vec2 worldXZ, float t) {
    vec2 longA = normalize(vec2(0.86, 0.50));
    vec2 longB = normalize(vec2(-0.42, 0.91));
    vec2 ripple = normalize(vec2(0.17, -0.98));
    vec2 slope =
        waterWaveSlope(worldXZ, longA, 0.22, 0.92, 0.19, 0.0, t)
        + waterWaveSlope(worldXZ, longB, 0.36, 0.68, 0.10, 1.7, t)
        + waterWaveSlope(worldXZ, ripple, 0.88, 1.42, 0.025, 2.9, t);
    return normalize(vec3(-slope.x, 1.0, -slope.y));
}

// --- Face normals ---

const vec3 FACE_NORMALS[6] = vec3[6](
    vec3( 1, 0, 0),
    vec3(-1, 0, 0),
    vec3( 0, 1, 0),
    vec3( 0,-1, 0),
    vec3( 0, 0, 1),
    vec3( 0, 0,-1)
);

// --- Pattern generators ---

vec3 patternNoise(MaterialData mat, vec3 wp) {
    float scale = mat.noiseParams.x;
    float strength = mat.noiseParams.y;
    float n = valueNoise(wp * scale);
    return mix(mat.baseColor.rgb, mat.secondaryColor.rgb, n * strength);
}

vec3 patternOreSpots(MaterialData mat, vec3 wp) {
    float scale = mat.noiseParams.x;
    float n = valueNoise(wp * scale);
    float spots = smoothstep(0.55, 0.65, n);
    return mix(mat.baseColor.rgb, mat.secondaryColor.rgb, spots);
}

vec3 patternGrass(MaterialData mat, vec3 wp, uint faceIdx) {
    if (faceIdx == 2u) {
        float n = valueNoise(wp * 4.0) * 0.15;
        return mat.baseColor.rgb + vec3(-n, n * 0.5, -n);
    }
    if (faceIdx == 3u) {
        return mat.secondaryColor.rgb;
    }
    float edgeFactor = fract(wp.y);
    float greenEdge = smoothstep(0.7, 0.95, edgeFactor);
    vec3 dirtColor = mat.secondaryColor.rgb * (0.9 + valueNoise(wp * 3.0) * 0.1);
    return mix(dirtColor, mat.baseColor.rgb, greenEdge);
}

vec3 patternWoodGrain(MaterialData mat, vec3 wp, uint faceIdx) {
    vec2 crossSection = wp.xz;
    float rings = sin(length(crossSection - floor(crossSection) - 0.5) * 20.0) * 0.5 + 0.5;
    if (faceIdx == 2u || faceIdx == 3u) {
        return mix(mat.baseColor.rgb, mat.secondaryColor.rgb, rings * 0.4);
    }
    float bark = valueNoise(wp * vec3(1.0, 4.0, 1.0)) * 0.3;
    return mat.secondaryColor.rgb + vec3(bark * 0.2, bark * 0.1, 0.0);
}

vec3 patternEdgeHighlight(MaterialData mat, vec3 wp, uint faceIdx) {
    vec3 normal = FACE_NORMALS[faceIdx];
    vec3 localPos = fract(wp);

    float edgeDist = 1.0;
    if (abs(normal.x) > 0.5) {
        edgeDist = min(min(localPos.y, 1.0 - localPos.y), min(localPos.z, 1.0 - localPos.z));
    } else if (abs(normal.y) > 0.5) {
        edgeDist = min(min(localPos.x, 1.0 - localPos.x), min(localPos.z, 1.0 - localPos.z));
    } else {
        edgeDist = min(min(localPos.x, 1.0 - localPos.x), min(localPos.y, 1.0 - localPos.y));
    }

    float edgeWidth = mat.noiseParams.z;
    float edge = 1.0 - smoothstep(0.0, edgeWidth, edgeDist);
    float n = valueNoise(wp * mat.noiseParams.x) * mat.noiseParams.y * 0.3;
    return mix(mat.baseColor.rgb, mat.secondaryColor.rgb, edge * 0.5) + vec3(n);
}

// W1: dynamic water surface. Three pieces stacked:
//  1. Two scrolling noise layers at different scales (gives a "current"
//     direction without obvious tiling).
//  2. A coarse swell layer that breathes much slower for low-frequency motion.
//  3. Depth tint: water far below sea level gets a darker, more saturated
//     blue. Cheap because we already know worldPos.y.
// Side faces are dimmed slightly compared to the top — light bounces
// downward off the surface, so vertical water walls feel duller.
vec3 patternWater(MaterialData mat, vec3 wp, uint faceIdx) {
    float t = pc.lightParams.w;

    // Layer 1: fast lateral flow, small scale → "wavelets"
    vec2 flowA = wp.xz * 0.32 + vec2( t * 0.055, -t * 0.038);
    float rippleA = valueNoise(vec3(flowA, 0.0));
    // Layer 2: medium scale, perpendicular flow direction
    vec2 flowB = wp.xz * 0.12 + vec2(-t * 0.024,  t * 0.041);
    float rippleB = valueNoise(vec3(flowB.yx * 1.7 + vec2(13.2, 4.7), 1.0));
    // Layer 3: slow swell — large scale, very slow phase
    vec2 flowC = wp.xz * 0.04 + vec2( t * 0.012,  t * 0.008);
    float swell = valueNoise(vec3(flowC, 7.3));

    float ripple = rippleA * 0.50 + rippleB * 0.30 + swell * 0.20;

    // Face role: top is bright, sides are dim, bottom (rare) gets the
    // darkest tint.
    float top    = (faceIdx == 2u) ? 1.0 : 0.0;
    float side   = (faceIdx == 0u || faceIdx == 1u || faceIdx == 4u || faceIdx == 5u) ? 1.0 : 0.0;
    float bottom = (faceIdx == 3u) ? 1.0 : 0.0;

    // Depth tint: how far below sea level is THIS column? Use the chunk-
    // boundary world-Y so the tint progresses smoothly along the column,
    // not just at the visible water surface.
    float depthBelow = max(0.0, kWaterSeaLevel - wp.y);
    float depthT = clamp(depthBelow / kWaterDeepTintFalloff, 0.0, 1.0);
    float shallowT = 1.0 - depthT;
    vec3 baseTint = mix(kWaterShallowTint, kWaterDeepTint, depthT);

    // Mix the material's authored colors with the depth tint and ripple.
    // The bigger material-color gap (Fix #1) means the ripple is now
    // clearly visible — wavelets read as drifting bands of light blue
    // across the deep base color.
    vec3 color = mix(mat.baseColor.rgb, mat.secondaryColor.rgb, ripple);
    color = mix(color, baseTint, 0.35);
    float waveSignal = waterWaveSignal(wp.xz, t);
    float trough = smoothstep(0.08, 0.72, -waveSignal);
    color = mix(color, kWaterTroughTint, trough * (0.10 + depthT * 0.22));

    // Bright animated highlight patches on the top face. Uses smoothstep
    // around the ripple's peak so only the bright "crests" pick up the
    // highlight. Reads as moving sunlight on a moving surface — much more
    // convincing than Phong specular for stylized water.
    if (top > 0.5) {
        vec3 waveNormal = waterWaveNormal(wp.xz, t);
        float highlight = smoothstep(
            kWaterHighlightCutoff,
            kWaterHighlightCutoff + kWaterHighlightSoftness,
            ripple);
        color += kWaterHighlightColor * highlight * kWaterHighlightStrength;

        vec3 sunDir = normalize(pc.lightParams.xyz);
        vec3 viewDir = normalize(pc.cameraWorldParams.xyz - wp);
        vec3 reflectedView = reflect(-viewDir, waveNormal);
        float skyFacing = clamp(reflectedView.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 skyReflection = mix(kWaterHorizonReflection, kWaterSkyReflection, skyFacing);
        float waveShade = clamp(dot(waveNormal, normalize(vec3(0.18, 1.0, 0.10))) * 0.5 + 0.5, 0.0, 1.0);
        color *= mix(0.88, 1.08, waveShade);

        float sunGlint = pow(max(dot(reflect(-sunDir, waveNormal), viewDir), 0.0), 48.0);
        color += vec3(1.0, 0.84, 0.56) * sunGlint * (0.32 + shallowT * 0.18);

        float fresnel = pow(1.0 - max(dot(viewDir, waveNormal), 0.0), 4.0);
        color = mix(color, skyReflection, fresnel * (0.18 + shallowT * 0.10));

        vec2 causticUv = wp.xz * 0.85 + vec2(t * 0.08, -t * 0.05);
        float caustic = valueNoise(vec3(causticUv, 4.2));
        caustic = smoothstep(0.58, 0.86, caustic) * shallowT;
        color = mix(color, kWaterShoreTint, shallowT * 0.28);
        color += kWaterShoreTint * caustic * 0.18;

        float foam = smoothstep(0.72, 0.92, rippleB + swell * 0.35) * shallowT;
        color = mix(color, kWaterFoamColor, foam * 0.15);
    }

    color *= mix(1.0, 0.74, side);
    color *= mix(1.0, 0.55, bottom);
    color *= mix(0.78, 0.90, shallowT);
    return color;
}

vec3 evaluatePattern(MaterialData mat, vec3 wp, uint faceIdx) {
    uint patternType = uint(mat.noiseParams.w);
    if (patternType == 1u) return patternOreSpots(mat, wp);
    if (patternType == 2u) return patternGrass(mat, wp, faceIdx);
    if (patternType == 3u) return patternWoodGrain(mat, wp, faceIdx);
    if (patternType == 4u) return patternEdgeHighlight(mat, wp, faceIdx);
    if (patternType == 5u) return patternWater(mat, wp, faceIdx);
    return patternNoise(mat, wp);
}

float faceAmbientFactor(vec3 normal) {
    if (normal.y > 0.5) return 1.0;
    if (normal.y < -0.5) return 0.55;
    return 0.78;
}

vec3 evaluateLighting(vec3 normal, uint packedLight, float emission) {
    vec3 sunDir = normalize(pc.lightParams.xyz);
    float viewDistance = length(fragWorldPos - pc.cameraWorldParams.xyz);
    float farT = smoothstep(220.0, max(230.0, pc.atmosphereParams.y * 0.78), viewDistance);

    // Shader-only lighting path. CPU propagation is no longer required for
    // ordinary chunk visibility; the mesher may still pack a cheap face shade
    // in the low byte, which we treat as material/face AO.
    float faceShade = clamp(float(packedLight & 255u) / 255.0, 0.42, 1.0);
    faceShade = mix(faceShade, max(faceShade, 0.74), farT * pc.atmosphereParams.w);

    float sunFacing = max(dot(normal, sunDir), 0.0);
    float wrappedSun = clamp((dot(normal, sunDir) + 0.32) / 1.32, 0.0, 1.0);
    float skyDiffuse = (0.28 + sunFacing * 0.58 + wrappedSun * 0.26) * faceAmbientFactor(normal);
    float upward = normal.y * 0.5 + 0.5;
    float heightFogLift = clamp((fragWorldPos.y + 32.0) / 128.0, 0.0, 1.0);
    vec3 ambient = mix(vec3(0.13, 0.14, 0.16), vec3(0.22, 0.24, 0.28), heightFogLift);
    ambient += kGroundBounceColor * (1.0 - upward) * 0.18;
    vec3 skyLight = kSkyLightColor * skyDiffuse;
    vec3 sunLight = kSunLightColor * wrappedSun * 0.72;
    vec3 blockGlow = vec3(1.0, 0.74, 0.46) * emission * 1.35;

    vec3 lighting = (ambient + skyLight + sunLight) * faceShade + blockGlow;
    lighting *= mix(1.0, 1.0 + pc.atmosphereParams.w * 0.22, farT);
    return max(lighting, vec3(emission));
}

vec3 applyAerialPerspective(vec3 color) {
    vec3 toFragment = fragWorldPos - pc.cameraWorldParams.xyz;
    float viewDistance = length(toFragment);
    float fogNear = max(1.0, pc.atmosphereParams.x);
    float fogFar = max(fogNear + 1.0, pc.atmosphereParams.y);
    float distanceFog = smoothstep(fogNear, fogFar, viewDistance);

    float verticalDelta = abs(toFragment.y);
    float horizonT = 1.0 - clamp(verticalDelta / max(256.0, fogFar * 0.22), 0.0, 1.0);
    float lowMist = 1.0 - smoothstep(-36.0, 180.0, fragWorldPos.y);
    float fogAmount = distanceFog * pc.atmosphereParams.z * mix(0.58, 1.18, horizonT);
    fogAmount += lowMist * distanceFog * 0.14;

    vec3 viewDir = normalize(toFragment + vec3(0.0, 0.0001, 0.0));
    vec3 sunDir = normalize(pc.lightParams.xyz);
    float sunForward = pow(max(dot(viewDir, sunDir), 0.0), 9.0);
    vec3 fogColor = mix(kZenithFogColor, kHorizonFogColor, horizonT);
    fogColor = mix(fogColor, kSunHazeColor, sunForward * 0.58);

    return mix(color, fogColor, clamp(fogAmount, 0.0, 0.72));
}

vec3 toneMap(vec3 color) {
    color *= 1.08;
    return vec3(1.0) - exp(-max(color, vec3(0.0)));
}

vec3 colorGrade(vec3 color) {
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float lift = smoothstep(0.08, 0.62, luma);
    color = mix(vec3(luma), color, 1.12 + lift * 0.10);
    color = max(color - vec3(0.012), vec3(0.0)) * 1.08;
    color += vec3(0.014, 0.010, 0.004);
    return color;
}

// --- Main ---

void main() {
    MaterialData mat = matTable.materials[fragMaterialTypeId];

    vec3 albedo;
    if (mat.textureParams.x > 0.0) {
        albedo = mat.baseColor.rgb;
    } else {
        albedo = evaluatePattern(mat, fragWorldPos, fragFaceIndex);
    }

    float emission = mat.textureParams.y;
    vec3 lighting = evaluateLighting(FACE_NORMALS[fragFaceIndex], fragPackedLight, emission);
    // Water uses pre-lit color (the pattern bakes in the depth tint and
    // animated highlight). Multiplying it by `lighting` would crush the
    // bright highlight to invisibility — keep the sun-affected lighting for
    // non-water blocks but pass water through with only a gentle modulation.
    vec3 color = albedo * lighting;
    if (uint(mat.noiseParams.w) == 5u) {
        // Light water at ~70% of full lighting so it picks up day/night
        // changes but doesn't lose the highlights to multiplication.
        color = albedo * mix(vec3(1.0), lighting, 0.55);
    }

    // W0: underwater fog. When camera is inside water (cameraParams.x > 0),
    // tint everything with deep blue-green and fade with view distance.
    // Use linear depth (1/w on gl_FragCoord encodes view-space distance for
    // perspective projections).
    if (pc.cameraParams.x > 0.0) {
        float linearDepth = 1.0 / max(gl_FragCoord.w, 1e-6);
        // Fog density: 0 at the camera, ~1 at 24 blocks of water.
        float fog = 1.0 - exp(-linearDepth * 0.05);
        fog *= pc.cameraParams.x;  // overall strength
        color = mix(color, pc.cameraParams.yzw, clamp(fog, 0.0, 0.85));
        // Slight overall dim regardless of distance — the surface above
        // attenuates ambient light.
        color *= mix(1.0, 0.55, pc.cameraParams.x);
    }

    if (pc.cameraParams.x <= 0.0) {
        color = applyAerialPerspective(color);
    }
    color = colorGrade(color);
    color = toneMap(color);

    float alpha = mat.baseColor.a;
    if (uint(mat.noiseParams.w) == 5u) {
        alpha = fragFaceIndex == 2u ? alpha : alpha * 0.82;
    }
    outColor = vec4(color, alpha);
}

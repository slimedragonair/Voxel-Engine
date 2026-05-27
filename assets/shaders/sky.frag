#version 450

layout(location = 0) in vec2 fragUv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform SkyPushConstants {
    vec4 cameraForward;
    vec4 cameraRight;
    vec4 cameraUpAspect; // xyz = camera up, w = aspect
    vec4 sunDirTime;     // xyz = sun direction, w = time
    vec4 skyParams;      // horizon lift, saturation, cloud strength, brightness
} pc;

float hash2(vec2 p) {
    p = fract(p * vec2(443.8975, 397.2973));
    p += dot(p, p.yx + 19.19);
    return fract(p.x * p.y);
}

float noise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float a = hash2(i);
    float b = hash2(i + vec2(1.0, 0.0));
    float c = hash2(i + vec2(0.0, 1.0));
    float d = hash2(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p) {
    float total = 0.0;
    float amp = 0.55;
    for (int i = 0; i < 4; ++i) {
        total += noise2(p) * amp;
        p = p * 2.03 + vec2(17.7, 5.1);
        amp *= 0.52;
    }
    return total;
}

void main() {
    const float tanHalfFov = 0.70;
    float horizonLift = pc.skyParams.x;
    float skyY = -fragUv.y + horizonLift;
    vec3 ray = normalize(
        pc.cameraForward.xyz
        + pc.cameraRight.xyz * fragUv.x * pc.cameraUpAspect.w * tanHalfFov
        + pc.cameraUpAspect.xyz * skyY * tanHalfFov);
    vec3 sunDir = normalize(pc.sunDirTime.xyz);

    float skyHemisphere = smoothstep(0.0, 0.10, ray.y);
    float up = clamp(ray.y, 0.0, 1.0);
    float horizon = exp(-abs(ray.y) * 8.0);
    vec3 zenith = vec3(0.16, 0.43, 0.90);
    vec3 midSky = vec3(0.42, 0.68, 1.00);
    vec3 horizonColor = vec3(0.76, 0.88, 1.00);
    vec3 groundMist = vec3(0.54, 0.68, 0.86);
    vec3 sky = mix(horizonColor, mix(midSky, zenith, smoothstep(0.18, 1.0, up)), smoothstep(0.0, 0.72, up));
    sky = mix(groundMist, sky, skyHemisphere);

    float sunDot = max(dot(ray, sunDir), 0.0);
    float sunCore = pow(sunDot, 620.0) * skyHemisphere;
    float sunHalo = pow(sunDot, 18.0) * skyHemisphere;
    float warmHorizon = pow(max(dot(normalize(vec3(ray.x, 0.16, ray.z)), sunDir), 0.0), 5.0);
    sky += vec3(1.0, 0.72, 0.40) * sunHalo * 0.55;
    sky += vec3(1.0, 0.80, 0.48) * warmHorizon * horizon * skyHemisphere * 0.22;
    sky += vec3(1.0, 0.92, 0.72) * sunCore * 2.8;

    float cloudMask = smoothstep(0.10, 0.64, ray.y) * (1.0 - smoothstep(0.88, 1.0, ray.y));
    vec2 wind = vec2(0.0);
    vec2 cloudUv = ray.xz / max(ray.y + 0.42, 0.18);
    float cloud = fbm(cloudUv * 1.35 + wind);
    float cloudDetail = fbm(cloudUv * 4.4 - wind * 2.1);
    cloud = smoothstep(0.52, 0.74, cloud * 0.78 + cloudDetail * 0.22);
    cloud *= cloudMask;
    vec3 cloudColor = mix(vec3(0.86, 0.88, 0.90), vec3(1.0, 0.96, 0.88), clamp(sunDot * 2.0, 0.0, 1.0));
    sky = mix(sky, cloudColor, cloud * pc.skyParams.z);

    float vignette = smoothstep(1.75, 0.15, dot(vec2(fragUv.x, skyY), vec2(fragUv.x, skyY)));
    sky *= mix(0.90, 1.0, vignette);
    float luma = dot(sky, vec3(0.2126, 0.7152, 0.0722));
    sky = mix(vec3(luma), sky, pc.skyParams.y) * pc.skyParams.w;

    outColor = vec4(vec3(1.0) - exp(-max(sky, vec3(0.0))), 1.0);
}

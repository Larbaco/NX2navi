#version 450
// build_shaders: keep functions
//
// Copyright (c) 2025 Carlos Lopez Jr.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// The HDR tonemap, as a function that runtime/vk_hdr.c (vk_hdr_shader_swap) splices into the game's own tonemap
// shader: nx_tonemap takes the three values the game feeds its fitted ACES curve (its scene light after its colour
// matrix, bloom and exposure) and returns the colour the shader then stores, linear Rec. 709 relative to the HUD's
// white, which the present encodes to PQ (shaders/hdr_encode.comp). main only exists so the compiler keeps the
// functions; vk_hdr.c drops it. build_shaders.py compiles this file without optimisation ("keep functions" above),
// as inlining would leave no function to splice.
//
// The picture: the game's SDR result (its curve, clamped) is the colour grade; the tone mapper upgrades it to the
// HDR range by luminance against a neutral SDR curve of the same input, then maps that onto the display: a
// luminance curve towards the peak with exposure, highlights, shadows, contrast and flare, and in OkLab the hue of
// the input, saturation, highlight saturation and blowout. Settings (vk_hdr.c vk_hdr_support, support buffer bytes
// 1440..1503):
//   p0: peak nits, game nits (white of the picture), HUD nits, tone mapper (-1: the game's own SDR picture, as when the
//       swapchain is not HDR10; 0: the game's curve; 3: the HDR tone mapper)
//   p1: exposure, highlights, shadows, contrast
//   p2: saturation, highlight saturation, blowout, flare (parsed: 1 = unchanged for the first two)
//   p3: hue correction, grade strength, gamma correction (0 off, 1: 2.2, 2: 2.4; applied on the present), unused
layout(location = 0) in vec4 a0;
layout(location = 1) in vec4 a1;
layout(location = 2) in vec4 a2;
layout(location = 3) in vec4 a3;
layout(location = 4) in vec4 a4;
layout(location = 0) out vec4 o;

// Matrices are written row by row; mulm(m, v) is the row-vector product of that layout.
vec3 mulm(mat3 m, vec3 v) { return v * m; }

const mat3 BT709_TO_AP1 = mat3(0.6130974024, 0.3395231462, 0.0473794514,
                               0.0701937225, 0.9163538791, 0.0134523985,
                               0.0206155929, 0.1095697729, 0.8698146342);
const mat3 AP1_TO_BT709 = mat3(1.7050509927, -0.6217921207, -0.0832588720,
                               -0.1302564175, 1.1408047366, -0.0105483191,
                               -0.0240033568, -0.1289689761, 1.1529723329);
const mat3 BT709_TO_OKLABLMS = mat3(0.4122214708, 0.5363325363, 0.0514459929,
                                    0.2119034982, 0.6806995451, 0.1073969566,
                                    0.0883024619, 0.2817188376, 0.6299787005);
const mat3 OKLABLMS_TO_OKLAB = mat3(0.2104542553, 0.7936177850, -0.0040720468,
                                    1.9779984951, -2.4285922050, 0.4505937099,
                                    0.0259040371, 0.7827717662, -0.8086757660);
const mat3 OKLAB_TO_OKLABLMS = mat3(1.0, 0.3963377774, 0.2158037573,
                                    1.0, -0.1055613458, -0.0638541728,
                                    1.0, -0.0894841775, -1.2914855480);
const mat3 OKLABLMS_TO_BT709 = mat3(4.0767416621, -3.3077115913, 0.2309699292,
                                    -1.2684380046, 2.6097574011, -0.3413193965,
                                    -0.0041960863, -0.7034186147, 1.7076147010);

float y709(vec3 c) { return dot(c, vec3(0.2126390059, 0.7151686788, 0.0721923154)); }

float divide_safe(float a, float b, float fallback) { return b == 0.0 ? fallback : a / b; }

vec3 copy_sign(vec3 mag, vec3 s) {
    vec3 a = abs(mag);
    return mix(a, -a, lessThan(s, vec3(0.0)));
}

vec3 cbrt(vec3 x) { return copy_sign(pow(abs(x), vec3(1.0 / 3.0)), x); }

vec3 oklab_from_bt709(vec3 c) { return mulm(OKLABLMS_TO_OKLAB, cbrt(mulm(BT709_TO_OKLABLMS, c))); }

vec3 bt709_from_oklab(vec3 lab) {
    vec3 lms = mulm(OKLAB_TO_OKLABLMS, lab);
    return mulm(OKLABLMS_TO_BT709, lms * lms * lms);
}

vec3 clamp_ap1(vec3 c) { return mulm(AP1_TO_BT709, max(mulm(BT709_TO_AP1, c), vec3(0.0))); }

// The hue (OkLab a, b direction) of correct with the chrominance of incorrect.
vec3 hue_oklab(vec3 incorrect, vec3 correct) {
    vec3 il = oklab_from_bt709(incorrect), cl = oklab_from_bt709(correct);
    float pre = length(il.yz);
    vec2 ab = cl.yz;
    ab *= divide_safe(pre, length(ab), 1.0);
    il.yz = ab;
    return clamp_ap1(bt709_from_oklab(il));
}

float srgb_decode(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

// pow_to_srgb: a value of a gamma-g picture as the sRGB value of the same code (the peak's correction).
float gamma_to_srgb(float c, float g) { return srgb_decode(pow(c, 1.0 / g)); }

float smoother(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

float highlights(float x, float h, float mid) {
    if (h == 1.0) return x;
    float t = 0.0;
    if (x > mid) t = clamp(log2(x / mid) / log2(1.0 / mid), 0.0, 1.0);
    t = smoother(t);
    if (h > 1.0) return mix(x, mid * pow(x / mid, h), t);
    float b = mid * pow(x / mid, 2.0 - h);
    return divide_safe(x * x, mix(x, b, t), x);
}

float shadows(float x, float s, float mid) {
    if (s == 1.0) return x;
    float ratio = max(divide_safe(x, mid, 0.0), 0.0);
    float base_term = x * mid;
    float base_scale = divide_safe(base_term, ratio, 0.0);
    float shadow_floor = mid / 16.0;
    float t = 1.0;
    if (x > shadow_floor) t = clamp(log2(x / mid) / log2(shadow_floor / mid), 0.0, 1.0);
    t = smoother(t);
    if (s > 1.0) {
        float raised = x * (1.0 + divide_safe(base_term, pow(ratio, s), 0.0));
        return x + (raised - x * (1.0 + base_scale)) * t;
    }
    float lowered = x * (1.0 - divide_safe(base_term, pow(ratio, 2.0 - s), 0.0));
    return x + (lowered - x * (1.0 - base_scale)) * t;
}

// The luminance curve: n peak nits, g contrast, c_d mid grey in nits, t_1 flare; 100 nits reference, scene grey 0.18.
float curve(float x, float n, float g, float c_d, float t_1) {
    const float n_r = 100.0, c = 0.18, w_g = 0.0, r_hit_min = 128.0, r_hit_max = 256.0;
    float m_0 = n / n_r;
    float m_1 = 0.5 * (m_0 + sqrt(m_0 * (m_0 + 4.0 * t_1)));
    float r_hit = r_hit_min + (r_hit_max - r_hit_min) * (log(m_0) / log(10000.0 / 100.0));
    float u = pow((r_hit / m_1) / ((r_hit / m_1) + 1.0), g);
    float m = m_1 / u;
    float w_i = log(n / 100.0) / log(2.0);
    float c_t = (c_d / n_r) * (1.0 + w_i * w_g);
    float g_ip = 0.5 * (c_t + sqrt(c_t * (c_t + 4.0 * t_1)));
    float g_ipp2 = -m_1 * pow(g_ip / m, 1.0 / g) / (pow(g_ip / m, 1.0 / g) - 1.0);
    float w_2 = c / g_ipp2;
    float s_2 = w_2 * m_1;
    float u_2 = pow((r_hit / m_1) / ((r_hit / m_1) + w_2), g);
    float m_2 = m_1 / u_2;
    float ts = pow(max(0.0, x) / (x + s_2), g) * m_2;
    return max(0.0, (ts * ts) / (ts + t_1));
}

struct Drt {
    float nits_peak, exposure, highlights, shadows, contrast, saturation, dechroma, flare, hue, blowout;
    float mid_gray_nits;
    bool clamp_zero;   // clamp to Rec. 709 (no negative channel) before the peak
};

// The tone mapper by luminance, then hue, saturation and blowout in OkLab; clamped to the peak in Rec. 709.
vec3 drt(vec3 bt709, Drt d) {
    const float mid = 0.18;
    float peak = d.nits_peak / 100.0;
    float y_original = y709(bt709);
    float y = y_original * d.exposure;
    y = highlights(y, d.highlights, mid);
    y = shadows(y, d.shadows, mid);
    float y_new = clamp(curve(y, d.nits_peak, d.contrast, d.mid_gray_nits, d.flare), 0.0, peak);
    vec3 color = bt709 * (y_original > 0.0 ? y_new / y_original : 0.0);
    if (d.dechroma != 0.0 || d.saturation != 1.0 || d.hue != 0.0 || d.blowout != 0.0) {
        vec3 lab = oklab_from_bt709(color);
        if (d.hue != 0.0) {
            vec3 old_lab = oklab_from_bt709(bt709);
            float pre = length(lab.yz);
            lab.yz = mix(lab.yz, old_lab.yz, d.hue);
            lab.yz *= divide_safe(pre, length(lab.yz), 1.0);
        }
        if (d.dechroma != 0.0) lab.yz *= mix(1.0, 0.0, clamp(pow(y_original / 100.0, 1.0 - d.dechroma), 0.0, 1.0));
        if (d.blowout != 0.0) {
            float percent_max = clamp(y_original * 100.0 / 10000.0, 0.0, 1.0);
            float change = pow(1.0 - percent_max, 100.0 * abs(d.blowout));
            if (d.blowout < 0.0) change = 2.0 - change;
            lab.yz *= change;
        }
        lab.yz *= d.saturation;
        color = bt709_from_oklab(lab);
    }
    if (d.clamp_zero) color = max(color, vec3(0.0));
    return min(color, vec3(peak));
}

// The same curve at SDR range with nothing changed: what the SDR grade is measured against.
vec3 neutral_sdr(vec3 c) {
    Drt d = Drt(100.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 18.0, true);
    return drt(c, d);
}

// The graded SDR colour carried to the HDR range: its luminance gains what the untonemapped colour has above the
// neutral SDR curve, in the graded colour's hue.
vec3 upgrade(vec3 untonemapped, vec3 neutral, vec3 graded, float strength) {
    float yu = y709(untonemapped), yn = y709(neutral), yg = y709(graded);
    float ratio;
    if (yu < yn) {
        ratio = yu / yn;
    } else {
        float y_new = yg + max(0.0, yu - yn);
        ratio = yg > 0.0 ? y_new / yg : 0.0;
    }
    vec3 scaled = hue_oklab(graded * ratio, graded);
    return mix(untonemapped, scaled, strength);
}

vec3 aces_fitted(vec3 x) {   // the game's curve and clamp
    return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 nx_tonemap(vec3 x, vec4 p0, vec4 p1, vec4 p2, vec4 p3) {
    vec3 vanilla = aces_fitted(x);
    if (p0.w < 0.0) return vanilla;
    vec3 color = vanilla;
    if (p0.w > 0.0) {
        vec3 graded = p3.y == 0.0 ? x : upgrade(x, neutral_sdr(x), vanilla, p3.y);
        float peak = p0.x / p0.y;   // the display's peak in picture whites; corrected for the present's gamma
        if (p3.z != 0.0) peak = gamma_to_srgb(peak, p3.z == 1.0 ? 2.2 : 2.4);
        Drt d = Drt(peak * 100.0, p1.x, p1.y, p1.z, p1.w, p2.x, p2.z, 0.10 * pow(p2.w, 10.0), p3.x, -(p2.y - 1.0),
                    18.0, false);
        color = drt(graded, d);
    }
    color *= p0.y / p0.z;   // the picture's white against the HUD's
    return mix(color, vec3(0.0), isnan(color));
}

void main() { o = vec4(nx_tonemap(a0.xyz, a1, a2, a3, a4), 1.0); }

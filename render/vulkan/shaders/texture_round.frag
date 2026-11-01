#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

// struct wlr_vk_frag_texture_pcr_data
layout(push_constant, row_major) uniform UBO {
	layout(offset = 80) mat4 matrix;
	float alpha;
	float luminance_multiplier;
} data;

// struct wlr_vk_round_params: set 1 of round pipelines, bound with a
// dynamic offset into the shared wlr_vk_renderer.round_ubo ring
layout(set = 1, binding = 0) uniform RoundParams {
	vec2 origin;
	vec2 size;
	vec4 radii;
	vec4 cutout_box;
	vec4 cutout_radii;
	vec4 clip_boxes[4];
	vec4 clip_radii[4];
	int clip_count;
} round_ubo;

layout (constant_id = 0) const int TEXTURE_TRANSFORM = 0;

// Matches enum wlr_vk_texture_transform
#define TEXTURE_TRANSFORM_IDENTITY 0
#define TEXTURE_TRANSFORM_SRGB 1
#define TEXTURE_TRANSFORM_ST2084_PQ 2
#define TEXTURE_TRANSFORM_GAMMA22 3
#define TEXTURE_TRANSFORM_BT1886 4

float srgb_channel_to_linear(float x) {
	return mix(x / 12.92,
		pow((x + 0.055) / 1.055, 2.4),
		x > 0.04045);
}

vec3 srgb_color_to_linear(vec3 color) {
	return vec3(
		srgb_channel_to_linear(color.r),
		srgb_channel_to_linear(color.g),
		srgb_channel_to_linear(color.b)
	);
}

vec3 pq_color_to_linear(vec3 color) {
	float inv_m1 = 1 / 0.1593017578125;
	float inv_m2 = 1 / 78.84375;
	float c1 = 0.8359375;
	float c2 = 18.8515625;
	float c3 = 18.6875;
	vec3 num = max(pow(color, vec3(inv_m2)) - c1, 0);
	vec3 denom = c2 - c3 * pow(color, vec3(inv_m2));
	return pow(num / denom, vec3(inv_m1));
}

vec3 bt1886_color_to_linear(vec3 color) {
	float Lmin = 0.01;
	float Lmax = 100.0;
	float lb = pow(Lmin, 1.0 / 2.4);
	float lw = pow(Lmax, 1.0 / 2.4);
	float a  = pow(lw - lb, 2.4);
	float b  = lb / (lw - lb);
	vec3 L = a * pow(color + vec3(b), vec3(2.4));
	return (L - Lmin) / (Lmax - Lmin);
}

// Radii (top-left, top-right, bottom-right, bottom-left) that fit a box of
// this size, by the rule CSS border-radius uses: where the two radii on one
// edge add up to more than the edge, all four shrink by the smallest such
// ratio.
vec4 fit_radii(vec2 box, vec4 r) {
	float f = 1.0;
	if (r.x + r.y > box.x) f = min(f, box.x / (r.x + r.y));
	if (r.w + r.z > box.x) f = min(f, box.x / (r.w + r.z));
	if (r.x + r.w > box.y) f = min(f, box.y / (r.x + r.w));
	if (r.y + r.z > box.y) f = min(f, box.y / (r.y + r.z));
	return r * f;
}

// One corner's coverage, d the point's offset from the arc centre pointing
// out of the box: a one pixel ramp on the arc.
float arc_alpha(vec2 d, float r) {
	return r > 0.0 ? 1.0 - smoothstep(-0.5, 0.5, length(max(d, 0.0)) - r) : 1.0;
}

// Coverage of a point p, in pixels from the box origin, in a box of the
// given size cut by four radii: 1 inside, 0 outside, a one pixel ramp on
// the edges and arcs. Each corner cuts only its own square.
float corner_alpha(vec2 p, vec2 box, vec4 r) {
	r = fit_radii(box, r);
	vec2 inside = min(p, box - p);
	float alpha = smoothstep(-0.5, 0.5, min(inside.x, inside.y));
	alpha *= arc_alpha(vec2(r.x - p.x, r.x - p.y), r.x);
	alpha *= arc_alpha(vec2(p.x - box.x + r.y, r.y - p.y), r.y);
	alpha *= arc_alpha(vec2(p.x - box.x + r.z, p.y - box.y + r.z), r.z);
	alpha *= arc_alpha(vec2(r.w - p.x, p.y - box.y + r.w), r.w);
	return alpha;
}

// The box coverage times every rounded clip's, clip boxes relative to the
// box origin.
float round_coverage(vec2 p) {
	float coverage = corner_alpha(p, round_ubo.size, round_ubo.radii);
	for (int i = 0; i < 4; i++) {
		if (i >= round_ubo.clip_count) {
			break;
		}
		coverage *= corner_alpha(p - round_ubo.clip_boxes[i].xy,
			round_ubo.clip_boxes[i].zw, round_ubo.clip_radii[i]);
	}
	return coverage;
}

void main() {
	vec4 in_color = textureLod(tex, uv, 0);

	// Convert from pre-multiplied alpha to straight alpha
	float alpha = in_color.a;
	vec3 rgb;
	if (alpha == 0) {
		rgb = vec3(0);
	} else {
		rgb = in_color.rgb / alpha;
	}

	if (TEXTURE_TRANSFORM != TEXTURE_TRANSFORM_IDENTITY) {
		rgb = max(rgb, vec3(0));
	}
	if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_SRGB) {
		rgb = srgb_color_to_linear(rgb);
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_ST2084_PQ) {
		rgb = pq_color_to_linear(rgb);
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_GAMMA22) {
		rgb = pow(rgb, vec3(2.2));
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_BT1886) {
		rgb = bt1886_color_to_linear(rgb);
	}

	rgb *= data.luminance_multiplier;

	rgb = mat3(data.matrix) * rgb;

	// Back to pre-multiplied alpha
	out_color = vec4(rgb * alpha, alpha);

	out_color *= data.alpha;

	// A cut corner is transparent whatever the texture is
	out_color *= round_coverage(gl_FragCoord.xy - round_ubo.origin);
}

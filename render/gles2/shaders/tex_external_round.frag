#extension GL_OES_EGL_image_external : require

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
varying vec2 v_pos;
uniform float alpha;
uniform samplerExternalOES texture0;
uniform vec2 size;
uniform vec4 radii;
uniform int clip_count;
uniform vec4 clip_boxes[4];
uniform vec4 clip_radii[4];

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

// Coverage of a point p, in pixels from the box origin, in a box of the given
// size cut by four radii: 1 inside, 0 outside, a one pixel ramp on the edges
// and arcs. Each corner cuts only its own square.
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
	float coverage = corner_alpha(p, size, radii);
	for (int i = 0; i < 4; i++) {
		if (i >= clip_count) {
			break;
		}
		coverage *= corner_alpha(p - clip_boxes[i].xy, clip_boxes[i].zw, clip_radii[i]);
	}
	return coverage;
}

void main() {
	gl_FragColor = texture2D(texture0, v_texcoord) * alpha * round_coverage(v_pos * size);
}

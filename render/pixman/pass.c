#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "render/pixman.h"

static const struct wlr_render_pass_impl render_pass_impl;

static struct wlr_pixman_render_pass *get_render_pass(struct wlr_render_pass *wlr_pass) {
	assert(wlr_pass->impl == &render_pass_impl);
	struct wlr_pixman_render_pass *pass = wl_container_of(wlr_pass, pass, base);
	return pass;
}

static struct wlr_pixman_texture *get_texture(struct wlr_texture *wlr_texture) {
	assert(wlr_texture_is_pixman(wlr_texture));
	struct wlr_pixman_texture *texture = wl_container_of(wlr_texture, texture, wlr_texture);
	return texture;
}

static bool render_pass_submit(struct wlr_render_pass *wlr_pass) {
	struct wlr_pixman_render_pass *pass = get_render_pass(wlr_pass);

	wlr_buffer_end_data_ptr_access(pass->buffer->buffer);
	wlr_buffer_unlock(pass->buffer->buffer);
	free(pass);

	return true;
}

static pixman_op_t get_pixman_blending(enum wlr_render_blend_mode mode) {
	switch (mode) {
	case WLR_RENDER_BLEND_MODE_PREMULTIPLIED:
		return PIXMAN_OP_OVER;
	case WLR_RENDER_BLEND_MODE_NONE:
		return PIXMAN_OP_SRC;
	}
	abort();
}

// A corner's coverage tile: a radius by radius A8 square whose arc centre is
// the square's inner corner, with the one pixel ramp of the GLES2
// corner_alpha. Cached on the renderer per radius and corner.
static pixman_image_t *corner_tile(struct wlr_pixman_renderer *renderer,
		int radius, int corner) {
	struct wlr_pixman_corner_tiles *tiles = NULL, *entry;
	wl_list_for_each(entry, &renderer->corner_tiles, link) {
		if (entry->radius == radius) {
			tiles = entry;
			break;
		}
	}
	if (tiles == NULL) {
		tiles = calloc(1, sizeof(*tiles));
		if (tiles == NULL) {
			return NULL;
		}
		tiles->radius = radius;
		wl_list_insert(&renderer->corner_tiles, &tiles->link);
	}
	if (tiles->images[corner] != NULL) {
		return tiles->images[corner];
	}

	pixman_image_t *image = pixman_image_create_bits(PIXMAN_a8, radius, radius, NULL, 0);
	if (image == NULL) {
		return NULL;
	}
	uint8_t *data = (uint8_t *)pixman_image_get_data(image);
	int stride = pixman_image_get_stride(image);
	double cx = corner == 0 || corner == 3 ? radius : 0;
	double cy = corner == 0 || corner == 1 ? radius : 0;
	for (int y = 0; y < radius; y++) {
		for (int x = 0; x < radius; x++) {
			double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
			double dist = sqrt(dx * dx + dy * dy) - radius;
			// 1 - smoothstep(-0.5, 0.5, dist)
			double t = dist + 0.5;
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			double coverage = 1.0 - t * t * (3.0 - 2.0 * t);
			data[y * stride + x] = (uint8_t)(coverage * 255.0 + 0.5);
		}
	}
	tiles->images[corner] = image;
	return image;
}

// Multiply each nonzero corner's tile into the mask at the corners of the
// box at (x, y) of the given size, the radii fitted to the box.
static bool mask_cut_corners(struct wlr_pixman_renderer *renderer,
		pixman_image_t *mask, int x, int y, int width, int height,
		const float radii[static 4]) {
	float fit[4];
	wlr_render_fit_corner_radii(fit, radii, width, height);
	for (int i = 0; i < 4; i++) {
		int r = roundf(fit[i]);
		if (r <= 0) {
			continue;
		}
		pixman_image_t *tile = corner_tile(renderer, r, i);
		if (tile == NULL) {
			return false;
		}
		pixman_image_composite32(PIXMAN_OP_IN, tile, NULL, mask, 0, 0, 0, 0,
			i == 0 || i == 3 ? x : x + width - r,
			i == 0 || i == 1 ? y : y + height - r, r, r);
	}
	return true;
}

// An opaque A8 mask of the given size with its corners cut.
static pixman_image_t *box_mask(struct wlr_pixman_renderer *renderer,
		int width, int height, const float radii[static 4]) {
	pixman_image_t *mask = pixman_image_create_bits(PIXMAN_a8, width, height, NULL, 0);
	if (mask == NULL) {
		return NULL;
	}
	pixman_image_fill_rectangles(PIXMAN_OP_SRC, mask,
		&(pixman_color_t){ .alpha = 0xFFFF }, 1,
		&(pixman_rectangle16_t){ 0, 0, width, height });
	if (!mask_cut_corners(renderer, mask, 0, 0, width, height, radii)) {
		pixman_image_unref(mask);
		return NULL;
	}
	return mask;
}

// What a mask is built from, relative to the box, zeroed so it compares
// bytewise
static void rounded_mask_key(struct wlr_pixman_rounded_mask_key *key,
		const struct wlr_box *box, const float radii[static 4],
		const struct wlr_box *cutout, const float *cutout_radii,
		const struct wlr_render_rounded_clip *clips, size_t clips_len, float alpha) {
	memset(key, 0, sizeof(*key));
	key->width = box->width;
	key->height = box->height;
	memcpy(key->radii, radii, sizeof(key->radii));
	if (cutout != NULL && cutout->width > 0) {
		key->cutout = (struct wlr_box){
			.x = cutout->x - box->x,
			.y = cutout->y - box->y,
			.width = cutout->width,
			.height = cutout->height,
		};
		memcpy(key->cutout_radii, cutout_radii, sizeof(key->cutout_radii));
	}
	for (size_t i = 0; i < clips_len; i++) {
		key->clips[i].box = (struct wlr_box){
			.x = clips[i].box.x - box->x,
			.y = clips[i].box.y - box->y,
			.width = clips[i].box.width,
			.height = clips[i].box.height,
		};
		memcpy(key->clips[i].radii, clips[i].radii, sizeof(key->clips[i].radii));
	}
	key->clips_len = clips_len;
	key->alpha = alpha;
}

// An A8 mask of the key's size: opaque, the corners cut, less the cutout
// (width 0 for none), zero outside every rounded clip, times alpha when it is
// not 1. The cutout is a rounded box mask taken out of it, so its corners are
// exact wherever they overlap.
static pixman_image_t *build_rounded_mask(struct wlr_pixman_renderer *renderer,
		const struct wlr_pixman_rounded_mask_key *key) {
	int width = key->width, height = key->height;
	pixman_image_t *mask = box_mask(renderer, width, height, key->radii);
	if (mask == NULL) {
		return NULL;
	}
	bool ok = true;

	const struct wlr_box *cutout = &key->cutout;
	if (cutout->width > 0) {
		pixman_image_t *hole = box_mask(renderer, cutout->width, cutout->height,
			key->cutout_radii);
		ok = hole != NULL;
		if (ok) {
			pixman_image_composite32(PIXMAN_OP_OUT_REVERSE, hole, NULL, mask,
				0, 0, 0, 0, cutout->x, cutout->y, cutout->width, cutout->height);
			pixman_image_unref(hole);
		}
	}

	for (size_t i = 0; ok && i < key->clips_len; i++) {
		struct wlr_box clip = key->clips[i].box;
		pixman_region32_t outside;
		pixman_region32_init_rect(&outside, 0, 0, width, height);
		pixman_region32_t keep;
		pixman_region32_init_rect(&keep, clip.x, clip.y, clip.width, clip.height);
		pixman_region32_subtract(&outside, &outside, &keep);
		int n;
		const pixman_box32_t *boxes = pixman_region32_rectangles(&outside, &n);
		pixman_image_fill_boxes(PIXMAN_OP_SRC, mask, &(pixman_color_t){0}, n, boxes);
		pixman_region32_fini(&keep);
		pixman_region32_fini(&outside);
		ok = mask_cut_corners(renderer, mask, clip.x, clip.y, clip.width,
			clip.height, key->clips[i].radii);
	}

	if (!ok) {
		pixman_image_unref(mask);
		return NULL;
	}
	if (key->alpha != 1) {
		pixman_image_t *fill = pixman_image_create_solid_fill(&(struct pixman_color){
			.alpha = 0xFFFF * key->alpha,
		});
		pixman_image_composite32(PIXMAN_OP_IN, fill, NULL, mask,
			0, 0, 0, 0, 0, 0, width, height);
		pixman_image_unref(fill);
	}
	return mask;
}

// The mask for a box with its radii, cutout, clips and alpha: a referenced
// hit from the renderer's cache, or one built now and kept at its head, the
// oldest dropped past WLR_PIXMAN_ROUNDED_MASKS. The caller unrefs its use.
static pixman_image_t *rounded_mask(struct wlr_pixman_renderer *renderer,
		const struct wlr_box *box, const float radii[static 4],
		const struct wlr_box *cutout, const float *cutout_radii,
		const struct wlr_render_rounded_clip *clips, size_t clips_len, float alpha) {
	struct wlr_pixman_rounded_mask_key key;
	rounded_mask_key(&key, box, radii, cutout, cutout_radii, clips, clips_len, alpha);

	struct wlr_pixman_rounded_mask *entry;
	wl_list_for_each(entry, &renderer->rounded_masks, link) {
		if (memcmp(&entry->key, &key, sizeof(key)) == 0) {
			return pixman_image_ref(entry->image);
		}
	}

	pixman_image_t *mask = build_rounded_mask(renderer, &key);
	if (mask == NULL) {
		return NULL;
	}
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return mask;
	}
	entry->key = key;
	entry->image = mask;
	wl_list_insert(&renderer->rounded_masks, &entry->link);
	if (++renderer->rounded_masks_len > WLR_PIXMAN_ROUNDED_MASKS) {
		struct wlr_pixman_rounded_mask *oldest =
			wl_container_of(renderer->rounded_masks.prev, oldest, link);
		pixman_image_unref(oldest->image);
		wl_list_remove(&oldest->link);
		free(oldest);
		renderer->rounded_masks_len--;
	}
	return pixman_image_ref(mask);
}

static void render_pass_add_texture(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_texture_options *options) {
	struct wlr_pixman_render_pass *pass = get_render_pass(wlr_pass);
	struct wlr_pixman_texture *texture = get_texture(options->texture);
	struct wlr_pixman_buffer *buffer = pass->buffer;

	if (texture->buffer != NULL && !begin_pixman_data_ptr_access(texture->buffer,
			&texture->image, WLR_BUFFER_DATA_PTR_ACCESS_READ)) {
		return;
	}

	pixman_op_t op = get_pixman_blending(options->blend_mode);
	pixman_image_set_clip_region32(buffer->image, options->clip);

	struct wlr_fbox src_fbox;
	wlr_render_texture_options_get_src_box(options, &src_fbox);
	struct wlr_box src_box = {
		.x = roundf(src_fbox.x),
		.y = roundf(src_fbox.y),
		.width = roundf(src_fbox.width),
		.height = roundf(src_fbox.height),
	};

	struct wlr_box dst_box;
	wlr_render_texture_options_get_dst_box(options, &dst_box);

	pixman_image_t *mask = NULL;
	float alpha = wlr_render_texture_options_get_alpha(options);
	if (wlr_render_texture_options_is_rounded(options)) {
		mask = rounded_mask(buffer->renderer, &dst_box, options->corner_radii,
			NULL, NULL, options->clips, options->clips_len, alpha);
		// A cut corner is transparent whatever the texture is, so it blends
		op = PIXMAN_OP_OVER;
	}
	if (mask == NULL && alpha != 1) {
		mask = pixman_image_create_solid_fill(&(struct pixman_color){
			.alpha = 0xFFFF * alpha,
		});
	}

	// Rotate the source size into destination coordinates
	struct wlr_box src_box_transformed;
	wlr_box_transform(&src_box_transformed, &src_box, options->transform,
		buffer->buffer->width, buffer->buffer->height);

	if (options->transform != WL_OUTPUT_TRANSFORM_NORMAL ||
			src_box_transformed.width != dst_box.width ||
			src_box_transformed.height != dst_box.height) {
		// Cosinus/sinus values are exact integers for enum wl_output_transform entries
		int tr_cos = 1, tr_sin = 0, tr_x = 0, tr_y = 0;
		switch (options->transform) {
		case WL_OUTPUT_TRANSFORM_NORMAL:
		case WL_OUTPUT_TRANSFORM_FLIPPED:
			break;
		case WL_OUTPUT_TRANSFORM_90:
		case WL_OUTPUT_TRANSFORM_FLIPPED_90:
			tr_cos = 0;
			tr_sin = 1;
			tr_y = src_box.width;
			break;
		case WL_OUTPUT_TRANSFORM_180:
		case WL_OUTPUT_TRANSFORM_FLIPPED_180:
			tr_cos = -1;
			tr_sin = 0;
			tr_x = src_box.width;
			tr_y = src_box.height;
			break;
		case WL_OUTPUT_TRANSFORM_270:
		case WL_OUTPUT_TRANSFORM_FLIPPED_270:
			tr_cos = 0;
			tr_sin = -1;
			tr_x = src_box.height;
			break;
		}

		// Pixman transforms are generally the opposite of what you expect because they
		// apply to the coordinate system rather than the image.  The comments here
		// refer to what happens to the image, so all the code between
		// pixman_transform_init_identity() and pixman_image_set_transform() is probably
		// best read backwards.  Also this means translations are in the opposite
		// direction, imagine them as moving the origin around rather than moving the
		// image.
		//
		// Beware that this doesn't work quite the same as wp_viewporter: We apply crop
		// before transform and scale, whereas it defines crop in post-transform-scale
		// coordinates.  But this only applies to internal wlroots code - the viewporter
		// extension code makes sure that to clients everything works as it should.

		struct pixman_transform transform;
		pixman_transform_init_identity(&transform);

		// Apply scaling to get to the dst_box size.  Because the scaling is applied last
		// it depends on the whether the rotation swapped width and height, which is why
		// we use src_box_transformed instead of src_box.
		pixman_transform_scale(&transform, NULL,
			pixman_double_to_fixed(src_box_transformed.width / (double)dst_box.width),
			pixman_double_to_fixed(src_box_transformed.height / (double)dst_box.height));

		// pixman rotates about the origin which again leaves everything outside of the
		// viewport.  Translate the result so that its new top-left corner is back at the
		// origin.
		pixman_transform_translate(&transform, NULL,
			-pixman_int_to_fixed(tr_x), -pixman_int_to_fixed(tr_y));

		// Apply the rotation
		pixman_transform_rotate(&transform, NULL,
			pixman_int_to_fixed(tr_cos), pixman_int_to_fixed(tr_sin));

		// Apply flip before rotation
		if (options->transform >= WL_OUTPUT_TRANSFORM_FLIPPED) {
			// The flip leaves everything left of the Y axis which is outside the
			// viewport. So translate everything back into the viewport.
			pixman_transform_translate(&transform, NULL,
				-pixman_int_to_fixed(src_box.width), pixman_int_to_fixed(0));
			// Flip by applying a scale of -1 to the X axis
			pixman_transform_scale(&transform, NULL,
				pixman_int_to_fixed(-1), pixman_int_to_fixed(1));
		}

		// Apply the translation for source crop so the origin is now at the top-left of
		// the region we're actually using.  Do this last so all the other transforms
		// apply on top of this.
		pixman_transform_translate(&transform, NULL,
			pixman_int_to_fixed(src_box.x), pixman_int_to_fixed(src_box.y));

		pixman_image_set_transform(texture->image, &transform);

		switch (options->filter_mode) {
		case WLR_SCALE_FILTER_BILINEAR:
			pixman_image_set_repeat(texture->image, PIXMAN_REPEAT_PAD);
			pixman_image_set_filter(texture->image, PIXMAN_FILTER_BILINEAR, NULL, 0);
			break;
		case WLR_SCALE_FILTER_NEAREST:
			pixman_image_set_filter(texture->image, PIXMAN_FILTER_NEAREST, NULL, 0);
			break;
		}

		// Now composite the result onto the pass buffer.  We specify a source origin of 0,0
		// because the x,y part of source crop is already done using the transform. The
		// width,height part of source crop is done here by the width and height we pass:
		// because of the scaling, cropping at the end by dst_box.{width,height} is
		// equivalent to if we cropped at the start by src_box.{width,height}.
		pixman_image_composite32(op, texture->image, mask, buffer->image,
			0, 0, // source x,y
			0, 0, // mask x,y
			dst_box.x, dst_box.y, // dest x,y
			dst_box.width, dst_box.height // composite width,height
		);

		pixman_image_set_transform(texture->image, NULL);
	} else {
		// No transforms or crop needed, just a straight blit from the source
		pixman_image_set_transform(texture->image, NULL);
		pixman_image_composite32(op, texture->image, mask, buffer->image,
			src_box.x, src_box.y, 0, 0, dst_box.x, dst_box.y,
			src_box.width, src_box.height);
	}

	pixman_image_set_clip_region32(buffer->image, NULL);

	if (texture->buffer != NULL) {
		wlr_buffer_end_data_ptr_access(texture->buffer);
	}

	if (mask != NULL) {
		pixman_image_unref(mask);
	}
}

static void render_pass_add_rect(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_rect_options *options) {
	struct wlr_pixman_render_pass *pass = get_render_pass(wlr_pass);
	struct wlr_pixman_buffer *buffer = pass->buffer;
	struct wlr_box box;
	wlr_render_rect_options_get_box(options, pass->buffer->buffer, &box);

	pixman_op_t op = get_pixman_blending(options->color.a == 1 ?
		WLR_RENDER_BLEND_MODE_NONE : options->blend_mode);

	struct pixman_color color = {
		.red = options->color.r * 0xFFFF,
		.green = options->color.g * 0xFFFF,
		.blue = options->color.b * 0xFFFF,
		.alpha = options->color.a * 0xFFFF,
	};

	pixman_image_t *fill = pixman_image_create_solid_fill(&color);

	// A rounded rect is the fill through its coverage mask, so it blends
	pixman_image_t *mask = NULL;
	if (wlr_render_rect_options_is_rounded(options) && box.width > 0 && box.height > 0) {
		mask = rounded_mask(buffer->renderer, &box, options->corner_radii,
			&options->cutout, options->cutout_radii, options->clips,
			options->clips_len, 1);
		op = PIXMAN_OP_OVER;
	}

	pixman_image_set_clip_region32(buffer->image, options->clip);
	pixman_image_composite32(op, fill, mask, buffer->image,
		0, 0, 0, 0, box.x, box.y, box.width, box.height);
	pixman_image_set_clip_region32(buffer->image, NULL);

	pixman_image_unref(fill);
	if (mask != NULL) {
		pixman_image_unref(mask);
	}
}

static const struct wlr_render_pass_impl render_pass_impl = {
	.submit = render_pass_submit,
	.add_texture = render_pass_add_texture,
	.add_rect = render_pass_add_rect,
};

struct wlr_pixman_render_pass *begin_pixman_render_pass(
		struct wlr_pixman_buffer *buffer) {
	struct wlr_pixman_render_pass *pass = calloc(1, sizeof(*pass));
	if (pass == NULL) {
		return NULL;
	}

	wlr_render_pass_init(&pass->base, &render_pass_impl);

	if (!begin_pixman_data_ptr_access(buffer->buffer, &buffer->image,
			WLR_BUFFER_DATA_PTR_ACCESS_READ | WLR_BUFFER_DATA_PTR_ACCESS_WRITE)) {
		free(pass);
		return NULL;
	}

	wlr_buffer_lock(buffer->buffer);
	pass->buffer = buffer;

	return pass;
}

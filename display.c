#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "private.h"

static struct hwc_plane *plane_create(struct hwc_display *display, int32_t id)
{
	struct hwc_plane *plane;
	drmModePlane *drm_plane;
	drmModeObjectProperties *drm_props;
	uint32_t i;
	drmModePropertyRes *drm_prop;
	struct hwc_plane_property *prop;

	plane = calloc(1, sizeof(*plane));
	if (plane == NULL) {
		return NULL;
	}

	drm_plane = drmModeGetPlane(display->drm_fd, id);
	if (drm_plane == NULL) {
		return NULL;
	}
	plane->id = drm_plane->plane_id;
	plane->possible_crtcs = drm_plane->possible_crtcs;
	drmModeFreePlane(drm_plane);

	drm_props = drmModeObjectGetProperties(display->drm_fd, id,
					       DRM_MODE_OBJECT_PLANE);
	if (drm_props == NULL) {
		return NULL;
	}
	plane->props = calloc(drm_props->count_props,
			      sizeof(struct hwc_plane_property));
	if (plane->props == NULL) {
		drmModeFreeObjectProperties(drm_props);
		return NULL;
	}
	for (i = 0; i < drm_props->count_props; i++) {
		drm_prop = drmModeGetProperty(display->drm_fd,
					      drm_props->props[i]);
		if (drm_prop == NULL) {
			drmModeFreeObjectProperties(drm_props);
			return NULL;
		}
		prop = &plane->props[i];
		memcpy(prop->name, drm_prop->name, sizeof(prop->name));
		prop->id = drm_prop->prop_id;
		drmModeFreeProperty(drm_prop);
		plane->props_len++;
	}
	drmModeFreeObjectProperties(drm_props);

	hwc_list_insert(display->planes.prev, &plane->link);

	return plane;
}

static void plane_destroy(struct hwc_plane *plane)
{
	hwc_list_remove(&plane->link);
	free(plane->props);
	free(plane);
}

struct hwc_display *hwc_display_create(int drm_fd)
{
	struct hwc_display *display;
	drmModePlaneRes *drm_plane_res;
	uint32_t i;

	display = calloc(1, sizeof(*display));
	if (display == NULL) {
		return NULL;
	}
	display->drm_fd = dup(drm_fd);
	if (display->drm_fd < 0) {
		hwc_display_destroy(display);
		return NULL;
	}

	hwc_list_init(&display->planes);
	hwc_list_init(&display->outputs);

	/* TODO: allow users to choose which layers to hand over */
	drm_plane_res = drmModeGetPlaneResources(drm_fd);
	if (drm_plane_res == NULL) {
		hwc_display_destroy(display);
		return NULL;
	}

	for (i = 0; i < drm_plane_res->count_planes; i++) {
		if (plane_create(display, drm_plane_res->planes[i]) == NULL) {
			hwc_display_destroy(display);
			return NULL;
		}
	}
	drmModeFreePlaneResources(drm_plane_res);

	return display;
}

void hwc_display_destroy(struct hwc_display *display)
{
	struct hwc_plane *plane, *tmp;

	close(display->drm_fd);
	hwc_list_for_each_safe(plane, tmp, &display->planes, link) {
		plane_destroy(plane);
	}
	free(display);
}

static struct hwc_plane_property *plane_get_property(struct hwc_plane *plane,
						     const char *name)
{
	size_t i;

	for (i = 0; i < plane->props_len; i++) {
		if (strcmp(plane->props[i].name, name) == 0) {
			return &plane->props[i];
		}
	}
	return NULL;
}

static bool plane_set_prop(struct hwc_plane *plane, drmModeAtomicReq *req,
			   struct hwc_plane_property *prop, uint64_t value)
{
	int ret;

	fprintf(stderr, "  Setting %s = %"PRIu64"\n", prop->name, value);
	ret = drmModeAtomicAddProperty(req, plane->id, prop->id, value);
	if (ret < 0) {
		perror("drmModeAtomicAddProperty");
		return false;
	}

	return true;
}

static bool plane_apply(struct hwc_plane *plane, struct hwc_layer *layer,
			drmModeAtomicReq *req)
{
	size_t i;
	struct hwc_layer_property *layer_prop;
	struct hwc_plane_property *plane_prop;

	if (layer == NULL) {
		plane_prop = plane_get_property(plane, "FB_ID");
		assert(plane_prop);
		return plane_set_prop(plane, req, plane_prop, 0);
	}

	plane_prop = plane_get_property(plane, "CRTC_ID");
	assert(plane_prop);
	if (!plane_set_prop(plane, req, plane_prop, layer->output->crtc_id)) {
		return false;
	}

	for (i = 0; i < layer->props_len; i++) {
		layer_prop = &layer->props[i];
		plane_prop = plane_get_property(plane, layer_prop->name);
		if (plane_prop == NULL) {
			fprintf(stderr, "failed to find property %s\n",
				layer_prop->name);
			return false;
		}

		if (!plane_set_prop(plane, req, plane_prop, layer_prop->value)) {
			return false;
		}
	}

	return true;
}

static bool layer_choose_plane(struct hwc_layer *layer, drmModeAtomicReq *req)
{
	struct hwc_display *display;
	int cursor;
	struct hwc_plane *plane;
	int ret;

	display = layer->output->display;
	cursor = drmModeAtomicGetCursor(req);

	hwc_list_for_each(plane, &display->planes, link) {
		if (plane->layer != NULL) {
			continue;
		}

		fprintf(stderr, "Trying to apply layer %p with plane %d...\n",
			(void *)layer, plane->id);
		if (!plane_apply(plane, layer, req)) {
			return false;
		}

		ret = drmModeAtomicCommit(display->drm_fd, req,
					  DRM_MODE_ATOMIC_TEST_ONLY, NULL);
		if (ret == 0) {
			fprintf(stderr, "Success\n");
			layer->plane = plane;
			plane->layer = layer;
			return true;
		} else if (-ret != EINVAL && -ret != ERANGE) {
			perror("drmModeAtomicCommit");
			return false;
		}

		fprintf(stderr, "Failure\n");
		drmModeAtomicSetCursor(req, cursor);
	}

	fprintf(stderr, "Failed to find plane for layer %p\n", (void *)layer);
	return true;
}

bool hwc_display_apply(struct hwc_display *display, drmModeAtomicReq *req)
{
	struct hwc_output *output;
	struct hwc_layer *layer;
	struct hwc_plane *plane;

	/* Unset all existing plane and layer mappings.
	   TODO: incremental updates keeping old configuration if possible */
	hwc_list_for_each(plane, &display->planes, link) {
		if (plane->layer != NULL) {
			plane->layer->plane = NULL;
			plane->layer = NULL;
		}
	}

	/* Disable all planes. Do it before building mappings to make sure not
	   to hit bandwidth limits because too many planes are enabled. */
	hwc_list_for_each(plane, &display->planes, link) {
		if (plane->layer == NULL) {
			fprintf(stderr, "Disabling plane %d\n", plane->id);
			if (!plane_apply(plane, NULL, req)) {
				return false;
			}
		}
	}

	hwc_list_for_each(output, &display->outputs, link) {
		hwc_list_for_each(layer, &output->layers, link) {
			if (!layer_choose_plane(layer, req)) {
				return false;
			}
		}
	}

	return true;
}

#pragma once
#include <stdint.h>
#include <stdbool.h>

#define GHOST_ICON_SIZE 192
#define GHOST_ICON_MARGIN 24

/* Matches Poingo's lower-right cast shadow: 4.8px right, 8px down and
 * 40% black at the core.  Integer offsets keep the other two raster paths
 * aligned with the same light direction. */
#define GHOST_ICON_SHADOW_OFFSET_X 5
#define GHOST_ICON_SHADOW_OFFSET_Y 8
#define GHOST_ICON_SHADOW_OPACITY 0.40f

#ifdef __cplusplus
extern "C" {
#endif

// Allocates and returns a GHOST_ICON_SIZE x GHOST_ICON_SIZE RGBA buffer
// containing an anti-aliased, procedurally grained soapstone circle with a
// labradorite inlay bordered by thin brushed-gold bands. The image is internally
// supersampled before being returned at GHOST_ICON_SIZE.
// Returns NULL on out-of-memory.
// The `premultiplied` flag determines whether the RGB channels should be
// pre-multiplied by the alpha channel, to match the OpenGL blend mode.
uint32_t* ghost_icon_create_bg(bool premultiplied);

/* Composites a soft, Poingo-style black shadow from source's alpha channel
 * onto destination. Both buffers are width x height straight-alpha RGBA
 * pixels in the native Wayland/OpenGL byte layout used by the toys. */
void ghost_icon_composite_shadow(uint32_t *destination, const uint32_t *source,
                                 int width, int height);

#ifdef __cplusplus
}
#endif

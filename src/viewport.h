#ifndef IMV_VIEWPORT_H
#define IMV_VIEWPORT_H

#include <stdbool.h>
#include <SDL2/SDL.h>
#include "image.h"

struct imv_viewport;

/* Used to signify how a a user requested a zoom */
enum imv_zoom_source {
  IMV_ZOOM_MOUSE,
  IMV_ZOOM_KEYBOARD
};

/* Creates an instance of imv_viewport */
struct imv_viewport *imv_viewport_create(SDL_Window *window, SDL_Renderer *renderer);

/* Cleans up an imv_viewport instance */
void imv_viewport_free(struct imv_viewport *view);

/* Toggle their viewport's fullscreen mode. Triggers a redraw */
void imv_viewport_toggle_fullscreen(struct imv_viewport *view);

/* Set playback of animated gifs */
void imv_viewport_set_playing(struct imv_viewport *view, bool playing);

/* Get playback status of animated gifs */
bool imv_viewport_is_playing(struct imv_viewport *view);

/* Toggle playback of animated gifs */
void imv_viewport_toggle_playing(struct imv_viewport *view);

/* Fetch viewport offset/position */
void imv_viewport_get_offset(struct imv_viewport *view, int *x, int *y);

/* Fetch viewport scale */
void imv_viewport_get_scale(struct imv_viewport *view, double *scale);

/* Pan the view by the given amounts without letting the image get too far
 * off-screen */
void imv_viewport_move(struct imv_viewport *view, int x, int y,
    const struct imv_image *image);

/* Zoom the view by the given amount. imv_image* is used to get the image
 * dimensions */
void imv_viewport_zoom(struct imv_viewport *view, const struct imv_image *image,
                       enum imv_zoom_source, int amount);

/* Recenter the view to be in the middle of the image */
void imv_viewport_center(struct imv_viewport *view,
                         const struct imv_image *image);

/* Recenter the view to be in the top of the image */
void imv_viewport_top(struct imv_viewport *view,
                         const struct imv_image *image);

/* Recenter the view to be in the bottom of the image */
void imv_viewport_bottom(struct imv_viewport *view,
                         const struct imv_image *image);

/* Scale the view so that the image appears at its actual resolution */
void imv_viewport_scale_to_actual(struct imv_viewport *view,
                                  const struct imv_image *image);

/* Scale the view so that the image fills the window */
void imv_viewport_scale_to_window(struct imv_viewport *view,
                                  const struct imv_image *image);

/* Tell the viewport that it needs to be redrawn */
void imv_viewport_set_redraw(struct imv_viewport *view);

/* Set the title of the viewport */
void imv_viewport_set_title(struct imv_viewport *view, char *title);

/* Tell the viewport the window or image has changed */
void imv_viewport_update(struct imv_viewport *view, struct imv_image *image);

/* Poll whether we need to redraw */
int imv_viewport_needs_redraw(struct imv_viewport *view);

#endif

/* vim:set ts=2 sts=2 sw=2 et: */

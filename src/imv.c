#include "imv.h"

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <wordexp.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "backend.h"
#include "binds.h"
#include "commands.h"
#include "image.h"
#include "ini.h"
#include "list.h"
#include "log.h"
#include "navigator.h"
#include "source.h"
#include "util.h"
#include "viewport.h"

/* Some systems like GNU/Hurd don't define PATH_MAX */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum scaling_mode {
  SCALING_NONE,
  SCALING_DOWN,
  SCALING_FULL,
  SCALING_MODE_COUNT
};

enum upscaling_method {
  UPSCALING_LINEAR,
  UPSCALING_NEAREST_NEIGHBOUR,
  UPSCALING_METHOD_COUNT,
};

static const char *scaling_label[] = {
  "actual size",
  "shrink to fit",
  "scale to fit"
};

enum background_type {
  BACKGROUND_SOLID,
  BACKGROUND_CHEQUERED,
  BACKGROUND_TYPE_COUNT
};

/* window behaviour on image change */
enum resize_mode {
  RESIZE_NONE,  /* do nothing */
  RESIZE_ONLY,  /* resize to fit the new image */
  RESIZE_CENTER /* resize to fit the new image and recenter */
};

struct backend_chain {
  const struct imv_backend *backend;
  struct backend_chain *next;
};

struct imv {
  /* set to true to trigger clean exit */
  bool quit;

  /* indicates a new image is being loaded */
  bool loading;

  /* fullscreen state */
  bool fullscreen;

  /* initial window dimensions */
  int initial_width;
  int initial_height;

  /* display some textual info onscreen */
  bool overlay_enabled;

  /* method for scaling up images: interpolate or nearest neighbour */
  enum upscaling_method upscaling_method;

  /* for multiple monitors, should we stay fullscreen if we lose focus? */
  bool stay_fullscreen_on_focus_loss;

  /* dirty state flags */
  bool need_redraw;
  bool need_rescale;
  bool need_resize;

  /* mode for resizing the window on image change */
  enum resize_mode resize_mode;

  /* traverse sub-directories for more images */
  bool recursive_load;

  /* 'next' on the last image goes back to the first */
  bool loop_input;

  /* print all paths to stdout on clean exit */
  bool list_files_at_exit;

  /* read paths from stdin, as opposed to image data */
  bool paths_from_stdin;

  /* scale up / down images to match window, or actual size */
  enum scaling_mode scaling_mode;

  /* show a solid background colour, or chequerboard pattern */
  enum background_type background_type;
  /* the aforementioned background colour */
  struct { unsigned char r, g, b; } background_color;

  /* slideshow state tracking */
  unsigned long slideshow_image_duration;
  unsigned long slideshow_time_elapsed;

  /* for animated images, the GetTicks() time to display the next frame */
  unsigned int next_frame_due;
  /* how long the next frame to be put onscreen should be displayed for */
  int next_frame_duration;
  /* the next frame of an animated image, pre-fetched */
  struct imv_bitmap *next_frame;

  /* overlay font name */
  char *font_name;
  /* buffer for storing input commands, NULL when not in command mode */
  char *input_buffer;

  /* if specified by user, the path of the first image to display */
  char *starting_path;

  /* the user-specified format strings for the overlay and window title */
  char *title_text;
  char *overlay_text;

  /* when true, imv will ignore all window events until it encounters a
   * ENABLE_INPUT user-event. This is required to overcome a bug where
   * SDL will send input events to us from before we gained focus
   */
  bool ignore_window_events;

  /* imv subsystems */
  struct imv_binds *binds;
  struct imv_navigator *navigator;
  struct backend_chain *backends;
  struct imv_source *source;
  struct imv_source *last_source;
  struct imv_commands *commands;
  struct imv_image *image;
  struct imv_viewport *view;

  /* if reading an image from stdin, this is the buffer for it */
  void *stdin_image_data;
  size_t stdin_image_data_len;

  /* SDL subsystems */
  SDL_Window *window;
  SDL_Renderer *renderer;
  TTF_Font *font;
  SDL_Texture *background_image;
  bool sdl_init;
  bool ttf_init;
  struct {
    unsigned int NEW_IMAGE;
    unsigned int BAD_IMAGE;
    unsigned int NEW_PATH;
    unsigned int ENABLE_INPUT;
  } events;
  struct {
    int width;
    int height;
  } current_image;
};

void command_quit(struct list *args, const char *argstr, void *data);
void command_pan(struct list *args, const char *argstr, void *data);
void command_select_rel(struct list *args, const char *argstr, void *data);
void command_select_abs(struct list *args, const char *argstr, void *data);
void command_zoom(struct list *args, const char *argstr, void *data);
void command_open(struct list *args, const char *argstr, void *data);
void command_close(struct list *args, const char *argstr, void *data);
void command_fullscreen(struct list *args, const char *argstr, void *data);
void command_overlay(struct list *args, const char *argstr, void *data);
void command_exec(struct list *args, const char *argstr, void *data);
void command_center(struct list *args, const char *argstr, void *data);
void command_top(struct list *args, const char *argstr, void *data);
void command_bottom(struct list *args, const char *argstr, void *data);
void command_reset(struct list *args, const char *argstr, void *data);
void command_next_frame(struct list *args, const char *argstr, void *data);
void command_toggle_playing(struct list *args, const char *argstr, void *data);
void command_set_scaling_mode(struct list *args, const char *argstr, void *data);
void command_set_slideshow_duration(struct list *args, const char *argstr, void *data);

static bool setup_window(struct imv *imv);
static void handle_event(struct imv *imv, SDL_Event *event);
static void render_window(struct imv *imv);
static void update_env_vars(struct imv *imv);
static size_t generate_env_text(struct imv *imv, char *buf, size_t len, const char *format);


/* Finds the next split between commands in a string (';'). Provides a pointer
 * to the next character after the delimiter as out, or a pointer to '\0' if
 * nothing is left. Also provides the len from start up to the delimiter.
 */
static void split_commands(const char *start, const char **out, size_t *len)
{
  bool in_single_quotes = false;
  bool in_double_quotes = false;

  const char *str = start;

  while (*str) {
    if (!in_single_quotes && *str == '"') {
      in_double_quotes = !in_double_quotes;
    } else if (!in_double_quotes && *str == '\'') {
      in_single_quotes = !in_single_quotes;
    } else if (*str == '\\') {
      /* We don't care about the behaviour of any escaped character, just
       * make sure to skip over them. We do need to make sure not to allow
       * escaping of the null terminator though.
       */
      if (str[1] != '\0') {
        ++str;
      }
    } else if (!in_single_quotes && !in_double_quotes && *str == ';') {
      /* Found a command split that wasn't escaped or quoted */
      *len = str - start;
      *out = str + 1;
      return;
    }
    ++str;
  }

  *out = str;
  *len = str - start;
}

static bool add_bind(struct imv *imv, const char *keys, const char *commands)
{
  struct list *list = imv_bind_parse_keys(keys);
  if(!list) {
    imv_log(IMV_ERROR, "Invalid key combination");
    return false;
  }

  char command_buf[512];
  const char *next_command;
  size_t command_len;

  bool success = true;

  imv_binds_clear_key(imv->binds, list);
  while (*commands != '\0') {
    split_commands(commands, &next_command, &command_len);

    if (command_len >= sizeof command_buf) {
      imv_log(IMV_ERROR, "Command exceeded max length, not binding: %.*s\n", (int)command_len, commands);
      imv_binds_clear_key(imv->binds, list);
      success = false;
      break;
    }
    strncpy(command_buf, commands, command_len);
    command_buf[command_len] = '\0';

    enum bind_result result = imv_binds_add(imv->binds, list, command_buf);

    if (result == BIND_INVALID_KEYS) {
      imv_log(IMV_ERROR, "Invalid keys to bind to");
      success = false;
      break;
    } else if (result == BIND_INVALID_COMMAND) {
      imv_log(IMV_ERROR, "No command given to bind to");
      success = false;
      break;
    } else if (result == BIND_CONFLICTS) {
      imv_log(IMV_ERROR, "Key combination conflicts with existing bind");
      success = false;
      break;
    }
    commands = next_command;
  }

  list_free(list);

  return success;
}

static int async_free_source_thread(void *raw)
{
  struct imv_source *src = raw;
  src->free(src);
  return 0;
}

static void async_free_source(struct imv_source *src)
{
  SDL_Thread *thread = SDL_CreateThread(async_free_source_thread,
      "async_free_source", src);
  SDL_DetachThread(thread);
}

static void async_load_first_frame(struct imv_source *src)
{
  typedef int (*thread_func)(void*);
  SDL_Thread *thread = SDL_CreateThread((thread_func)src->load_first_frame,
      "async_load_first_frame",
      src);
  SDL_DetachThread(thread);
}

static void async_load_next_frame(struct imv_source *src)
{
  typedef int (*thread_func)(void*);
  SDL_Thread *thread = SDL_CreateThread((thread_func)src->load_next_frame,
      "async_load_next_frame",
      src);
  SDL_DetachThread(thread);
}

static void source_callback(struct imv_source_message *msg)
{
  struct imv *imv = msg->user_data;
  if (msg->source != imv->source) {
    /* We received a message from an old source, tidy up contents
     * as required, but ignore it.
     */
    if (msg->bitmap) {
      imv_bitmap_free(msg->bitmap);
    }
    return;
  }

  SDL_Event event;
  SDL_zero(event);

  if (msg->bitmap) {
    event.type = imv->events.NEW_IMAGE;
    event.user.data1 = msg->bitmap;
    event.user.code = msg->frametime;

    /* Keep track of the last source to send us a bitmap in order to detect
     * when we're getting a new image, as opposed to a new frame from the
     * same image.
     */
    uintptr_t is_new_image = msg->source != imv->last_source;
    event.user.data2 = (void*)is_new_image;
    imv->last_source = msg->source;
  } else {
    event.type = imv->events.BAD_IMAGE;
    /* TODO: Something more elegant with error messages */
    /* event.user.data1 = strdup(msg->error); */
  }

  SDL_PushEvent(&event);
}

struct imv *imv_create(void)
{
  struct imv *imv = calloc(1, sizeof *imv);
  imv->initial_width = 1280;
  imv->initial_height = 720;
  imv->need_redraw = true;
  imv->need_rescale = true;
  imv->scaling_mode = SCALING_FULL;
  imv->loop_input = true;
  imv->font_name = strdup("Monospace:24");
  imv->binds = imv_binds_create();
  imv->navigator = imv_navigator_create();
  imv->commands = imv_commands_create();
  imv->title_text = strdup(
      "imv - [${imv_current_index}/${imv_file_count}]"
      " [${imv_width}x${imv_height}] [${imv_scale}%]"
      " $imv_current_file [$imv_scaling_mode]"
  );
  imv->overlay_text = strdup(
      "[${imv_current_index}/${imv_file_count}]"
      " [${imv_width}x${imv_height}] [${imv_scale}%]"
      " $imv_current_file [$imv_scaling_mode]"
  );

  imv_command_register(imv->commands, "quit", &command_quit);
  imv_command_register(imv->commands, "pan", &command_pan);
  imv_command_register(imv->commands, "select_rel", &command_select_rel);
  imv_command_register(imv->commands, "select_abs", &command_select_abs);
  imv_command_register(imv->commands, "zoom", &command_zoom);
  imv_command_register(imv->commands, "open", &command_open);
  imv_command_register(imv->commands, "close", &command_close);
  imv_command_register(imv->commands, "fullscreen", &command_fullscreen);
  imv_command_register(imv->commands, "overlay", &command_overlay);
  imv_command_register(imv->commands, "exec", &command_exec);
  imv_command_register(imv->commands, "center", &command_center);
  imv_command_register(imv->commands, "top", &command_top);
  imv_command_register(imv->commands, "bottom", &command_bottom);
  imv_command_register(imv->commands, "reset", &command_reset);
  imv_command_register(imv->commands, "next_frame", &command_next_frame);
  imv_command_register(imv->commands, "toggle_playing", &command_toggle_playing);
  imv_command_register(imv->commands, "scaling_mode", &command_set_scaling_mode);
  imv_command_register(imv->commands, "slideshow_duration", &command_set_slideshow_duration);

  add_bind(imv, "q", "quit");
  add_bind(imv, "<Left>", "select_rel -1");
  add_bind(imv, "<LeftSquareBracket>", "select_rel -1");
  add_bind(imv, "<Right>", "select_rel 1");
  add_bind(imv, "<RightSquareBracket>", "select_rel 1");
  add_bind(imv, "gg", "select_abs 0");
  add_bind(imv, "<Shift+g>", "select_abs -1");
  add_bind(imv, "j", "pan 0 -50");
  add_bind(imv, "k", "pan 0 50");
  add_bind(imv, "h", "pan 50 0");
  add_bind(imv, "l", "pan -50 0");
  add_bind(imv, "x", "close");
  add_bind(imv, "f", "fullscreen");
  add_bind(imv, "d", "overlay");
  add_bind(imv, "p", "exec echo $imv_current_file");
  add_bind(imv, "<Equals>", "zoom 1");
  add_bind(imv, "<Up>", "zoom 1");
  add_bind(imv, "+", "zoom 1");
  add_bind(imv, "i", "zoom 1");
  add_bind(imv, "<Down>", "zoom -1");
  add_bind(imv, "-", "zoom -1");
  add_bind(imv, "o", "zoom -1");
  add_bind(imv, "c", "center");
  add_bind(imv, "s", "scaling_mode next");
  add_bind(imv, "a", "zoom actual");
  add_bind(imv, "r", "reset");
  add_bind(imv, ".", "next_frame");
  add_bind(imv, "<Space>", "toggle_playing");
  add_bind(imv, "t", "slideshow_duration +1");
  add_bind(imv, "<Shift+t>", "slideshow_duration -1");

  return imv;
}

void imv_free(struct imv *imv)
{
  free(imv->font_name);
  free(imv->title_text);
  free(imv->overlay_text);
  imv_binds_free(imv->binds);
  imv_navigator_free(imv->navigator);
  if (imv->source) {
    imv->source->free(imv->source);
  }
  imv_commands_free(imv->commands);
  imv_viewport_free(imv->view);
  if (imv->image) {
    imv_image_free(imv->image);
  }
  if (imv->next_frame) {
    imv_bitmap_free(imv->next_frame);
  }
  if(imv->stdin_image_data) {
    free(imv->stdin_image_data);
  }
  if(imv->input_buffer) {
    free(imv->input_buffer);
  }
  if(imv->renderer) {
    SDL_DestroyRenderer(imv->renderer);
  }
  if(imv->window) {
    SDL_DestroyWindow(imv->window);
  }
  if(imv->background_image) {
    SDL_DestroyTexture(imv->background_image);
  }
  if(imv->font) {
    TTF_CloseFont(imv->font);
  }
  if(imv->ttf_init) {
    TTF_Quit();
  }
  if(imv->sdl_init) {
    SDL_Quit();
  }
  free(imv);
}

void imv_install_backend(struct imv *imv, const struct imv_backend *backend)
{
  struct backend_chain *chain = malloc(sizeof *chain);
  chain->backend = backend;
  chain->next = imv->backends;
  imv->backends = chain;
}

static bool parse_bg(struct imv *imv, const char *bg)
{
  if(strcmp("checks", bg) == 0) {
    imv->background_type = BACKGROUND_CHEQUERED;
  } else {
    imv->background_type = BACKGROUND_SOLID;
    if(*bg == '#')
      ++bg;
    char *ep;
    uint32_t n = strtoul(bg, &ep, 16);
    if(*ep != '\0' || ep - bg != 6 || n > 0xFFFFFF) {
      imv_log(IMV_ERROR, "Invalid hex color: '%s'\n", bg);
      return false;
    }
    imv->background_color.b = n & 0xFF;
    imv->background_color.g = (n >> 8) & 0xFF;
    imv->background_color.r = (n >> 16);
  }
  return true;
}

static bool parse_slideshow_duration(struct imv *imv, const char *duration)
{
  char *decimal;
  imv->slideshow_image_duration = strtoul(duration, &decimal, 10);
  imv->slideshow_image_duration *= 1000;
  if (*decimal == '.') {
    char *ep;
    long delay = strtoul(++decimal, &ep, 10);
    for (int i = 3 - (ep - decimal); i; i--) {
      delay *= 10;
    }
    if (delay < 1000) {
      imv->slideshow_image_duration += delay;
    } else {
      imv->slideshow_image_duration = ULONG_MAX;
    }
  }
  if (imv->slideshow_image_duration == ULONG_MAX) {
    imv_log(IMV_ERROR, "Wrong slideshow duration '%s'. Aborting.\n", optarg);
    return false;
  }
  return true;
}

static bool parse_scaling_mode(struct imv *imv, const char *mode)
{
  if (!strcmp(mode, "shrink")) {
    imv->scaling_mode = SCALING_DOWN;
    return true;
  }

  if (!strcmp(mode, "full")) {
    imv->scaling_mode = SCALING_FULL;
    return true;
  }

  if (!strcmp(mode, "none")) {
    imv->scaling_mode = SCALING_NONE;
    return true;
  }

  return false;
}

static bool parse_upscaling_method(struct imv *imv, const char *method)
{
  if (!strcmp(method, "linear")) {
    imv->upscaling_method = UPSCALING_LINEAR;
    return true;
  }

  if (!strcmp(method, "nearest_neighbour")) {
    imv->upscaling_method = UPSCALING_NEAREST_NEIGHBOUR;
    return true;
  }

  return false;
}

static bool parse_resizing_mode(struct imv *imv, const char *method)
{
  if (!strcmp(method, "none")) {
    imv->resize_mode = RESIZE_NONE;
    return true;
  }

  if (!strcmp(method, "resize")) {
    imv->resize_mode = RESIZE_ONLY;
    return true;
  }

  if (!strcmp(method, "recenter")) {
    imv->resize_mode = RESIZE_CENTER;
    return true;
  }

  return false;
}

static int load_paths_from_stdin(void *data)
{
  struct imv *imv = data;

  imv_log(IMV_INFO, "Reading paths from stdin...");

  char buf[PATH_MAX];
  while(fgets(buf, sizeof(buf), stdin) != NULL) {
    size_t len = strlen(buf);
    if(buf[len-1] == '\n') {
      buf[--len] = 0;
    }
    if(len > 0) {
      /* return the path via SDL event queue */
      SDL_Event event;
      SDL_zero(event);
      event.type = imv->events.NEW_PATH;
      event.user.data1 = strdup(buf);
      SDL_PushEvent(&event);
    }
  }
  return 0;
}

static void print_help(struct imv *imv)
{
  printf("imv %s\nSee manual for usage information.\n", IMV_VERSION);
  puts("This version of imv has been compiled with the following backends:\n");

  for (struct backend_chain *chain = imv->backends;
       chain;
       chain = chain->next) {
    printf("Name: %s\n"
           "Description: %s\n"
           "Website: %s\n"
           "License: %s\n\n",
           chain->backend->name,
           chain->backend->description,
           chain->backend->website,
           chain->backend->license);
  }

  puts("Legal:\n"
       "imv's full source code is published under the terms of the MIT\n"
       "license, and can be found at https://github.com/eXeC64/imv\n"
       "\n"
       "imv uses the inih library to parse ini files.\n"
       "See https://github.com/benhoyt/inih for details.\n"
       "inih is used under the New (3-clause) BSD license.");
}

bool imv_parse_args(struct imv *imv, int argc, char **argv)
{
  /* Do not print getopt errors */
  opterr = 0;

  int o;

  while((o = getopt(argc, argv, "frdwWxhvlu:s:n:b:t:")) != -1) {
    switch(o) {
      case 'f': imv->fullscreen = true;                          break;
      case 'r': imv->recursive_load = true;                      break;
      case 'd': imv->overlay_enabled = true;                     break;
      case 'w': imv->resize_mode = RESIZE_ONLY;                  break;
      case 'W': imv->resize_mode = RESIZE_CENTER;                break;
      case 'x': imv->loop_input = false;                         break;
      case 'l': imv->list_files_at_exit = true;                  break;
      case 'n': imv->starting_path = optarg;                     break;
      case 'h':
        print_help(imv);
        imv->quit = true;
        return true;
      case 'v': 
        printf("Version: %s\n", IMV_VERSION);
          imv->quit = true;
          return false;
      case 's':
        if(!parse_scaling_mode(imv, optarg)) {
          imv_log(IMV_ERROR, "Invalid scaling mode. Aborting.\n");
          return false;
        }
        break;
      case 'u':
        if(!parse_upscaling_method(imv, optarg)) {
          imv_log(IMV_ERROR, "Invalid upscaling method. Aborting.\n");
          return false;
        }
        break;
      case 'b':
        if(!parse_bg(imv, optarg)) {
          imv_log(IMV_ERROR, "Invalid background. Aborting.\n");
          return false;
        }
        break;
      case 't':
        if(!parse_slideshow_duration(imv, optarg)) {
          imv_log(IMV_ERROR, "Invalid slideshow duration. Aborting.\n");
          return false;
        }
        break;
      case '?':
        imv_log(IMV_ERROR, "Unknown argument '%c'. Aborting.\n", optopt);
        return false;
    }
  }

  argc -= optind;
  argv += optind;

  /* if no paths are given as args, expect them from stdin */
  if(argc == 0) {
    imv->paths_from_stdin = true;
  } else {
    /* otherwise, add the paths */
    bool data_from_stdin = false;
    for(int i = 0; i < argc; ++i) {

      /* Special case: '-' denotes reading image data from stdin */
      if(!strcmp("-", argv[i])) {
        if(imv->paths_from_stdin) {
          imv_log(IMV_ERROR, "Can't read paths AND image data from stdin. Aborting.\n");
          return false;
        } else if(data_from_stdin) {
          imv_log(IMV_ERROR, "Can't read image data from stdin twice. Aborting.\n");
          return false;
        }
        data_from_stdin = true;

        imv->stdin_image_data_len = read_from_stdin(&imv->stdin_image_data);
      }

      imv_add_path(imv, argv[i]);
    }
  }

  return true;
}

void imv_add_path(struct imv *imv, const char *path)
{
  imv_navigator_add(imv->navigator, path, imv->recursive_load);
}

int imv_run(struct imv *imv)
{
  if(imv->quit)
    return 0;

  if(!setup_window(imv))
    return 1;

  /* if loading paths from stdin, kick off a thread to do that - we'll receive
   * events back via SDL */
  if(imv->paths_from_stdin) {
    SDL_Thread *thread;
    thread = SDL_CreateThread(load_paths_from_stdin, "load_paths_from_stdin", imv);
    SDL_DetachThread(thread);
  }

  if(imv->starting_path) {
    int index = imv_navigator_find_path(imv->navigator, imv->starting_path);
    if(index == -1) {
      index = (int) strtol(imv->starting_path, NULL, 10);
      index -= 1; /* input is 1-indexed, internally we're 0 indexed */
      if(errno == EINVAL) {
        index = -1;
      }
    }

    if(index >= 0) {
      imv_navigator_select_str(imv->navigator, index);
    } else {
      imv_log(IMV_ERROR, "Invalid starting image: %s\n", imv->starting_path);
    }
  }

  /* cache current image's dimensions */
  imv->current_image.width = 0;
  imv->current_image.height = 0;

  /* time keeping */
  unsigned int last_time = SDL_GetTicks();
  unsigned int current_time;

  while(!imv->quit) {

    SDL_Event e;
    while(!imv->quit && SDL_PollEvent(&e)) {
      handle_event(imv, &e);
    }

    /* if we're quitting, don't bother drawing any more images */
    if(imv->quit) {
      break;
    }

    /* Check if navigator wrapped around paths lists */
    if(!imv->loop_input && imv_navigator_wrapped(imv->navigator)) {
      break;
    }

    /* if we're out of images, and we're not expecting more from stdin, quit */
    if(!imv->paths_from_stdin && imv_navigator_length(imv->navigator) == 0) {
      imv_log(IMV_INFO, "No input files left. Exiting.\n");
      imv->quit = true;
      continue;
    }

    /* If the user has changed image, start loading the new one. It's possible
     * that there are lots of unsupported files listed back to back, so we
     * may immediate close one and navigate onto the next. So we attempt to
     * load in a while loop until the navigation stops.
     */
    while (imv_navigator_poll_changed(imv->navigator)) {
      const char *current_path = imv_navigator_selection(imv->navigator);
      /* check we got a path back */
      if(strcmp("", current_path)) {

        const bool path_is_stdin = !strcmp("-", current_path);
        struct imv_source *new_source;

        enum backend_result result = BACKEND_UNSUPPORTED;

        if (!imv->backends) {
          imv_log(IMV_ERROR, "No backends installed. Unable to load image.\n");
        }
        for (struct backend_chain *chain = imv->backends; chain; chain = chain->next) {
          const struct imv_backend *backend = chain->backend;
          if (path_is_stdin) {

            if (!backend->open_memory) {
              /* memory loading unsupported by backend */
              continue;
            }

            result = backend->open_memory(imv->stdin_image_data,
                imv->stdin_image_data_len, &new_source);
          } else {

            if (!backend->open_path) {
              /* path loading unsupported by backend */
              continue;
            }

            result = backend->open_path(current_path, &new_source);
          }
          if (result == BACKEND_UNSUPPORTED) {
            /* Try the next backend */
            continue;
          } else {
            break;
          }
        }

        if (result == BACKEND_SUCCESS) {
          if (imv->source) {
            async_free_source(imv->source);
          }
          imv->source = new_source;
          imv->source->callback = &source_callback;
          imv->source->user_data = imv;
          async_load_first_frame(imv->source);

          imv->loading = true;
          imv_viewport_set_playing(imv->view, true);

          char title[1024];
          generate_env_text(imv, title, sizeof title, imv->title_text);
          imv_viewport_set_title(imv->view, title);
        } else {
          /* Error loading path so remove it from the navigator */
          imv_navigator_remove(imv->navigator, current_path);
        }
      }
    }

    if(imv->need_rescale) {
      int ww, wh;
      SDL_GetWindowSize(imv->window, &ww, &wh);

      imv->need_rescale = false;
      if(imv->scaling_mode == SCALING_NONE ||
          (imv->scaling_mode == SCALING_DOWN
           && ww > imv->current_image.width
           && wh > imv->current_image.height)) {
        imv_viewport_scale_to_actual(imv->view, imv->image);
      } else {
        imv_viewport_scale_to_window(imv->view, imv->image);
      }
    }

    current_time = SDL_GetTicks();

    /* Check if a new frame is due */
    if (imv_viewport_is_playing(imv->view) && imv->next_frame
        && imv->next_frame_due && imv->next_frame_due <= current_time) {
      imv_image_set_bitmap(imv->image, imv->next_frame);
      imv->current_image.width = imv->next_frame->width;
      imv->current_image.height = imv->next_frame->height;
      imv_bitmap_free(imv->next_frame);
      imv->next_frame = NULL;
      imv->next_frame_due = current_time + imv->next_frame_duration;
      imv->next_frame_duration = 0;

      imv->need_redraw = true;

      /* Trigger loading of a new frame, now this one's being displayed */
      if (imv->source && imv->source->load_next_frame) {
        async_load_next_frame(imv->source);
      }
    }

    /* handle slideshow */
    if(imv->slideshow_image_duration != 0) {
      unsigned int dt = current_time - last_time;

      imv->slideshow_time_elapsed += dt;
      imv->need_redraw = true; /* need to update display */
      if(imv->slideshow_time_elapsed >= imv->slideshow_image_duration) {
        imv_navigator_select_rel(imv->navigator, 1);
        imv->slideshow_time_elapsed = 0;
      }
    }

    last_time = current_time;

    /* check if the viewport needs a redraw */
    if(imv_viewport_needs_redraw(imv->view)) {
      imv->need_redraw = true;
    }

    if(imv->need_redraw) {
      render_window(imv);
      SDL_RenderPresent(imv->renderer);
    }

    /* sleep until we have something to do */
    unsigned int timeout = 1000; /* milliseconds */

    /* if we need to display the next frame of an animation soon we should
     * limit our sleep until the next frame is due */
    if (imv_viewport_is_playing(imv->view)
        && imv->next_frame_due > current_time) {
      timeout = imv->next_frame_due - current_time;
    }

    /* go to sleep until an input event, etc. or the timeout expires */
    SDL_WaitEventTimeout(NULL, timeout);
  }

  if(imv->list_files_at_exit) {
    for(size_t i = 0; i < imv_navigator_length(imv->navigator); ++i)
      puts(imv_navigator_at(imv->navigator, i));
  }

  return 0;
}

static bool setup_window(struct imv *imv)
{
  if(SDL_Init(SDL_INIT_VIDEO) != 0) {
    imv_log(IMV_ERROR, "SDL Failed to Init: %s\n", SDL_GetError());
    return false;
  }

  /* register custom events */
  imv->events.NEW_IMAGE = SDL_RegisterEvents(1);
  imv->events.BAD_IMAGE = SDL_RegisterEvents(1);
  imv->events.NEW_PATH = SDL_RegisterEvents(1);
  imv->events.ENABLE_INPUT = SDL_RegisterEvents(1);

  imv->sdl_init = true;

  imv->window = SDL_CreateWindow(
        "imv",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        imv->initial_width, imv->initial_height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

  if(!imv->window) {
    imv_log(IMV_ERROR, "SDL Failed to create window: %s\n", SDL_GetError());
    return false;
  }

  /* we'll use SDL's built-in renderer, hardware accelerated if possible */
  imv->renderer = SDL_CreateRenderer(imv->window, -1, 0);
  if(!imv->renderer) {
    imv_log(IMV_ERROR, "SDL Failed to create renderer: %s\n", SDL_GetError());
    return false;
  }

  /* use the appropriate resampling method */
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
    imv->upscaling_method == UPSCALING_LINEAR? "1" : "0");

  /* allow fullscreen to be maintained even when focus is lost */
  SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS,
      imv->stay_fullscreen_on_focus_loss ? "0" : "1");

  /* construct a chequered background image */
  if(imv->background_type == BACKGROUND_CHEQUERED) {
    imv->background_image = create_chequered(imv->renderer);
  }

  /* set up the required fonts and surfaces for displaying the overlay */
  TTF_Init();
  imv->ttf_init = true;
  imv->font = load_font(imv->font_name);
  if(!imv->font) {
    imv_log(IMV_ERROR, "Error loading font: %s\n", TTF_GetError());
    return false;
  }

  imv->image = imv_image_create(imv->renderer);
  imv->view = imv_viewport_create(imv->window, imv->renderer);

  /* put us in fullscren mode to begin with if requested */
  if(imv->fullscreen) {
    imv_viewport_toggle_fullscreen(imv->view);
  }

  /* start outside of command mode */
  SDL_StopTextInput();

  return true;
}


static void handle_new_image(struct imv *imv, struct imv_bitmap *bitmap, int frametime)
{
  imv_image_set_bitmap(imv->image, bitmap);
  imv->current_image.width = bitmap->width;
  imv->current_image.height = bitmap->height;
  imv_bitmap_free(bitmap);
  imv->need_redraw = true;
  imv->need_rescale = true;
  /* If autoresizing on every image is enabled, make sure we do so */
  if (imv->resize_mode != RESIZE_NONE) {
    imv->need_resize = true;
  }
  if (imv->need_resize) {
    imv->need_resize = false;
    SDL_SetWindowSize(imv->window, imv->current_image.width, imv->current_image.height);
    if (imv->resize_mode == RESIZE_CENTER) {
      SDL_SetWindowPosition(imv->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
  }
  imv->loading = false;
  imv->next_frame_due = frametime ? SDL_GetTicks() + frametime : 0;
  imv->next_frame_duration = 0;

  /* If this is an animated image, we should kick off loading the next frame */
  if (imv->source && imv->source->load_next_frame && frametime) {
    async_load_next_frame(imv->source);
  }
}

static void handle_new_frame(struct imv *imv, struct imv_bitmap *bitmap, int frametime)
{
  if (imv->next_frame) {
    imv_bitmap_free(imv->next_frame);
  }
  imv->next_frame = bitmap;

  imv->next_frame_duration = frametime;
}

static void handle_event(struct imv *imv, SDL_Event *event)
{
  const int command_buffer_len = 1024;

  if (event->type == imv->events.NEW_IMAGE) {
    /* new image vs just a new frame of the same image */
    bool is_new_image = !!event->user.data2;
    if (is_new_image) {
      handle_new_image(imv, event->user.data1, event->user.code);
    } else {
      handle_new_frame(imv, event->user.data1, event->user.code);
    }
    return;
  } else if (event->type == imv->events.BAD_IMAGE) {
    /* an image failed to load, remove it from our image list */
    const char *err_path = imv_navigator_selection(imv->navigator);

    /* special case: the image came from stdin */
    if (strcmp(err_path, "-") == 0) {
      if (imv->stdin_image_data) {
        free(imv->stdin_image_data);
        imv->stdin_image_data = NULL;
        imv->stdin_image_data_len = 0;
      }
      imv_log(IMV_ERROR, "Failed to load image from stdin.\n");
    }

    imv_navigator_remove(imv->navigator, err_path);
    return;
  } else if (event->type == imv->events.NEW_PATH) {
    /* received a new path from the stdin reading thread */
    imv_add_path(imv, event->user.data1);
    free(event->user.data1);
    /* need to update image count */
    imv->need_redraw = true;
    return;
  } else if (event->type == imv->events.ENABLE_INPUT) {
    imv->ignore_window_events = false;
    return;
  } else if (imv->ignore_window_events) {
    /* Don't try and process this input event, we're in event ignoring mode */
    return;
  }

  switch (event->type) {
    case SDL_QUIT:
      imv_command_exec(imv->commands, "quit", imv);
      break;
    case SDL_TEXTINPUT:
      strncat(imv->input_buffer, event->text.text, command_buffer_len - 1);
      imv->need_redraw = true;
      break;

    case SDL_KEYDOWN:
      SDL_ShowCursor(SDL_DISABLE);

      if (imv->input_buffer) {
        /* in command mode, update the buffer */
        if (event->key.keysym.sym == SDLK_ESCAPE) {
          SDL_StopTextInput();
          free(imv->input_buffer);
          imv->input_buffer = NULL;
          imv->need_redraw = true;
        } else if (event->key.keysym.sym == SDLK_RETURN) {
          struct list *commands = list_create();
          list_append(commands, imv->input_buffer);
          imv_command_exec_list(imv->commands, commands, imv);
          SDL_StopTextInput();
          list_free(commands);
          free(imv->input_buffer);
          imv->input_buffer = NULL;
          imv->need_redraw = true;
        } else if (event->key.keysym.sym == SDLK_BACKSPACE) {
          const size_t len = strlen(imv->input_buffer);
          if (len > 0) {
            imv->input_buffer[len - 1] = '\0';
            imv->need_redraw = true;
          }
        }

        return;
      }

      /* Hitting : opens command-entry mode, like vim */
      if (event->key.keysym.sym == SDLK_SEMICOLON
          && event->key.keysym.mod & KMOD_SHIFT) {
        SDL_StartTextInput();
        imv->input_buffer = malloc(command_buffer_len);
        imv->input_buffer[0] = '\0';
        imv->need_redraw = true;
        return;
      }

      /* If none of the above, add the key to the current key sequence and
       * see if that triggers a bind */
      struct list *cmds = imv_bind_handle_event(imv->binds, event);
      if (cmds) {
        imv_command_exec_list(imv->commands, cmds, imv);
      }
      break;
    case SDL_MOUSEWHEEL:
      imv_viewport_zoom(imv->view, imv->image, IMV_ZOOM_MOUSE, event->wheel.y);
      SDL_ShowCursor(SDL_ENABLE);
      break;
    case SDL_MOUSEMOTION:
      if (event->motion.state & SDL_BUTTON_LMASK) {
        imv_viewport_move(imv->view, event->motion.xrel, event->motion.yrel, imv->image);
      }
      SDL_ShowCursor(SDL_ENABLE);
      break;
    case SDL_WINDOWEVENT:
      /* For some reason SDL passes events to us that occurred before we
       * gained focus, and passes them *after* the focus gained event.
       * Due to behavioural quirks from such events, whenever we gain focus
       * we have to ignore all window events already in the queue.
       */
      if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
        /* disable window event handling */
        imv->ignore_window_events = true;
        /* push an event to the back of the event queue to re-enable
         * window event handling */
        SDL_Event event;
        SDL_zero(event);
        event.type = imv->events.ENABLE_INPUT;
        SDL_PushEvent(&event);
      }

      imv_viewport_update(imv->view, imv->image);
      break;
  }
}

static void render_window(struct imv *imv)
{
  int ww, wh;
  SDL_GetWindowSize(imv->window, &ww, &wh);

  /* update window title */
  char title_text[1024];
  generate_env_text(imv, title_text, sizeof title_text, imv->title_text);
  imv_viewport_set_title(imv->view, title_text);

  /* first we draw the background */
  if(imv->background_type == BACKGROUND_SOLID) {
    /* solid background */
    SDL_SetRenderDrawColor(imv->renderer,
        imv->background_color.r,
        imv->background_color.g,
        imv->background_color.b,
        255);
    SDL_RenderClear(imv->renderer);
  } else {
    /* chequered background */
    int img_w, img_h;
    SDL_QueryTexture(imv->background_image, NULL, NULL, &img_w, &img_h);
    /* tile the image so it fills the window */
    for(int y = 0; y < wh; y += img_h) {
      for(int x = 0; x < ww; x += img_w) {
        SDL_Rect dst_rect = {x,y,img_w,img_h};
        SDL_RenderCopy(imv->renderer, imv->background_image, NULL, &dst_rect);
      }
    }
  }

  /* draw our actual image */
  {
    int x, y;
    double scale;
    imv_viewport_get_offset(imv->view, &x, &y);
    imv_viewport_get_scale(imv->view, &scale);
    imv_image_draw(imv->image, x, y, scale);
  }

  /* if the overlay needs to be drawn, draw that too */
  if(imv->overlay_enabled && imv->font) {
    SDL_Color fg = {255,255,255,255};
    SDL_Color bg = {0,0,0,160};
    char overlay_text[1024];
    generate_env_text(imv, overlay_text, sizeof overlay_text, imv->overlay_text);
    imv_printf(imv->renderer, imv->font, 0, 0, &fg, &bg, "%s", overlay_text);
  }

  /* draw command entry bar if needed */
  if(imv->input_buffer && imv->font) {
    SDL_Color fg = {255,255,255,255};
    SDL_Color bg = {0,0,0,160};
    imv_printf(imv->renderer,
        imv->font,
        0, wh - TTF_FontHeight(imv->font),
        &fg, &bg,
        ":%s", imv->input_buffer);
  }

  /* redraw complete, unset the flag */
  imv->need_redraw = false;
}

static char *get_config_path(void)
{
  const char *config_paths[] = {
    "$imv_config",
    "$XDG_CONFIG_HOME/imv/config",
    "$HOME/.config/imv/config",
    "$HOME/.imv_config",
    "$HOME/.imv/config",
    "/usr/local/etc/imv_config",
    "/etc/imv_config",
  };

  for(size_t i = 0; i < sizeof(config_paths) / sizeof(char*); ++i) {
    wordexp_t word;
    if(wordexp(config_paths[i], &word, 0) == 0) {
      if (!word.we_wordv[0]) {
        wordfree(&word);
        continue;
      }

      char *path = strdup(word.we_wordv[0]);
      wordfree(&word);

      if(!path || access(path, R_OK) == -1) {
        free(path);
        continue;
      }

      return path;
    }
  }
  return NULL;
}

static bool parse_bool(const char *str)
{
  return (
    !strcmp(str, "1") ||
    !strcmp(str, "yes") ||
    !strcmp(str, "true") ||
    !strcmp(str, "on")
  );
}

static int handle_ini_value(void *user, const char *section, const char *name,
                            const char *value)
{
  struct imv *imv = user;

  if (!strcmp(section, "binds")) {
    return add_bind(imv, name, value);
  }

  if (!strcmp(section, "aliases")) {
    imv_command_alias(imv->commands, name, value);
    return 1;
  }

  if (!strcmp(section, "options")) {

    if(!strcmp(name, "fullscreen")) {
      imv->fullscreen = parse_bool(value);
      return 1;
    }

    if(!strcmp(name, "width")) {
      imv->initial_width = strtol(value, NULL, 10);
      return 1;
    }
    if(!strcmp(name, "height")) {
      imv->initial_height = strtol(value, NULL, 10);
      return 1;
    }

    if(!strcmp(name, "overlay")) {
      imv->overlay_enabled = parse_bool(value);
      return 1;
    }

    if(!strcmp(name, "autoresize")) {
      return parse_resizing_mode(imv, value);
    }

    if(!strcmp(name, "upscaling_method")) {
      return parse_upscaling_method(imv, value);
    }

    if(!strcmp(name, "stay_fullscreen_on_focus_loss")) {
      imv->stay_fullscreen_on_focus_loss = parse_bool(value);
      return 1;
    }

    if(!strcmp(name, "recursive")) {
      imv->recursive_load = parse_bool(value);
      return 1;
    }

    if(!strcmp(name, "loop_input")) {
      imv->loop_input = parse_bool(value);
      return 1;
    }

    if(!strcmp(name, "list_files_at_exit")) {
      imv->list_files_at_exit = parse_bool(value);
      return 1;
    }

    if(!strcmp(name, "scaling_mode")) {
      return parse_scaling_mode(imv, value);
    }

    if(!strcmp(name, "background")) {
      if(!parse_bg(imv, value)) {
        return false;
      }
      return 1;
    }

    if(!strcmp(name, "slideshow_duration")) {
      if(!parse_slideshow_duration(imv, value)) {
        return false;
      }
      return 1;
    }

    if(!strcmp(name, "overlay_font")) {
      free(imv->font_name);
      imv->font_name = strdup(value);
      return 1;
    }

    if(!strcmp(name, "overlay_text")) {
      free(imv->overlay_text);
      imv->overlay_text = strdup(value);
      return 1;
    }

    if(!strcmp(name, "title_text")) {
      free(imv->title_text);
      imv->title_text = strdup(value);
      return 1;
    }

    if(!strcmp(name, "suppress_default_binds")) {
      const bool suppress_default_binds = parse_bool(value);
      if(suppress_default_binds) {
        /* clear out any default binds if requested */
        imv_binds_clear(imv->binds);
      }
      return 1;
    }

    /* No matches so far */
    imv_log(IMV_WARNING, "Ignoring unknown option: %s\n", name);
    return 1;
  }
  return 0;
}

bool imv_load_config(struct imv *imv)
{
  char *path = get_config_path();
  if(!path) {
    /* no config, no problem - we have defaults */
    return true;
  }

  const int err = ini_parse(path, handle_ini_value, imv);
  if (err == -1) {
    imv_log(IMV_ERROR, "Unable to open config file: %s\n", path);
    return false;
  } else if (err > 0) {
    imv_log(IMV_ERROR, "Error in config file: %s:%d\n", path, err);
    return false;
  }
  free(path);
  return true;
}

void command_quit(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;
  imv->quit = true;
}

void command_pan(struct list *args, const char *argstr, void *data)
{
  (void)argstr;
  struct imv *imv = data;
  if(args->len != 3) {
    return;
  }

  long int x = strtol(args->items[1], NULL, 10);
  long int y = strtol(args->items[2], NULL, 10);

  imv_viewport_move(imv->view, x, y, imv->image);
}

void command_select_rel(struct list *args, const char *argstr, void *data)
{
  (void)argstr;
  struct imv *imv = data;
  if(args->len != 2) {
    return;
  }

  long int index = strtol(args->items[1], NULL, 10);
  imv_navigator_select_rel(imv->navigator, index);

  imv->slideshow_time_elapsed = 0;
}

void command_select_abs(struct list *args, const char *argstr, void *data)
{
  (void)argstr;
  struct imv *imv = data;
  if(args->len != 2) {
    return;
  }

  long int index = strtol(args->items[1], NULL, 10);
  imv_navigator_select_abs(imv->navigator, index);

  imv->slideshow_time_elapsed = 0;
}

void command_zoom(struct list *args, const char *argstr, void *data)
{
  (void)argstr;
  struct imv *imv = data;
  if(args->len == 2) {
    const char *str = args->items[1];
    if(!strcmp(str, "actual")) {
      imv_viewport_scale_to_actual(imv->view, imv->image);
    } else {
      long int amount = strtol(args->items[1], NULL, 10);
      imv_viewport_zoom(imv->view, imv->image, IMV_ZOOM_KEYBOARD, amount);
    }
  }
}

void command_open(struct list *args, const char *argstr, void *data)
{
  (void)argstr;
  struct imv *imv = data;
  bool recursive = imv->recursive_load;

  update_env_vars(imv);
  for (size_t i = 1; i < args->len; ++i) {

    /* allow -r arg to specify recursive */
    if (i == 1 && !strcmp(args->items[i], "-r")) {
      recursive = true;
      continue;
    }

    wordexp_t word;
    if(wordexp(args->items[i], &word, 0) == 0) {
      for(size_t j = 0; j < word.we_wordc; ++j) {
        imv_navigator_add(imv->navigator, word.we_wordv[j], recursive);
      }
      wordfree(&word);
    }
  }
}

void command_close(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;
  char* path = strdup(imv_navigator_selection(imv->navigator));
  imv_navigator_remove(imv->navigator, path);
  free(path);

  imv->slideshow_time_elapsed = 0;
}

void command_fullscreen(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;
  imv_viewport_toggle_fullscreen(imv->view);
}

void command_overlay(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;
  imv->overlay_enabled = !imv->overlay_enabled;
  imv->need_redraw = true;
}

void command_exec(struct list *args, const char *argstr, void *data)
{
  (void)args;
  struct imv *imv = data;
  update_env_vars(imv);
  system(argstr);
}

void command_center(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;
  imv_viewport_center(imv->view, imv->image);
}

void command_top(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;
  imv_viewport_top(imv->view, imv->image);
}

void command_bottom(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;
  imv_viewport_bottom(imv->view, imv->image);
}

void command_reset(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;
  imv->need_rescale = true;
  imv->need_redraw = true;
}

void command_next_frame(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;
  if (imv->source && imv->source->load_next_frame) {
    async_load_next_frame(imv->source);
    imv->next_frame_due = 1; /* Earliest possible non-zero timestamp */
  }
}

void command_toggle_playing(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;
  imv_viewport_toggle_playing(imv->view);
}

void command_set_scaling_mode(struct list *args, const char *argstr, void *data)
{
  (void)args;
  (void)argstr;
  struct imv *imv = data;

  if(args->len != 2) {
    return;
  }

  const char *mode = args->items[1];

  if(!strcmp(mode, "next")) {
    imv->scaling_mode++;
    imv->scaling_mode %= SCALING_MODE_COUNT;
  } else if(!strcmp(mode, "none")) {
    imv->scaling_mode = SCALING_NONE;
  } else if(!strcmp(mode, "shrink")) {
    imv->scaling_mode = SCALING_DOWN;
  } else if(!strcmp(mode, "full")) {
    imv->scaling_mode = SCALING_FULL;
  } else {
    /* no changes, don't bother to redraw */
    return;
  }

  imv->need_rescale = true;
  imv->need_redraw = true;
}

void command_set_slideshow_duration(struct list *args, const char *argstr, void *data)
{
  (void)argstr;
  struct imv *imv = data;
  if(args->len == 2) {
    long int delta = 1000 * strtol(args->items[1], NULL, 10);

    /* Ensure we can't go below 0 */
    if(delta < 0 && (size_t)labs(delta) > imv->slideshow_image_duration) {
      imv->slideshow_image_duration = 0;
    } else {
      imv->slideshow_image_duration += delta;
    }

    imv->need_redraw = true;
  }
}

static void update_env_vars(struct imv *imv)
{
  char str[64];

  setenv("imv_current_file", imv_navigator_selection(imv->navigator), 1);
  setenv("imv_scaling_mode", scaling_label[imv->scaling_mode], 1);
  setenv("imv_loading", imv->loading ? "1" : "0", 1);

  snprintf(str, sizeof str, "%zu", imv_navigator_index(imv->navigator) + 1);
  setenv("imv_current_index", str, 1);

  snprintf(str, sizeof str, "%zu", imv_navigator_length(imv->navigator));
  setenv("imv_file_count", str, 1);

  snprintf(str, sizeof str, "%d", imv_image_width(imv->image));
  setenv("imv_width", str, 1);

  snprintf(str, sizeof str, "%d", imv_image_height(imv->image));
  setenv("imv_height", str, 1);

  {
    double scale;
    imv_viewport_get_scale(imv->view, &scale);
    snprintf(str, sizeof str, "%d", (int)(scale * 100.0));
    setenv("imv_scale", str, 1);
  }

  snprintf(str, sizeof str, "%zu", imv->slideshow_image_duration / 1000);
  setenv("imv_slidshow_duration", str, 1);

  snprintf(str, sizeof str, "%zu", imv->slideshow_time_elapsed / 1000);
  setenv("imv_slidshow_elapsed", str, 1);
}

static size_t generate_env_text(struct imv *imv, char *buf, size_t buf_len, const char *format)
{
  update_env_vars(imv);

  size_t len = 0;
  wordexp_t word;
  if(wordexp(format, &word, 0) == 0) {
    for(size_t i = 0; i < word.we_wordc; ++i) {
      len += snprintf(buf + len, buf_len - len, "%s ", word.we_wordv[i]);
    }
    wordfree(&word);
  } else {
    len += snprintf(buf, buf_len, "error expanding text");
  }

  return len;
}

/* vim:set ts=2 sts=2 sw=2 et: */

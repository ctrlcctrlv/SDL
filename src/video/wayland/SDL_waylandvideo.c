/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2021 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_WAYLAND

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_stdinc.h"
#include "../../events/SDL_events_c.h"

#include "SDL_waylandvideo.h"
#include "SDL_waylandevents_c.h"
#include "SDL_waylandwindow.h"
#include "SDL_waylandopengles.h"
#include "SDL_waylandmouse.h"
#include "SDL_waylandkeyboard.h"
#include "SDL_waylandtouch.h"
#include "SDL_waylandclipboard.h"
#include "SDL_waylandvulkan.h"

#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <xkbcommon/xkbcommon.h>

#include "SDL_waylanddyn.h"
#include <wayland-util.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-shell-unstable-v6-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "org-kde-kwin-server-decoration-manager-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

#define WAYLANDVID_DRIVER_NAME "wayland"

/* Initialization/Query functions */
static int
Wayland_VideoInit(_THIS);

static void
Wayland_GetDisplayModes(_THIS, SDL_VideoDisplay *sdl_display);
static int
Wayland_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode);

static void
Wayland_VideoQuit(_THIS);

/* Find out what class name we should use
 * Based on src/video/x11/SDL_x11video.c */
static char *
get_classname()
{
/* !!! FIXME: this is probably wrong, albeit harmless in many common cases. From protocol spec:
    "The surface class identifies the general class of applications
    to which the surface belongs. A common convention is to use the
    file name (or the full path if it is a non-standard location) of
    the application's .desktop file as the class." */

    char *spot;
#if defined(__LINUX__) || defined(__FREEBSD__)
    char procfile[1024];
    char linkfile[1024];
    int linksize;
#endif

    /* First allow environment variable override */
    spot = SDL_getenv("SDL_VIDEO_WAYLAND_WMCLASS");
    if (spot) {
        return SDL_strdup(spot);
    } else {
        /* Fallback to the "old" envvar */
        spot = SDL_getenv("SDL_VIDEO_X11_WMCLASS");
        if (spot) {
            return SDL_strdup(spot);
        }
    }

    /* Next look at the application's executable name */
#if defined(__LINUX__) || defined(__FREEBSD__)
#if defined(__LINUX__)
    SDL_snprintf(procfile, SDL_arraysize(procfile), "/proc/%d/exe", getpid());
#elif defined(__FREEBSD__)
    SDL_snprintf(procfile, SDL_arraysize(procfile), "/proc/%d/file",
                 getpid());
#else
#error Where can we find the executable name?
#endif
    linksize = readlink(procfile, linkfile, sizeof(linkfile) - 1);
    if (linksize > 0) {
        linkfile[linksize] = '\0';
        spot = SDL_strrchr(linkfile, '/');
        if (spot) {
            return SDL_strdup(spot + 1);
        } else {
            return SDL_strdup(linkfile);
        }
    }
#endif /* __LINUX__ || __FREEBSD__ */

    /* Finally use the default we've used forever */
    return SDL_strdup("SDL_App");
}

static void
Wayland_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_VideoData *data = (SDL_VideoData *)device->driverdata;
    if (data->display) {
        WAYLAND_wl_display_flush(data->display);
        WAYLAND_wl_display_disconnect(data->display);
    }
    SDL_free(data);
    SDL_free(device);
    SDL_WAYLAND_UnloadSymbols();
}

static SDL_VideoDevice *
Wayland_CreateDevice(int devindex)
{
    SDL_VideoDevice *device;
    SDL_VideoData *data;
    struct wl_display *display;

    if (!SDL_WAYLAND_LoadSymbols()) {
        return NULL;
    }

    display = WAYLAND_wl_display_connect(NULL);
    if (display == NULL) {
        SDL_WAYLAND_UnloadSymbols();
        return NULL;
    }

    data = SDL_calloc(1, sizeof(*data));
    if (data == NULL) {
        WAYLAND_wl_display_disconnect(display);
        SDL_WAYLAND_UnloadSymbols();
        SDL_OutOfMemory();
        return NULL;
    }

    data->display = display;

    /* Initialize all variables that we clean on shutdown */
    device = SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_free(data);
        WAYLAND_wl_display_disconnect(display);
        SDL_WAYLAND_UnloadSymbols();
        SDL_OutOfMemory();
        return NULL;
    }

    device->driverdata = data;

    /* Set the function pointers */
    device->VideoInit = Wayland_VideoInit;
    device->VideoQuit = Wayland_VideoQuit;
    device->SetDisplayMode = Wayland_SetDisplayMode;
    device->GetDisplayModes = Wayland_GetDisplayModes;
    device->GetWindowWMInfo = Wayland_GetWindowWMInfo;
    device->SuspendScreenSaver = Wayland_SuspendScreenSaver;

    device->PumpEvents = Wayland_PumpEvents;

    device->GL_SwapWindow = Wayland_GLES_SwapWindow;
    device->GL_GetSwapInterval = Wayland_GLES_GetSwapInterval;
    device->GL_SetSwapInterval = Wayland_GLES_SetSwapInterval;
    device->GL_GetDrawableSize = Wayland_GLES_GetDrawableSize;
    device->GL_MakeCurrent = Wayland_GLES_MakeCurrent;
    device->GL_CreateContext = Wayland_GLES_CreateContext;
    device->GL_LoadLibrary = Wayland_GLES_LoadLibrary;
    device->GL_UnloadLibrary = Wayland_GLES_UnloadLibrary;
    device->GL_GetProcAddress = Wayland_GLES_GetProcAddress;
    device->GL_DeleteContext = Wayland_GLES_DeleteContext;

    device->CreateSDLWindow = Wayland_CreateWindow;
    device->ShowWindow = Wayland_ShowWindow;
    device->SetWindowFullscreen = Wayland_SetWindowFullscreen;
    device->MaximizeWindow = Wayland_MaximizeWindow;
    device->MinimizeWindow = Wayland_MinimizeWindow;
    device->SetWindowMouseGrab = Wayland_SetWindowMouseGrab;
    device->SetWindowKeyboardGrab = Wayland_SetWindowKeyboardGrab;
    device->RestoreWindow = Wayland_RestoreWindow;
    device->SetWindowBordered = Wayland_SetWindowBordered;
    device->SetWindowResizable = Wayland_SetWindowResizable;
    device->SetWindowSize = Wayland_SetWindowSize;
    device->SetWindowMinimumSize = Wayland_SetWindowMinimumSize;
    device->SetWindowMaximumSize = Wayland_SetWindowMaximumSize;
    device->SetWindowTitle = Wayland_SetWindowTitle;
    device->DestroyWindow = Wayland_DestroyWindow;
    device->SetWindowHitTest = Wayland_SetWindowHitTest;

    device->SetClipboardText = Wayland_SetClipboardText;
    device->GetClipboardText = Wayland_GetClipboardText;
    device->HasClipboardText = Wayland_HasClipboardText;
    device->StartTextInput = Wayland_StartTextInput;
    device->StopTextInput = Wayland_StopTextInput;
    device->SetTextInputRect = Wayland_SetTextInputRect;

#if SDL_VIDEO_VULKAN
    device->Vulkan_LoadLibrary = Wayland_Vulkan_LoadLibrary;
    device->Vulkan_UnloadLibrary = Wayland_Vulkan_UnloadLibrary;
    device->Vulkan_GetInstanceExtensions = Wayland_Vulkan_GetInstanceExtensions;
    device->Vulkan_CreateSurface = Wayland_Vulkan_CreateSurface;
    device->Vulkan_GetDrawableSize = Wayland_Vulkan_GetDrawableSize;
#endif

    device->free = Wayland_DeleteDevice;

    return device;
}

VideoBootStrap Wayland_bootstrap = {
    WAYLANDVID_DRIVER_NAME, "SDL Wayland video driver",
    Wayland_CreateDevice
};

static void
display_handle_geometry(void *data,
                        struct wl_output *output,
                        int x, int y,
                        int physical_width,
                        int physical_height,
                        int subpixel,
                        const char *make,
                        const char *model,
                        int transform)

{
    SDL_VideoDisplay *display = data;

    display->name = SDL_strdup(model);
    ((SDL_WaylandOutputData*)display->driverdata)->transform = transform;
}

static void
display_handle_mode(void *data,
                    struct wl_output *output,
                    uint32_t flags,
                    int width,
                    int height,
                    int refresh)
{
    SDL_VideoDisplay *display = data;
    SDL_WaylandOutputData* driverdata = display->driverdata;

    if (flags & WL_OUTPUT_MODE_CURRENT) {
        driverdata->width = width;
        driverdata->height = height;
        driverdata->refresh = refresh;
    }
}

static void
display_handle_done(void *data,
                    struct wl_output *output)
{
    SDL_VideoDisplay *display = data;
    SDL_WaylandOutputData* driverdata = display->driverdata;
    SDL_DisplayMode mode;

    if (driverdata->done)
        return;

    driverdata->done = SDL_TRUE;

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB888;
    mode.w = driverdata->width / driverdata->scale_factor;
    mode.h = driverdata->height / driverdata->scale_factor;
    if (driverdata->transform & WL_OUTPUT_TRANSFORM_90) {
       mode.w = driverdata->height / driverdata->scale_factor;
       mode.h = driverdata->width / driverdata->scale_factor;
    }
    mode.refresh_rate = driverdata->refresh / 1000; // mHz to Hz
    mode.driverdata = driverdata->output;
    SDL_AddDisplayMode(display, &mode);
    display->current_mode = mode;
    display->desktop_mode = mode;

    SDL_AddVideoDisplay(display, SDL_FALSE);
    SDL_free(display->name);
}

static void
display_handle_scale(void *data,
                     struct wl_output *output,
                     int32_t factor)
{
    SDL_VideoDisplay *display = data;
    ((SDL_WaylandOutputData*)display->driverdata)->scale_factor = factor;
}

static const struct wl_output_listener output_listener = {
    display_handle_geometry,
    display_handle_mode,
    display_handle_done,
    display_handle_scale
};

static void
Wayland_add_display(SDL_VideoData *d, uint32_t id)
{
    struct wl_output *output;
    SDL_WaylandOutputData *data;
    SDL_VideoDisplay *display = SDL_malloc(sizeof *display);
    if (!display) {
        SDL_OutOfMemory();
        return;
    }
    SDL_zero(*display);

    output = wl_registry_bind(d->registry, id, &wl_output_interface, 2);
    if (!output) {
        SDL_SetError("Failed to retrieve output.");
        SDL_free(display);
        return;
    }
    data = SDL_malloc(sizeof *data);
    data->output = output;
    data->scale_factor = 1.0;
    data->done = SDL_FALSE;
    display->driverdata = data;

    wl_output_add_listener(output, &output_listener, display);
}

#ifdef SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH
static void
windowmanager_hints(void *data, struct qt_windowmanager *qt_windowmanager,
        int32_t show_is_fullscreen)
{
}

static void
windowmanager_quit(void *data, struct qt_windowmanager *qt_windowmanager)
{
    SDL_SendQuit();
}

static const struct qt_windowmanager_listener windowmanager_listener = {
    windowmanager_hints,
    windowmanager_quit,
};
#endif /* SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH */


static void
handle_ping_zxdg_shell(void *data, struct zxdg_shell_v6 *zxdg, uint32_t serial)
{
    zxdg_shell_v6_pong(zxdg, serial);
}

static const struct zxdg_shell_v6_listener shell_listener_zxdg = {
    handle_ping_zxdg_shell
};


static void
handle_ping_xdg_wm_base(void *data, struct xdg_wm_base *xdg, uint32_t serial)
{
    xdg_wm_base_pong(xdg, serial);
}

static const struct xdg_wm_base_listener shell_listener_xdg = {
    handle_ping_xdg_wm_base
};


static void
display_handle_global(void *data, struct wl_registry *registry, uint32_t id,
                      const char *interface, uint32_t version)
{
    SDL_VideoData *d = data;

    /*printf("WAYLAND INTERFACE: %s\n", interface);*/

    if (strcmp(interface, "wl_compositor") == 0) {
        d->compositor = wl_registry_bind(d->registry, id, &wl_compositor_interface, SDL_min(3, version));
    } else if (strcmp(interface, "wl_output") == 0) {
        Wayland_add_display(d, id);
    } else if (strcmp(interface, "wl_seat") == 0) {
        Wayland_display_add_input(d, id, version);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        d->shell.xdg = wl_registry_bind(d->registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(d->shell.xdg, &shell_listener_xdg, NULL);
    } else if (strcmp(interface, "zxdg_shell_v6") == 0) {
        d->shell.zxdg = wl_registry_bind(d->registry, id, &zxdg_shell_v6_interface, 1);
        zxdg_shell_v6_add_listener(d->shell.zxdg, &shell_listener_zxdg, NULL);
    } else if (strcmp(interface, "wl_shell") == 0) {
        d->shell.wl = wl_registry_bind(d->registry, id, &wl_shell_interface, 1);
    } else if (strcmp(interface, "wl_shm") == 0) {
        d->shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
        d->cursor_theme = WAYLAND_wl_cursor_theme_load(NULL, 32, d->shm);
    } else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0) {
        Wayland_display_add_relative_pointer_manager(d, id);
    } else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0) {
        Wayland_display_add_pointer_constraints(d, id);
    } else if (strcmp(interface, "zwp_keyboard_shortcuts_inhibit_manager_v1") == 0) {
        d->key_inhibitor_manager = wl_registry_bind(d->registry, id, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface, "zwp_idle_inhibit_manager_v1") == 0) {
        d->idle_inhibit_manager = wl_registry_bind(d->registry, id, &zwp_idle_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface, "wl_data_device_manager") == 0) {
        d->data_device_manager = wl_registry_bind(d->registry, id, &wl_data_device_manager_interface, SDL_min(3, version));
    } else if (strcmp(interface, "zxdg_decoration_manager_v1") == 0) {
        d->decoration_manager = wl_registry_bind(d->registry, id, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, "org_kde_kwin_server_decoration_manager") == 0) {
        d->kwin_server_decoration_manager = wl_registry_bind(d->registry, id, &org_kde_kwin_server_decoration_manager_interface, 1);

#ifdef SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH
    } else if (strcmp(interface, "qt_touch_extension") == 0) {
        Wayland_touch_create(d, id);
    } else if (strcmp(interface, "qt_surface_extension") == 0) {
        d->surface_extension = wl_registry_bind(registry, id,
                &qt_surface_extension_interface, 1);
    } else if (strcmp(interface, "qt_windowmanager") == 0) {
        d->windowmanager = wl_registry_bind(registry, id,
                &qt_windowmanager_interface, 1);
        qt_windowmanager_add_listener(d->windowmanager, &windowmanager_listener, d);
#endif /* SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH */
    }
}

static void
display_remove_global(void *data, struct wl_registry *registry, uint32_t id) {}

static const struct wl_registry_listener registry_listener = {
    display_handle_global,
    display_remove_global
};

int
Wayland_VideoInit(_THIS)
{
    SDL_VideoData *data = (SDL_VideoData*)_this->driverdata;

    data->xkb_context = WAYLAND_xkb_context_new(0);
    if (!data->xkb_context) {
        return SDL_SetError("Failed to create XKB context");
    }

    data->registry = wl_display_get_registry(data->display);
    if (data->registry == NULL) {
        return SDL_SetError("Failed to get the Wayland registry");
    }

    wl_registry_add_listener(data->registry, &registry_listener, data);

    // First roundtrip to receive all registry objects.
    WAYLAND_wl_display_roundtrip(data->display);

    // Second roundtrip to receive all output events.
    WAYLAND_wl_display_roundtrip(data->display);

    Wayland_InitMouse();

    /* Get the surface class name, usually the name of the application */
    data->classname = get_classname();

    WAYLAND_wl_display_flush(data->display);

    Wayland_InitKeyboard(_this);

#if SDL_USE_LIBDBUS
    SDL_DBus_Init();
#endif

    return 0;
}

static void
Wayland_GetDisplayModes(_THIS, SDL_VideoDisplay *sdl_display)
{
    // Nothing to do here, everything was already done in the wl_output
    // callbacks.
}

static int
Wayland_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    return SDL_Unsupported();
}

void
Wayland_VideoQuit(_THIS)
{
    SDL_VideoData *data = _this->driverdata;
    int i, j;

    Wayland_FiniMouse ();

    for (i = 0; i < _this->num_displays; ++i) {
        SDL_VideoDisplay *display = &_this->displays[i];

        wl_output_destroy(((SDL_WaylandOutputData*)display->driverdata)->output);
        SDL_free(display->driverdata);
        display->driverdata = NULL;

        for (j = display->num_display_modes; j--;) {
            display->display_modes[j].driverdata = NULL;
        }
        display->desktop_mode.driverdata = NULL;
    }

    Wayland_display_destroy_input(data);
    Wayland_display_destroy_pointer_constraints(data);
    Wayland_display_destroy_relative_pointer_manager(data);

    if (data->idle_inhibit_manager)
        zwp_idle_inhibit_manager_v1_destroy(data->idle_inhibit_manager);

    if (data->key_inhibitor_manager)
        zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(data->key_inhibitor_manager);

    if (data->xkb_context) {
        WAYLAND_xkb_context_unref(data->xkb_context);
        data->xkb_context = NULL;
    }
#ifdef SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH
    if (data->windowmanager)
        qt_windowmanager_destroy(data->windowmanager);

    if (data->surface_extension)
        qt_surface_extension_destroy(data->surface_extension);

    Wayland_touch_destroy(data);
#endif /* SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH */

    if (data->data_device_manager)
        wl_data_device_manager_destroy(data->data_device_manager);

    if (data->shm)
        wl_shm_destroy(data->shm);

    if (data->cursor_theme)
        WAYLAND_wl_cursor_theme_destroy(data->cursor_theme);

    if (data->shell.wl)
        wl_shell_destroy(data->shell.wl);

    if (data->shell.xdg)
        xdg_wm_base_destroy(data->shell.xdg);

    if (data->shell.zxdg)
        zxdg_shell_v6_destroy(data->shell.zxdg);

    if (data->compositor)
        wl_compositor_destroy(data->compositor);

    if (data->registry)
        wl_registry_destroy(data->registry);

    Wayland_QuitKeyboard(_this);

    SDL_free(data->classname);
}

#endif /* SDL_VIDEO_DRIVER_WAYLAND */

/* vi: set ts=4 sw=4 expandtab: */

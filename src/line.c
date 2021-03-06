#include "config.h"

#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <i3ipc-glib/i3ipc-glib.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

static xcb_connection_t* conn = NULL;
static xcb_screen_t* screen = NULL;
static xcb_colormap_t colormap;
static xcb_gcontext_t black_fill;
static xcb_gcontext_t white_fill;
static xcb_gcontext_t urgent_fill;
static xcb_gcontext_t focus_fill;
static xcb_gcontext_t active_fill;
static xcb_gcontext_t inactive_fill;
static xcb_gcontext_t off_fill;
static xcb_gcontext_t volume_fill;

#include <alsa/asoundlib.h>
#include <stdio.h>

#define MAX_SND_CARDS 256

typedef struct {
    xcb_window_t win;
    char* output;
    uint16_t width;
} line_window_t;

static line_window_t* line_windows = NULL;
static int line_windows_len = 0;

enum {
#define ATOM_DO(name) name,
#include "xcb_atoms.def"
    NUM_ATOMS
#undef ATOM_DO
};

static xcb_atom_t atoms[NUM_ATOMS];

static void request_atoms(void)
{
    xcb_intern_atom_cookie_t atom_cookies[NUM_ATOMS];
    xcb_intern_atom_reply_t* reply;

#define ATOM_DO(name) atom_cookies[name] = xcb_intern_atom(conn, 0, strlen(#name), #name);
#include "xcb_atoms.def"
#undef ATOM_DO

#define ATOM_DO(name)                                              \
    reply = xcb_intern_atom_reply(conn, atom_cookies[name], NULL); \
    if (reply == NULL) {                                           \
        fprintf(stderr, "Could not get atom %s\n", #name);         \
        exit(EXIT_FAILURE);                                        \
    }                                                              \
    atoms[name] = reply->atom;                                     \
    free(reply);

#include "xcb_atoms.def"
#undef ATOM_DO
}

static xcb_gcontext_t get_context(uint32_t fg_color, uint32_t bg_color)
{
    xcb_gcontext_t foreground = xcb_generate_id(conn);

    uint32_t mask;
    uint32_t values[2];
    mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
    values[0] = fg_color;
    values[1] = bg_color;
    xcb_create_gc(conn, foreground, screen->root, mask, values);

    return foreground;
}

static uint32_t color_pixel(unsigned short r, unsigned short g, unsigned short b)
{
    xcb_alloc_color_reply_t* reply = xcb_alloc_color_reply(conn, xcb_alloc_color(conn, colormap, r, g, b), NULL);
    uint32_t pixel = reply->pixel;
    free(reply);

    return pixel;
}

static void init_xcb(void)
{
    conn = xcb_connect(NULL, NULL);
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;

    colormap = xcb_generate_id(conn);
    xcb_create_colormap(conn, XCB_COLORMAP_ALLOC_NONE, colormap, screen->root, screen->root_visual);

    black_fill = get_context(screen->black_pixel, screen->white_pixel);
    white_fill = get_context(screen->white_pixel, screen->white_pixel);
    urgent_fill = get_context(color_pixel(URGENT_COLOR), screen->white_pixel);
    active_fill = get_context(color_pixel(ACTIVE_COLOR), screen->white_pixel);
    focus_fill = get_context(color_pixel(FOCUS_COLOR), screen->white_pixel);
    inactive_fill = get_context(color_pixel(INACTIVE_COLOR), screen->white_pixel);
    off_fill = get_context(color_pixel(OFF_COLOR), screen->white_pixel);
    volume_fill = get_context(color_pixel(VOLUME_COLOR), screen->white_pixel);

    request_atoms();
}

static void free_xcb(void)
{
    xcb_free_gc(conn, black_fill);
    xcb_free_gc(conn, white_fill);
    xcb_free_gc(conn, urgent_fill);
    xcb_free_gc(conn, active_fill);
    xcb_free_gc(conn, focus_fill);
    xcb_free_gc(conn, inactive_fill);
    xcb_free_gc(conn, off_fill);
    xcb_free_gc(conn, volume_fill);
    xcb_free_colormap(conn, colormap);
    xcb_disconnect(conn);
    conn = NULL;
    screen = NULL;
}

static xcb_window_t create_dock_window(xcb_randr_monitor_info_t* monitor)
{
    xcb_window_t win = xcb_generate_id(conn);

    uint32_t mask;
    uint32_t values[1];
    mask = XCB_CW_BACK_PIXEL;
    values[0] = screen->white_pixel;
    xcb_create_window(conn,
        XCB_COPY_FROM_PARENT,
        win,
        screen->root,
        monitor->x, monitor->y,
        monitor->width, line_height,
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,
        mask, values);

    xcb_change_property(conn,
        XCB_PROP_MODE_APPEND,
        win,
        atoms[_NET_WM_WINDOW_TYPE],
        XCB_ATOM_ATOM,
        32,
        1,
        &atoms[_NET_WM_WINDOW_TYPE_DOCK]);

    xcb_map_window(conn, win);

    xcb_flush(conn);

    return win;
}

static void create_line_windows(void)
{
    xcb_randr_get_monitors_reply_t* monitors = xcb_randr_get_monitors_reply(conn, xcb_randr_get_monitors(conn, screen->root, 1), NULL);

    int monitors_len = 0;
    xcb_randr_monitor_info_iterator_t iter;
    for (iter = xcb_randr_get_monitors_monitors_iterator(monitors); iter.rem; xcb_randr_monitor_info_next(&iter)) {
        monitors_len++;
    }

    line_window_t* wins = malloc(sizeof(line_window_t) * monitors_len);
    int idx = 0;
    for (iter = xcb_randr_get_monitors_monitors_iterator(monitors);
         iter.rem;
         xcb_randr_monitor_info_next(&iter)) {
        xcb_randr_monitor_info_t* monitor = iter.data;
        xcb_get_atom_name_reply_t* atom_reply = xcb_get_atom_name_reply(conn, xcb_get_atom_name(conn, monitor->name), NULL);
        char* name;
        asprintf(&name, "%.*s", xcb_get_atom_name_name_length(atom_reply), xcb_get_atom_name_name(atom_reply));

        xcb_window_t win = create_dock_window(monitor);
        line_window_t line_win = { win, name, monitor->width };
        wins[idx] = line_win;

        free(atom_reply);

        idx++;
    }

    free(monitors);

    line_windows = wins;
    line_windows_len = monitors_len;
}

static void free_line_windows(void)
{
    for (int i = 0; i < line_windows_len; i++) {
        line_window_t line = line_windows[i];
        xcb_destroy_window(conn, line.win);
        free(line.output);
    }

    free(line_windows);
    line_windows = NULL;
    line_windows_len = 0;
}

static void draw_workspaces(GSList* replies, xcb_drawable_t drawable, line_window_t line)
{
    const GSList* reply;
    i3ipcWorkspaceReply* ws;
    int max_ws_num = INT_MIN, min_ws_num = INT_MAX;
    for (reply = replies; reply; reply = reply->next) {
        ws = reply->data;
        if (strcmp(ws->output, line.output) != 0)
            continue;
        if (ws->num > max_ws_num)
            max_ws_num = ws->num;
        if (ws->num < min_ws_num)
            min_ws_num = ws->num;
    }

    int num_ws = max_ws_num - min_ws_num + 1;
    if (num_ws > 0) {
        xcb_rectangle_t rectangles[num_ws];
        int width = line.width / num_ws;
        int curr_pos = 0;
        for (int curr_num = 0; curr_num < num_ws; curr_num++) {
            int curr_width = (curr_num == num_ws - 1) ? line.width - curr_pos : width;
            xcb_rectangle_t rect = { curr_pos, 0, curr_width, line_height };
            rectangles[curr_num] = rect;
            curr_pos += curr_width + border_width;
        }

        xcb_poly_fill_rectangle(conn, drawable, off_fill, num_ws, rectangles);

        for (reply = replies; reply; reply = reply->next) {
            ws = reply->data;
            if (strcmp(ws->output, line.output) != 0)
                continue;

            if (ws->num >= min_ws_num && ws->num <= max_ws_num) {
                xcb_gcontext_t foreground;
                if (ws->urgent)
                    foreground = urgent_fill;
                else if (ws->focused)
                    foreground = focus_fill;
                else if (ws->visible)
                    foreground = active_fill;
                else
                    foreground = inactive_fill;

                xcb_rectangle_t rects[] = { rectangles[ws->num - min_ws_num] };
                xcb_poly_fill_rectangle(conn, drawable, foreground, 1, rects);
            }
        }
    }
}

static void draw_line_workspaces(i3ipcConnection* i3)
{
    for (int i = 0; i < line_windows_len; i++) {
        line_window_t line = line_windows[i];
        xcb_pixmap_t pixmap = xcb_generate_id(conn);
        xcb_create_pixmap(conn, screen->root_depth, pixmap, screen->root, line.width, line_height);
        xcb_rectangle_t rectangles[] = { { 0, 0, line.width, line_height } };
        xcb_poly_fill_rectangle(conn, pixmap, white_fill, 1, rectangles);

        GSList* replies = i3ipc_connection_get_workspaces(i3, NULL);
        draw_workspaces(replies, pixmap, line);
        g_slist_free_full(replies, (GDestroyNotify)i3ipc_workspace_reply_free);

        xcb_copy_area(conn, pixmap, line.win, black_fill, 0, 0, 0, 0, line.width, line_height);
        xcb_free_pixmap(conn, pixmap);
    }

    xcb_flush(conn);
}

static void workspace_event_handler(i3ipcConnection* i3, i3ipcWorkspaceEvent* ev)
{
    //TODO only partial update with ev data?
    printf("event: %s\n", ev->change);
    draw_line_workspaces(i3);
}

static void window_event_handler(i3ipcConnection* i3, i3ipcWindowEvent* ev)
{
    //TODO only redraw the affected output(s), for global fullscreen -> all
    printf("event: %s\n", ev->change);
    gboolean fullscreen;
    g_object_get(ev->container, "fullscreen-mode", &fullscreen, NULL);
    if (!fullscreen) {
        //TODO: why do we need to redraw when coming from fullscreen mode?
        draw_line_workspaces(i3);
    }
}

static void i3ipc(void)
{
    i3ipcConnection* i3 = i3ipc_connection_new(NULL, NULL);
    draw_line_workspaces(i3);
    i3ipc_connection_on(i3, "workspace::init", g_cclosure_new(G_CALLBACK(workspace_event_handler), NULL, NULL), NULL);
    i3ipc_connection_on(i3, "workspace::focus", g_cclosure_new(G_CALLBACK(workspace_event_handler), NULL, NULL), NULL);
    i3ipc_connection_on(i3, "workspace::urgent", g_cclosure_new(G_CALLBACK(workspace_event_handler), NULL, NULL), NULL);
    i3ipc_connection_on(i3, "window::fullscreen_mode", g_cclosure_new(G_CALLBACK(window_event_handler), NULL, NULL), NULL);

    i3ipc_connection_main(i3);

    g_object_unref(i3);
}

static int open_ctl(const char* name, snd_ctl_t** ctlp)
{
    snd_ctl_t* ctl;
    if (snd_ctl_open(&ctl, name, SND_CTL_READONLY) < 0) {
        fprintf(stderr, "cannot open ctl %s\n", name);
        return 1;
    }

    if (snd_ctl_subscribe_events(ctl, 1) < 0) {
        fprintf(stderr, "cannot subscribe events on ctl %s\n", name);
        snd_ctl_close(ctl);
        return 1;
    }

    *ctlp = ctl;
    return 0;
}

int audio_volume(snd_ctl_event_t *event, long* outvol)
{
    //TODO get from event, this is not the right card
    static const char* card = "default";
    static const char* mix_name = "Master";
    int mix_index = snd_ctl_event_elem_get_index(event);

    snd_mixer_selem_id_t* sid;
    snd_mixer_selem_id_alloca(&sid);

    snd_mixer_selem_id_set_index(sid, mix_index);
    snd_mixer_selem_id_set_name(sid, mix_name);

    snd_mixer_t* handle;
    if (snd_mixer_open(&handle, 0) < 0) {
        fprintf(stderr, "cannot open mixer\n");
        return 1;
    }

    if (snd_mixer_attach(handle, card) < 0) {
        fprintf(stderr, "cannot attach to mixer\n");
        snd_mixer_close(handle);
        return 1;
    }

    if (snd_mixer_selem_register(handle, NULL, NULL) < 0) {
        fprintf(stderr, "cannot register on mixer\n");
        snd_mixer_close(handle);
        return 1;
    }

    if (snd_mixer_load(handle) < 0) {
        fprintf(stderr, "cannot load mixer\n");
        snd_mixer_close(handle);
        return 1;
    }

    snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);
    if (elem == NULL) {
        fprintf(stderr, "cannot find mixer element\n");
        snd_mixer_close(handle);
        return 1;
    }

    long minv, maxv;
    if (snd_mixer_selem_get_playback_volume_range(elem, &minv, &maxv) < 0) {
        fprintf(stderr, "cannot get volume range\n");
        snd_mixer_close(handle);
        return 1;
    }

    long volume;
    if(snd_mixer_selem_get_playback_volume(elem, 0, &volume) < 0) {
        fprintf(stderr, "cannot get volume\n");
        snd_mixer_close(handle);
        return 1;
    }

    *outvol = 100 * (volume - minv) / (maxv - minv);

    snd_mixer_close(handle);
    return 0;
}

static void draw_volume(long volume, xcb_drawable_t drawable, line_window_t line)
{
    int width = line.width * volume / 100;
    xcb_rectangle_t rectangles[] = { { 0, 0, width, line_height } };
    xcb_poly_fill_rectangle(conn, drawable, volume_fill, 1, rectangles);
}

static int draw_line_volume(int card, snd_ctl_t* ctl)
{
    snd_ctl_event_t* event;
    snd_ctl_event_alloca(&event);
    if (snd_ctl_read(ctl, event) < 0) {
        fprintf(stderr, "cannot read from event\n");
        snd_ctl_event_free(event);
        return 1;
    }

    if (snd_ctl_event_get_type(event) == SND_CTL_EVENT_ELEM
        && snd_ctl_event_elem_get_mask(event) & SND_CTL_EVENT_MASK_VALUE) {

        printf("snd event: card %d, #%d (%i,%i,%i,%s,%i)\n",
            card,
            snd_ctl_event_elem_get_numid(event),
            snd_ctl_event_elem_get_interface(event),
            snd_ctl_event_elem_get_device(event),
            snd_ctl_event_elem_get_subdevice(event),
            snd_ctl_event_elem_get_name(event),
            snd_ctl_event_elem_get_index(event));

        long volume;
        if (audio_volume(event, &volume)) {
            fprintf(stderr, "cannot get audio volume\n");
            /* snd_ctl_event_free(event); */
            return 1;
        }

        printf("volume: %ld\n", volume);
        for (int i = 0; i < line_windows_len; i++) {
            line_window_t line = line_windows[i];
            xcb_pixmap_t pixmap = xcb_generate_id(conn);
            xcb_create_pixmap(conn, screen->root_depth, pixmap, screen->root, line.width, line_height);
            xcb_rectangle_t rectangles[] = { { 0, 0, line.width, line_height } };
            xcb_poly_fill_rectangle(conn, pixmap, white_fill, 1, rectangles);

            draw_volume(volume, pixmap, line);

            xcb_copy_area(conn, pixmap, line.win, black_fill, 0, 0, 0, 0, line.width, line_height);
            xcb_free_pixmap(conn, pixmap);
        }

        xcb_flush(conn);
    }

    /* snd_ctl_event_free(event); */
    return 0;
}

static void alsa(void)
{
    snd_ctl_t* ctls[MAX_SND_CARDS];
    int ncards = 0;
    for (int card = -1; snd_card_next(&card) >= 0 && card >= 0;) {
        char cardname[16];
        if (ncards >= MAX_SND_CARDS) {
            fprintf(stderr, "cannot open: too many cards\n");
            goto error;
        }
        sprintf(cardname, "hw:%d", card);
        if (open_ctl(cardname, &ctls[ncards]) < 0)
            goto error;
        ncards++;
    }

    if (ncards == 0) {
        fprintf(stderr, "cannot find any snd card\n");
        goto error;
    }

    while (1) {
        struct pollfd fds[ncards];
        for (int i = 0; i < ncards; i++) {
            snd_ctl_poll_descriptors(ctls[i], &fds[i], 1);
        }

        if (poll(fds, ncards, -1) <= 0)
            break;
        for (int i = 0; i < ncards; i++) {
            unsigned short revents;
            snd_ctl_poll_descriptors_revents(ctls[i], &fds[i], 1, &revents);
            if (revents & POLLIN)
                draw_line_volume(i, ctls[i]);
        }
    }

error:
    for (int i = 0; i < ncards; i++)
        snd_ctl_close(ctls[i]);
}

int main(void)
{
    init_xcb();
    create_line_windows();

    void* funcs[] = {
        &i3ipc,
        &alsa
    };

    int num_funcs = sizeof(funcs) / sizeof(funcs[0]);
    pthread_t threads[num_funcs];

    for (int i = 0; i < num_funcs; i++) {
        pthread_create(&threads[i], NULL, funcs[i], NULL);
    }

    for (int i = 0; i < num_funcs; i++) {
        pthread_join(threads[i], NULL);
    }

    free_line_windows();

    free_xcb();

    return 0;
}

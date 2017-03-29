#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <i3ipc-glib/i3ipc-glib.h>
#include <xcb/xcb.h>

static xcb_connection_t *conn = NULL;
static xcb_screen_t *screen = NULL;
static xcb_colormap_t colormap;
static xcb_gcontext_t black_fill;
static xcb_gcontext_t white_fill;
static xcb_gcontext_t urgent_fill;
static xcb_gcontext_t focus_fill;
static xcb_gcontext_t active_fill;
static xcb_gcontext_t inactive_fill;
static xcb_window_t line_window;

static int line_height = 5;
static int screen_width = 2560; //TODO correct monitor width
static int min_ws_num = 0;
static int max_ws_num = 12;

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

#define ATOM_DO(name)                                                        \
    reply = xcb_intern_atom_reply(conn, atom_cookies[name], NULL); \
    if (reply == NULL) {                                                     \
        fprintf(stderr, "Could not get atom %s\n", #name);                   \
        exit(EXIT_FAILURE);                                                  \
    }                                                                        \
    atoms[name] = reply->atom;                                               \
    free(reply);

#include "xcb_atoms.def"
#undef ATOM_DO
}

static xcb_gcontext_t get_context(uint32_t fg_color, uint32_t bg_color) {
    xcb_gcontext_t foreground = xcb_generate_id(conn);

    uint32_t mask;
    uint32_t values[2];
    mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
    values[0] = fg_color;
    values[1] = bg_color;
    xcb_create_gc(conn, foreground, screen->root, mask, values);

    return foreground;
}

static uint32_t color_pixel(unsigned short r, unsigned short g, unsigned short b) {
    xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(conn, xcb_alloc_color(conn, colormap, r, g, b), NULL);
    uint32_t pixel = reply->pixel;
    free (reply);

    return pixel;
}

static xcb_window_t create_dock_window(void)
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
        0, 0,
        screen_width, line_height,
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

static void draw_workspaces(GSList *replies, xcb_drawable_t drawable) {
    const GSList *reply;
    i3ipcWorkspaceReply *ws;
    int num_ws = max_ws_num - min_ws_num + 1;
    if (num_ws > 0) {
        xcb_rectangle_t rectangles[num_ws];
        int width = screen_width / num_ws;
        int curr_pos = 0;
        int curr_num;
        for (curr_num = 0; curr_num < num_ws; curr_num++)
        {
            int curr_width = (curr_num == num_ws - 1) ? screen_width - curr_pos : width;
            xcb_rectangle_t rect = { curr_pos, 0, curr_width, line_height };
            rectangles[curr_num] = rect;
            curr_pos += curr_width;
        }

        xcb_poly_rectangle(conn, drawable, white_fill, num_ws, rectangles);

        for (reply = replies; reply; reply = reply->next)
        {
            ws = reply->data;
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

                xcb_rectangle_t orig_rect = rectangles[ws->num - min_ws_num];
                xcb_rectangle_t rects[] = { { orig_rect.x + 1, orig_rect.y + 1, orig_rect.width - 2, orig_rect.height - 2 } };
                xcb_poly_fill_rectangle(conn, drawable, foreground, 1, rects);
            }
        }
    }
}

static void draw_line_content(i3ipcConnection *i3) {
    xcb_pixmap_t pixmap = xcb_generate_id(conn);
    xcb_create_pixmap(conn, screen->root_depth, pixmap, screen->root, screen_width, line_height);
    xcb_rectangle_t rectangles[] = { { 0, 0, screen_width, line_height } };
    xcb_poly_fill_rectangle(conn, pixmap, white_fill, 1, rectangles);

    GSList *replies = i3ipc_connection_get_workspaces(i3, NULL);
    draw_workspaces(replies, pixmap);
    g_slist_free_full(replies, (GDestroyNotify) i3ipc_workspace_reply_free);

    xcb_copy_area(conn, pixmap, line_window, black_fill, 0, 0, 0, 0, screen_width, line_height);

    xcb_flush(conn);

    xcb_free_pixmap(conn, pixmap);
}

static void workspace_event_handler(i3ipcConnection *i3, i3ipcWorkspaceEvent *ev, gpointer data)
{
    //TODO only partial update with ev data?
    printf("event: %s\n", ev->change);
    draw_line_content(i3);
}

int main(void) {
    conn = xcb_connect(NULL, NULL);
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;

    request_atoms();
    line_window = create_dock_window();

    colormap = xcb_generate_id(conn);
    xcb_create_colormap(conn, XCB_COLORMAP_ALLOC_NONE, colormap, line_window, screen->root_visual);

    black_fill = get_context(screen->black_pixel, screen->white_pixel);
    white_fill = get_context(screen->white_pixel, screen->white_pixel);
    urgent_fill = get_context(color_pixel(65535, 0, 0), screen->white_pixel);
    active_fill = get_context(color_pixel(0, 65535, 0), screen->white_pixel);
    focus_fill = get_context(color_pixel(0, 0, 65535), screen->white_pixel);
    inactive_fill = get_context(color_pixel(30000, 30000, 30000), screen->white_pixel);

    i3ipcConnection *i3 = i3ipc_connection_new(NULL, NULL);
    draw_line_content(i3);
    i3ipc_connection_on(i3, "workspace::init", g_cclosure_new(G_CALLBACK(workspace_event_handler), NULL, NULL), NULL);
    i3ipc_connection_on(i3, "workspace::focus", g_cclosure_new(G_CALLBACK(workspace_event_handler), NULL, NULL), NULL);
    i3ipc_connection_on(i3, "workspace::urgent", g_cclosure_new(G_CALLBACK(workspace_event_handler), NULL, NULL), NULL);

    i3ipc_connection_main(i3);

    return 0;
}

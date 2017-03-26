#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>

enum {
#define ATOM_DO(name) name,
#include "xcb_atoms.def"
    NUM_ATOMS
#undef ATOM_DO
};

xcb_connection_t *connection;
xcb_screen_t *screen;

xcb_intern_atom_cookie_t atom_cookies[NUM_ATOMS];
xcb_atom_t atoms[NUM_ATOMS];

static void request_atoms(void)
{
#define ATOM_DO(name) atom_cookies[name] = xcb_intern_atom(connection, 0, strlen(#name), #name);
#include "xcb_atoms.def"
#undef ATOM_DO
}

static void get_atoms(void)
{
    xcb_intern_atom_reply_t* reply;
#define ATOM_DO(name)                                                        \
    reply = xcb_intern_atom_reply(connection, atom_cookies[name], NULL);     \
    if (reply == NULL) {                                                     \
        fprintf(stderr, "Could not get atom %s\n", #name);                   \
        exit(EXIT_FAILURE);                                                  \
    }                                                                        \
    atoms[name] = reply->atom;                                               \
    free(reply);

#include "xcb_atoms.def"
#undef ATOM_DO
}

static xcb_gcontext_t get_foreground(void) {
    xcb_gcontext_t foreground = xcb_generate_id(connection);

    uint32_t mask;
    uint32_t values[2];
    mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    values[0] = screen->black_pixel;
    values[1] = 0;
    xcb_create_gc(connection, foreground, screen->root, mask, values);

    return foreground;
}

static void xcb_init(void)
{
    connection = xcb_connect(NULL, NULL);

    request_atoms();
    screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;

    get_atoms();
}

static void draw_bar(void)
{
    xcb_window_t win = xcb_generate_id(connection);

    uint32_t mask;
    uint32_t values[2];
    mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    values[0] = screen->white_pixel;
    values[1] = XCB_EVENT_MASK_EXPOSURE;
    xcb_create_window(connection,
        XCB_COPY_FROM_PARENT,
        win,
        screen->root,
        0, 0,
        screen->width_in_pixels, 10,
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,
        mask, values);

    xcb_change_property(connection,
        XCB_PROP_MODE_APPEND,
        win,
        atoms[_NET_WM_WINDOW_TYPE],
        XCB_ATOM_ATOM,
        32,
        1,
        &atoms[_NET_WM_WINDOW_TYPE_DOCK]);

    xcb_map_window(connection, win);

    xcb_flush(connection);
}

static void draw_content(xcb_window_t win) {
    xcb_point_t points[] = {
        { 10, 1 },
        { 10, 2 },
        { 20, 1 },
        { 20, 2 }
    };

    xcb_point_t polyline[] = {
        { 50, 10 },
        { 5, 20 },
        { 25, -20 },
        { 10, 10 }
    };

    xcb_segment_t segments[] = {
        { 100, 10, 140, 30 },
        { 110, 25, 130, 60 }
    };

    xcb_rectangle_t rectangles[] = {
        { 10, 50, 40, 20 },
        { 80, 50, 10, 40 }
    };

    xcb_arc_t arcs[] = {
        { 10, 100, 60, 40, 0, 90 << 6 },
        { 90, 100, 55, 40, 0, 270 << 6 }
    };

    xcb_gcontext_t foreground = get_foreground();

    xcb_poly_point(connection, XCB_COORD_MODE_ORIGIN, win, foreground, 4, points);

    xcb_poly_line(connection, XCB_COORD_MODE_PREVIOUS, win, foreground, 4, polyline);

    xcb_poly_segment(connection, win, foreground, 2, segments);

    xcb_poly_rectangle(connection, win, foreground, 2, rectangles);

    xcb_poly_arc(connection, win, foreground, 2, arcs);

    xcb_flush(connection);
}

int main(void)
{
    xcb_init();

    draw_bar();

    xcb_generic_event_t* e;
    while ((e = xcb_wait_for_event(connection))) {
        switch (e->response_type & ~0x80) {
        case XCB_EXPOSE: {
            xcb_expose_event_t *ev = (xcb_expose_event_t *) e;
            draw_content(ev->window);
            break;
        }
        default: {
            break;
        }
        }

        free(e);
    }

    return 0;
}

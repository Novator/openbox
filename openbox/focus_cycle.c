/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   focus.c for the Openbox window manager
   Copyright (c) 2006        Mikael Magnusson
   Copyright (c) 2003-2007   Dana Jansens

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   See the COPYING file for a copy of the GNU General Public License.
*/

#include "focus_cycle.h"
#include "client.h"
#include "focus.h"
#include "screen.h"
#include "openbox.h"
#include "frame.h"
#include "popup.h"
#include "debug.h"
#include "group.h"
#include "event.h"
#include "render/render.h"

#include <X11/Xlib.h>
#include <glib.h>

#define FOCUS_INDICATOR_WIDTH 6

struct {
    InternalWindow top;
    InternalWindow left;
    InternalWindow right;
    InternalWindow bottom;
} focus_indicator;

ObClient     *focus_cycle_target = NULL;

static RrAppearance *a_focus_indicator;
static RrColor      *color_white;
static ObIconPopup  *focus_cycle_popup;

static void      focus_cycle_destroy_notify (ObClient *client, gpointer data);
static Window    create_window              (Window parent, gulong mask,
                                             XSetWindowAttributes *attrib);
static gboolean  focus_target_valid         (ObClient *ft,
                                             gboolean all_desktops,
                                             gboolean dock_windows,
                                             gboolean desktop_windows);
static gboolean  focus_target_has_siblings  (ObClient *ft,
                                             gboolean all_desktops);
static gchar    *popup_get_name             (ObClient *c,
                                             ObClient **nametarget);
static void      popup_cycle                (ObClient *c,
                                             gboolean show,
                                             gboolean all_desktops,
                                             gboolean dock_windows,
                                             gboolean desktop_windows);
static ObClient *focus_find_directional    (ObClient *c,
                                            ObDirection dir,
                                            gboolean dock_windows,
                                            gboolean desktop_windows);
static ObClient *focus_find_directional    (ObClient *c,
                                            ObDirection dir,
                                            gboolean dock_windows,
                                            gboolean desktop_windows);

void focus_cycle_startup(gboolean reconfig)
{
    focus_cycle_popup = icon_popup_new(TRUE);

    if (!reconfig) {
        XSetWindowAttributes attr;

        client_add_destroy_notify(focus_cycle_destroy_notify, NULL);

        focus_indicator.top.obwin.type = Window_Internal;
        focus_indicator.left.obwin.type = Window_Internal;
        focus_indicator.right.obwin.type = Window_Internal;
        focus_indicator.bottom.obwin.type = Window_Internal;

        attr.override_redirect = True;
        attr.background_pixel = BlackPixel(ob_display, ob_screen);
        focus_indicator.top.win =
            create_window(RootWindow(ob_display, ob_screen),
                          CWOverrideRedirect | CWBackPixel, &attr);
        focus_indicator.left.win =
            create_window(RootWindow(ob_display, ob_screen),
                          CWOverrideRedirect | CWBackPixel, &attr);
        focus_indicator.right.win =
            create_window(RootWindow(ob_display, ob_screen),
                          CWOverrideRedirect | CWBackPixel, &attr);
        focus_indicator.bottom.win =
            create_window(RootWindow(ob_display, ob_screen),
                          CWOverrideRedirect | CWBackPixel, &attr);

        stacking_add(INTERNAL_AS_WINDOW(&focus_indicator.top));
        stacking_add(INTERNAL_AS_WINDOW(&focus_indicator.left));
        stacking_add(INTERNAL_AS_WINDOW(&focus_indicator.right));
        stacking_add(INTERNAL_AS_WINDOW(&focus_indicator.bottom));

        color_white = RrColorNew(ob_rr_inst, 0xff, 0xff, 0xff);

        a_focus_indicator = RrAppearanceNew(ob_rr_inst, 4);
        a_focus_indicator->surface.grad = RR_SURFACE_SOLID;
        a_focus_indicator->surface.relief = RR_RELIEF_FLAT;
        a_focus_indicator->surface.primary = RrColorNew(ob_rr_inst,
                                                        0, 0, 0);
        a_focus_indicator->texture[0].type = RR_TEXTURE_LINE_ART;
        a_focus_indicator->texture[0].data.lineart.color = color_white;
        a_focus_indicator->texture[1].type = RR_TEXTURE_LINE_ART;
        a_focus_indicator->texture[1].data.lineart.color = color_white;
        a_focus_indicator->texture[2].type = RR_TEXTURE_LINE_ART;
        a_focus_indicator->texture[2].data.lineart.color = color_white;
        a_focus_indicator->texture[3].type = RR_TEXTURE_LINE_ART;
        a_focus_indicator->texture[3].data.lineart.color = color_white;
    }
}

void focus_cycle_shutdown(gboolean reconfig)
{
    icon_popup_free(focus_cycle_popup);

    if (!reconfig) {
        client_remove_destroy_notify(focus_cycle_destroy_notify);

        RrColorFree(color_white);

        RrAppearanceFree(a_focus_indicator);

        XDestroyWindow(ob_display, focus_indicator.top.win);
        XDestroyWindow(ob_display, focus_indicator.left.win);
        XDestroyWindow(ob_display, focus_indicator.right.win);
        XDestroyWindow(ob_display, focus_indicator.bottom.win);
    }
}

void focus_cycle_stop()
{
    if (focus_cycle_target)
        focus_cycle(TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE);
}


static Window create_window(Window parent, gulong mask,
                            XSetWindowAttributes *attrib)
{
    return XCreateWindow(ob_display, parent, 0, 0, 1, 1, 0,
                         RrDepth(ob_rr_inst), InputOutput,
                         RrVisual(ob_rr_inst), mask, attrib);
                       
}

static gchar *popup_get_name(ObClient *c, ObClient **nametarget)
{
    ObClient *p;
    gchar *title = NULL;
    const gchar *desk = NULL;
    gchar *ret;

    /* find our highest direct parent, including non-normal windows */
    for (p = c; p->transient_for && p->transient_for != OB_TRAN_GROUP;
         p = p->transient_for);

    if (c->desktop != DESKTOP_ALL && c->desktop != screen_desktop)
        desk = screen_desktop_names[c->desktop];

    /* use the transient's parent's title/icon if we don't have one */
    if (p != c && !strcmp("", (c->iconic ? c->icon_title : c->title)))
        title = g_strdup(p->iconic ? p->icon_title : p->title);

    if (title == NULL)
        title = g_strdup(c->iconic ? c->icon_title : c->title);

    if (desk)
        ret = g_strdup_printf("%s [%s]", title, desk);
    else {
        ret = title;
        title = NULL;
    }
    g_free(title);

    /* set this only if we're returning true and they asked for it */
    if (ret && nametarget) *nametarget = p;
    return ret;
}

static void popup_cycle(ObClient *c, gboolean show,
                        gboolean all_desktops, gboolean dock_windows,
                        gboolean desktop_windows)
{
    gchar *showtext = NULL;
    ObClient *showtarget;

    if (!show) {
        icon_popup_hide(focus_cycle_popup);
        return;
    }

    /* do this stuff only when the dialog is first showing */
    if (!focus_cycle_popup->popup->mapped &&
        !focus_cycle_popup->popup->delay_mapped)
    {
        Rect *a;
        gchar **names;
        GList *targets = NULL, *it;
        gint n = 0, i;

        /* position the popup */
        a = screen_physical_area_monitor(0);
        icon_popup_position(focus_cycle_popup, CenterGravity,
                            a->x + a->width / 2, a->y + a->height / 2);
        icon_popup_height(focus_cycle_popup, POPUP_HEIGHT);
        icon_popup_min_width(focus_cycle_popup, POPUP_WIDTH);
        icon_popup_max_width(focus_cycle_popup,
                             MAX(a->width/3, POPUP_WIDTH));


        /* make its width to be the width of all the possible titles */

        /* build a list of all the valid focus targets */
        for (it = focus_order; it; it = g_list_next(it)) {
            ObClient *ft = it->data;
            if (focus_target_valid(ft, all_desktops, dock_windows
                                   , desktop_windows))
            {
                targets = g_list_prepend(targets, ft);
                ++n;
            }
        }
        /* make it null terminated so we can use g_strfreev */
        names = g_new(char*, n+1);
        for (it = targets, i = 0; it; it = g_list_next(it), ++i) {
            ObClient *ft = it->data, *t;
            names[i] = popup_get_name(ft, &t);

            /* little optimization.. save this text and client, so we dont
               have to get it again */
            if (ft == c) {
                showtext = g_strdup(names[i]);
                showtarget = t;
            }
        }
        names[n] = NULL;

        icon_popup_text_width_to_strings(focus_cycle_popup, names, n);
        g_strfreev(names);
    }


    if (!showtext) showtext = popup_get_name(c, &showtarget);
    icon_popup_show(focus_cycle_popup, showtext,
                    client_icon(showtarget, 48, 48));
    g_free(showtext);
}

static void focus_cycle_destroy_notify(ObClient *client, gpointer data)
{
    /* end cycling if the target disappears. CurrentTime is fine, time won't
       be used
    */
    if (focus_cycle_target == client)
        focus_cycle(TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE);
}

void focus_cycle_draw_indicator()
{
    if (!focus_cycle_target) {
        XUnmapWindow(ob_display, focus_indicator.top.win);
        XUnmapWindow(ob_display, focus_indicator.left.win);
        XUnmapWindow(ob_display, focus_indicator.right.win);
        XUnmapWindow(ob_display, focus_indicator.bottom.win);

        /* kill enter events cause by this unmapping */
        event_ignore_all_queued_enters();
    } else {
        /*
          if (focus_cycle_target)
              frame_adjust_focus(focus_cycle_target->frame, FALSE);
          frame_adjust_focus(focus_cycle_target->frame, TRUE);
        */
        gint x, y, w, h;
        gint wt, wl, wr, wb;

        wt = wl = wr = wb = FOCUS_INDICATOR_WIDTH;

        x = focus_cycle_target->frame->area.x;
        y = focus_cycle_target->frame->area.y;
        w = focus_cycle_target->frame->area.width;
        h = wt;

        XMoveResizeWindow(ob_display, focus_indicator.top.win,
                          x, y, w, h);
        a_focus_indicator->texture[0].data.lineart.x1 = 0;
        a_focus_indicator->texture[0].data.lineart.y1 = h-1;
        a_focus_indicator->texture[0].data.lineart.x2 = 0;
        a_focus_indicator->texture[0].data.lineart.y2 = 0;
        a_focus_indicator->texture[1].data.lineart.x1 = 0;
        a_focus_indicator->texture[1].data.lineart.y1 = 0;
        a_focus_indicator->texture[1].data.lineart.x2 = w-1;
        a_focus_indicator->texture[1].data.lineart.y2 = 0;
        a_focus_indicator->texture[2].data.lineart.x1 = w-1;
        a_focus_indicator->texture[2].data.lineart.y1 = 0;
        a_focus_indicator->texture[2].data.lineart.x2 = w-1;
        a_focus_indicator->texture[2].data.lineart.y2 = h-1;
        a_focus_indicator->texture[3].data.lineart.x1 = (wl-1);
        a_focus_indicator->texture[3].data.lineart.y1 = h-1;
        a_focus_indicator->texture[3].data.lineart.x2 = w - wr;
        a_focus_indicator->texture[3].data.lineart.y2 = h-1;
        RrPaint(a_focus_indicator, focus_indicator.top.win,
                w, h);

        x = focus_cycle_target->frame->area.x;
        y = focus_cycle_target->frame->area.y;
        w = wl;
        h = focus_cycle_target->frame->area.height;

        XMoveResizeWindow(ob_display, focus_indicator.left.win,
                          x, y, w, h);
        a_focus_indicator->texture[0].data.lineart.x1 = w-1;
        a_focus_indicator->texture[0].data.lineart.y1 = 0;
        a_focus_indicator->texture[0].data.lineart.x2 = 0;
        a_focus_indicator->texture[0].data.lineart.y2 = 0;
        a_focus_indicator->texture[1].data.lineart.x1 = 0;
        a_focus_indicator->texture[1].data.lineart.y1 = 0;
        a_focus_indicator->texture[1].data.lineart.x2 = 0;
        a_focus_indicator->texture[1].data.lineart.y2 = h-1;
        a_focus_indicator->texture[2].data.lineart.x1 = 0;
        a_focus_indicator->texture[2].data.lineart.y1 = h-1;
        a_focus_indicator->texture[2].data.lineart.x2 = w-1;
        a_focus_indicator->texture[2].data.lineart.y2 = h-1;
        a_focus_indicator->texture[3].data.lineart.x1 = w-1;
        a_focus_indicator->texture[3].data.lineart.y1 = wt-1;
        a_focus_indicator->texture[3].data.lineart.x2 = w-1;
        a_focus_indicator->texture[3].data.lineart.y2 = h - wb;
        RrPaint(a_focus_indicator, focus_indicator.left.win,
                w, h);

        x = focus_cycle_target->frame->area.x +
            focus_cycle_target->frame->area.width - wr;
        y = focus_cycle_target->frame->area.y;
        w = wr;
        h = focus_cycle_target->frame->area.height ;

        XMoveResizeWindow(ob_display, focus_indicator.right.win,
                          x, y, w, h);
        a_focus_indicator->texture[0].data.lineart.x1 = 0;
        a_focus_indicator->texture[0].data.lineart.y1 = 0;
        a_focus_indicator->texture[0].data.lineart.x2 = w-1;
        a_focus_indicator->texture[0].data.lineart.y2 = 0;
        a_focus_indicator->texture[1].data.lineart.x1 = w-1;
        a_focus_indicator->texture[1].data.lineart.y1 = 0;
        a_focus_indicator->texture[1].data.lineart.x2 = w-1;
        a_focus_indicator->texture[1].data.lineart.y2 = h-1;
        a_focus_indicator->texture[2].data.lineart.x1 = w-1;
        a_focus_indicator->texture[2].data.lineart.y1 = h-1;
        a_focus_indicator->texture[2].data.lineart.x2 = 0;
        a_focus_indicator->texture[2].data.lineart.y2 = h-1;
        a_focus_indicator->texture[3].data.lineart.x1 = 0;
        a_focus_indicator->texture[3].data.lineart.y1 = wt-1;
        a_focus_indicator->texture[3].data.lineart.x2 = 0;
        a_focus_indicator->texture[3].data.lineart.y2 = h - wb;
        RrPaint(a_focus_indicator, focus_indicator.right.win,
                w, h);

        x = focus_cycle_target->frame->area.x;
        y = focus_cycle_target->frame->area.y +
            focus_cycle_target->frame->area.height - wb;
        w = focus_cycle_target->frame->area.width;
        h = wb;

        XMoveResizeWindow(ob_display, focus_indicator.bottom.win,
                          x, y, w, h);
        a_focus_indicator->texture[0].data.lineart.x1 = 0;
        a_focus_indicator->texture[0].data.lineart.y1 = 0;
        a_focus_indicator->texture[0].data.lineart.x2 = 0;
        a_focus_indicator->texture[0].data.lineart.y2 = h-1;
        a_focus_indicator->texture[1].data.lineart.x1 = 0;
        a_focus_indicator->texture[1].data.lineart.y1 = h-1;
        a_focus_indicator->texture[1].data.lineart.x2 = w-1;
        a_focus_indicator->texture[1].data.lineart.y2 = h-1;
        a_focus_indicator->texture[2].data.lineart.x1 = w-1;
        a_focus_indicator->texture[2].data.lineart.y1 = h-1;
        a_focus_indicator->texture[2].data.lineart.x2 = w-1;
        a_focus_indicator->texture[2].data.lineart.y2 = 0;
        a_focus_indicator->texture[3].data.lineart.x1 = wl-1;
        a_focus_indicator->texture[3].data.lineart.y1 = 0;
        a_focus_indicator->texture[3].data.lineart.x2 = w - wr;
        a_focus_indicator->texture[3].data.lineart.y2 = 0;
        RrPaint(a_focus_indicator, focus_indicator.bottom.win,
                w, h);

        XMapWindow(ob_display, focus_indicator.top.win);
        XMapWindow(ob_display, focus_indicator.left.win);
        XMapWindow(ob_display, focus_indicator.right.win);
        XMapWindow(ob_display, focus_indicator.bottom.win);
    }
}

/*! Returns if a focus target has valid group siblings that can be cycled
  to in its place */
static gboolean focus_target_has_siblings(ObClient *ft, gboolean all_desktops)
                                                         
{
    GSList *it;

    if (!ft->group) return FALSE;

    for (it = ft->group->members; it; it = g_slist_next(it)) {
        ObClient *c = it->data;
        /* check that it's not a helper window to avoid infinite recursion */
        if (c != ft && !client_helper(c) &&
            focus_target_valid(c, all_desktops, FALSE, FALSE))
        {
            return TRUE;
        }
    }
    return FALSE;
}

/*! @param allow_helpers This is used for calling itself recursively while
                         checking helper windows. */
static gboolean focus_target_valid(ObClient *ft,
                                   gboolean all_desktops,
                                   gboolean dock_windows,
                                   gboolean desktop_windows)
{
    gboolean ok = FALSE;

    /* it's on this desktop unless you want all desktops.

       do this check first because it will usually filter out the most
       windows */
    ok = (all_desktops || ft->desktop == screen_desktop ||
          ft->desktop == DESKTOP_ALL);

    /* the window can receive focus somehow */
    ok = ok && (ft->can_focus || ft->focus_notify);

    /* it's the right type of window */
    if (dock_windows || desktop_windows)
        ok = ok && ((dock_windows && ft->type == OB_CLIENT_TYPE_DOCK) ||
                    (desktop_windows && ft->type == OB_CLIENT_TYPE_DESKTOP));
    else
        /* normal non-helper windows are valid targets */
        ok = ok &&
            ((client_normal(ft) && !client_helper(ft))
             ||
             /* helper windows are valid targets it... */
             (client_helper(ft) &&
              /* ...a window in its group already has focus ... */
              ((focus_client && ft->group == focus_client->group) ||
               /* ... or if there are no other windows in its group 
                  that can be cycled to instead */
               !focus_target_has_siblings(ft, all_desktops))));

    /* it's not set to skip the taskbar (unless it is a type that would be
       expected to set this hint */
    ok = ok && ((ft->type == OB_CLIENT_TYPE_DOCK ||
                 ft->type == OB_CLIENT_TYPE_DESKTOP ||
                 ft->type == OB_CLIENT_TYPE_TOOLBAR ||
                 ft->type == OB_CLIENT_TYPE_MENU ||
                 ft->type == OB_CLIENT_TYPE_UTILITY) ||
                !ft->skip_taskbar);

    /* it's not going to just send fous off somewhere else (modal window) */
    ok = ok && ft == client_focus_target(ft);

    return ok;
}

void focus_cycle(gboolean forward, gboolean all_desktops,
                 gboolean dock_windows, gboolean desktop_windows,
                 gboolean linear, gboolean interactive,
                 gboolean dialog, gboolean done, gboolean cancel)
{
    static ObClient *first = NULL;
    static ObClient *t = NULL;
    static GList *order = NULL;
    GList *it, *start, *list;
    ObClient *ft = NULL;

    if (interactive) {
        if (cancel) {
            focus_cycle_target = NULL;
            goto done_cycle;
        } else if (done)
            goto done_cycle;

        if (!focus_order)
            goto done_cycle;

        if (!first) first = focus_client;

        if (linear) list = client_list;
        else        list = focus_order;
    } else {
        if (!focus_order)
            goto done_cycle;
        list = client_list;
    }
    if (!focus_cycle_target) focus_cycle_target = focus_client;

    start = it = g_list_find(list, focus_cycle_target);
    if (!start) /* switched desktops or something? */
        start = it = forward ? g_list_last(list) : g_list_first(list);
    if (!start) goto done_cycle;

    do {
        if (forward) {
            it = it->next;
            if (it == NULL) it = g_list_first(list);
        } else {
            it = it->prev;
            if (it == NULL) it = g_list_last(list);
        }
        ft = it->data;
        if (focus_target_valid(ft, all_desktops, dock_windows,
                               desktop_windows))
        {
            if (interactive) {
                if (ft != focus_cycle_target) { /* prevents flicker */
                    focus_cycle_target = ft;
                    focus_cycle_draw_indicator();
                }
                /* same arguments as focus_target_valid */
                popup_cycle(ft, dialog, all_desktops, dock_windows,
                            desktop_windows);
                return;
            } else if (ft != focus_cycle_target) {
                focus_cycle_target = ft;
                done = TRUE;
                break;
            }
        }
    } while (it != start);

done_cycle:
    if (done && focus_cycle_target)
        client_activate(focus_cycle_target, FALSE, TRUE);

    t = NULL;
    first = NULL;
    focus_cycle_target = NULL;
    g_list_free(order);
    order = NULL;

    if (interactive) {
        focus_cycle_draw_indicator();
        popup_cycle(ft, FALSE, FALSE, FALSE, FALSE);
    }

    return;
}

/* this be mostly ripped from fvwm */
static ObClient *focus_find_directional(ObClient *c, ObDirection dir,
                                        gboolean dock_windows,
                                        gboolean desktop_windows) 
{
    gint my_cx, my_cy, his_cx, his_cy;
    gint offset = 0;
    gint distance = 0;
    gint score, best_score;
    ObClient *best_client, *cur;
    GList *it;

    if(!client_list)
        return NULL;

    /* first, find the centre coords of the currently focused window */
    my_cx = c->frame->area.x + c->frame->area.width / 2;
    my_cy = c->frame->area.y + c->frame->area.height / 2;

    best_score = -1;
    best_client = NULL;

    for(it = g_list_first(client_list); it; it = g_list_next(it)) {
        cur = it->data;

        /* the currently selected window isn't interesting */
        if(cur == c)
            continue;
        if (cur->type == OB_CLIENT_TYPE_DOCK && !dock_windows)
            continue;
        if (cur->type == OB_CLIENT_TYPE_DESKTOP && !desktop_windows)
            continue;
        if (!client_normal(cur) &&
            cur->type != OB_CLIENT_TYPE_DOCK &&
            cur->type != OB_CLIENT_TYPE_DESKTOP)
            continue;
        /* using c->desktop instead of screen_desktop doesn't work if the
         * current window was omnipresent, hope this doesn't have any other
         * side effects */
        if(screen_desktop != cur->desktop && cur->desktop != DESKTOP_ALL)
            continue;
        if(cur->iconic)
            continue;
        if(!(client_focus_target(cur) == cur &&
             client_can_focus(cur)))
            continue;

        /* find the centre coords of this window, from the
         * currently focused window's point of view */
        his_cx = (cur->frame->area.x - my_cx)
            + cur->frame->area.width / 2;
        his_cy = (cur->frame->area.y - my_cy)
            + cur->frame->area.height / 2;

        if(dir == OB_DIRECTION_NORTHEAST || dir == OB_DIRECTION_SOUTHEAST ||
           dir == OB_DIRECTION_SOUTHWEST || dir == OB_DIRECTION_NORTHWEST) {
            gint tx;
            /* Rotate the diagonals 45 degrees counterclockwise.
             * To do this, multiply the matrix /+h +h\ with the
             * vector (x y).                   \-h +h/
             * h = sqrt(0.5). We can set h := 1 since absolute
             * distance doesn't matter here. */
            tx = his_cx + his_cy;
            his_cy = -his_cx + his_cy;
            his_cx = tx;
        }

        switch(dir) {
        case OB_DIRECTION_NORTH:
        case OB_DIRECTION_SOUTH:
        case OB_DIRECTION_NORTHEAST:
        case OB_DIRECTION_SOUTHWEST:
            offset = (his_cx < 0) ? -his_cx : his_cx;
            distance = ((dir == OB_DIRECTION_NORTH ||
                         dir == OB_DIRECTION_NORTHEAST) ?
                        -his_cy : his_cy);
            break;
        case OB_DIRECTION_EAST:
        case OB_DIRECTION_WEST:
        case OB_DIRECTION_SOUTHEAST:
        case OB_DIRECTION_NORTHWEST:
            offset = (his_cy < 0) ? -his_cy : his_cy;
            distance = ((dir == OB_DIRECTION_WEST ||
                         dir == OB_DIRECTION_NORTHWEST) ?
                        -his_cx : his_cx);
            break;
        }

        /* the target must be in the requested direction */
        if(distance <= 0)
            continue;

        /* Calculate score for this window.  The smaller the better. */
        score = distance + offset;

        /* windows more than 45 degrees off the direction are
         * heavily penalized and will only be chosen if nothing
         * else within a million pixels */
        if(offset > distance)
            score += 1000000;

        if(best_score == -1 || score < best_score)
            best_client = cur,
                best_score = score;
    }

    return best_client;
}

void focus_directional_cycle(ObDirection dir, gboolean dock_windows,
                             gboolean desktop_windows, gboolean interactive,
                             gboolean dialog, gboolean done, gboolean cancel)
{
    static ObClient *first = NULL;
    ObClient *ft = NULL;

    if (!interactive)
        return;

    if (cancel) {
        focus_cycle_target = NULL;
        goto done_cycle;
    } else if (done)
        goto done_cycle;

    if (!focus_order)
        goto done_cycle;

    if (!first) first = focus_client;
    if (!focus_cycle_target) focus_cycle_target = focus_client;

    if (focus_cycle_target)
        ft = focus_find_directional(focus_cycle_target, dir, dock_windows,
                                    desktop_windows);
    else {
        GList *it;

        for (it = focus_order; it; it = g_list_next(it))
            if (focus_target_valid(it->data, FALSE, dock_windows,
                                   desktop_windows))
                ft = it->data;
    }
        
    if (ft) {
        if (ft != focus_cycle_target) {/* prevents flicker */
            focus_cycle_target = ft;
            focus_cycle_draw_indicator();
        }
    }
    if (focus_cycle_target) {
        /* same arguments as focus_target_valid */
        popup_cycle(focus_cycle_target, dialog, FALSE, dock_windows,
                    desktop_windows);
        if (dialog)
            return;
    }


done_cycle:
    if (done && focus_cycle_target)
        client_activate(focus_cycle_target, FALSE, TRUE);

    first = NULL;
    focus_cycle_target = NULL;

    focus_cycle_draw_indicator();
    popup_cycle(ft, FALSE, FALSE, FALSE, FALSE);

    return;
}

void focus_order_add_new(ObClient *c)
{
    if (c->iconic)
        focus_order_to_top(c);
    else {
        g_assert(!g_list_find(focus_order, c));
        /* if there are any iconic windows, put this above them in the order,
           but if there are not, then put it under the currently focused one */
        if (focus_order && ((ObClient*)focus_order->data)->iconic)
            focus_order = g_list_insert(focus_order, c, 0);
        else
            focus_order = g_list_insert(focus_order, c, 1);
    }
}


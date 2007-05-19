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

#include "debug.h"
#include "event.h"
#include "openbox.h"
#include "grab.h"
#include "client.h"
#include "config.h"
#include "focus_cycle.h"
#include "screen.h"
#include "prop.h"
#include "keyboard.h"
#include "focus.h"
#include "stacking.h"

#include <X11/Xlib.h>
#include <glib.h>

#define FOCUS_INDICATOR_WIDTH 6

ObClient *focus_client = NULL;
GList *focus_order = NULL;

void focus_startup(gboolean reconfig)
{
    if (reconfig) return;

    /* start with nothing focused */
    focus_nothing();
}

void focus_shutdown(gboolean reconfig)
{
    if (reconfig) return;

    /* reset focus to root */
    XSetInputFocus(ob_display, PointerRoot, RevertToNone, CurrentTime);
}

static void push_to_top(ObClient *client)
{
    focus_order = g_list_remove(focus_order, client);
    focus_order = g_list_prepend(focus_order, client);
}

void focus_set_client(ObClient *client)
{
    Window active;

    ob_debug_type(OB_DEBUG_FOCUS,
                  "focus_set_client 0x%lx\n", client ? client->window : 0);

    if (focus_client == client)
        return;

    /* uninstall the old colormap, and install the new one */
    screen_install_colormap(focus_client, FALSE);
    screen_install_colormap(client, TRUE);

    /* in the middle of cycling..? kill it. CurrentTime is fine, time won't
       be used.
    */
    focus_cycle_stop();

    focus_client = client;

    if (client != NULL) {
        /* move to the top of the list */
        push_to_top(client);
        /* remove hiliting from the window when it gets focused */
        client_hilite(client, FALSE);
    }

    /* set the NET_ACTIVE_WINDOW hint, but preserve it on shutdown */
    if (ob_state() != OB_STATE_EXITING) {
        active = client ? client->window : None;
        PROP_SET32(RootWindow(ob_display, ob_screen),
                   net_active_window, window, active);
    }
}

static ObClient* focus_fallback_target(gboolean allow_refocus)
{
    GList *it;
    ObClient *c;
    ObClient *old = focus_client;

    ob_debug_type(OB_DEBUG_FOCUS, "trying pointer stuff\n");
    if (config_focus_follow && !config_focus_last)
        if ((c = client_under_pointer()) &&
            (allow_refocus || c != old) &&
            (client_normal(c) &&
             client_focus(c)))
        {
            ob_debug_type(OB_DEBUG_FOCUS, "found in pointer stuff\n");
            return c;
        }

    ob_debug_type(OB_DEBUG_FOCUS, "trying omnipresentness\n");
    if (allow_refocus && old &&
        old->desktop == DESKTOP_ALL &&
        client_normal(old) &&
        client_focus(old))
    {
        ob_debug_type(OB_DEBUG_FOCUS, "found in omnipresentness\n");
        return old;
    }


    ob_debug_type(OB_DEBUG_FOCUS, "trying the focus order\n");
    for (it = focus_order; it; it = g_list_next(it)) {
        c = it->data;
        /* fallback focus to a window if:
           1. it is on the current desktop. this ignores omnipresent
           windows, which are problematic in their own rite.
           2. it is a normal type window, don't fall back onto a dock or
           a splashscreen or a desktop window (save the desktop as a
           backup fallback though)
        */
        if (c->desktop == screen_desktop &&
            client_normal(c) &&
            (allow_refocus || c != old) &&
            client_focus(c))
        {
            ob_debug_type(OB_DEBUG_FOCUS, "found in focus order\n");
            return c;
        }
    }

    ob_debug_type(OB_DEBUG_FOCUS, "trying a desktop window\n");
    for (it = focus_order; it; it = g_list_next(it)) {
        c = it->data;
        /* fallback focus to a window if:
           1. it is on the current desktop. this ignores omnipresent
           windows, which are problematic in their own rite.
           2. it is a normal type window, don't fall back onto a dock or
           a splashscreen or a desktop window (save the desktop as a
           backup fallback though)
        */
        if (c->type == OB_CLIENT_TYPE_DESKTOP &&
            (allow_refocus || c != old) &&
            client_focus(c))
        {
            ob_debug_type(OB_DEBUG_FOCUS, "found a desktop window\n");
            return c;
        }
    }

    return NULL;
}

ObClient* focus_fallback(gboolean allow_refocus)
{
    ObClient *new;

    /* unfocus any focused clients.. they can be focused by Pointer events
       and such, and then when we try focus them, we won't get a FocusIn
       event at all for them. */
    focus_nothing();

    new = focus_fallback_target(allow_refocus);

    return new;
}

void focus_nothing()
{
    /* Install our own colormap */
    if (focus_client != NULL) {
        screen_install_colormap(focus_client, FALSE);
        screen_install_colormap(NULL, TRUE);
    }

    /* Don't set focus_client to NULL here. It will be set to NULL when the
       FocusOut event comes. Otherwise, if we focus nothing and then focus the
       same window again, The focus code says nothing changed, but focus_client
       ends up being NULL anyways.
    focus_client = NULL;
    */

    /* if there is a grab going on, then we need to cancel it. if we move
       focus during the grab, applications will get NotifyWhileGrabbed events
       and ignore them !

       actions should not rely on being able to move focus during an
       interactive grab.
    */
    if (keyboard_interactively_grabbed())
        keyboard_interactive_cancel();

    /* when nothing will be focused, send focus to the backup target */
    XSetInputFocus(ob_display, screen_support_win, RevertToPointerRoot,
                   event_curtime);
}

void focus_order_remove(ObClient *c)
{
    focus_order = g_list_remove(focus_order, c);
}

void focus_order_to_top(ObClient *c)
{
    focus_order = g_list_remove(focus_order, c);
    if (!c->iconic) {
        focus_order = g_list_prepend(focus_order, c);
    } else {
        GList *it;

        /* insert before first iconic window */
        for (it = focus_order;
             it && !((ObClient*)it->data)->iconic; it = g_list_next(it));
        focus_order = g_list_insert_before(focus_order, it, c);
    }
}

void focus_order_to_bottom(ObClient *c)
{
    focus_order = g_list_remove(focus_order, c);
    if (c->iconic) {
        focus_order = g_list_append(focus_order, c);
    } else {
        GList *it;

        /* insert before first iconic window */
        for (it = focus_order;
             it && !((ObClient*)it->data)->iconic; it = g_list_next(it));
        focus_order = g_list_insert_before(focus_order, it, c);
    }
}

ObClient *focus_order_find_first(guint desktop)
{
    GList *it;
    for (it = focus_order; it; it = g_list_next(it)) {
        ObClient *c = it->data;
        if (c->desktop == desktop || c->desktop == DESKTOP_ALL)
            return c;
    }
    return NULL;
}

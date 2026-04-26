#include "window_internal.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Individual key handlers                                            */
/* ------------------------------------------------------------------ */

static gboolean handle_enter_key(QuickHelpWindow *qh, GdkModifierType state) {
    if (state & GDK_SHIFT_MASK)
        return FALSE; /* let GtkTextView insert newline */

    if (qh->streaming) {
        /* Cancel current stream; submit will fire when it finishes */
        char *text = get_input_text(qh);
        g_strstrip(text);
        if (*text || qh->pending_images->len > 0) {
            qh->submit_after_cancel = TRUE;
            cancel_stream(qh);
        }
        g_free(text);
        return TRUE;
    }

    char *text = get_input_text(qh);
    g_strstrip(text);
    gboolean empty = !*text;
    g_free(text);

    /* If input is empty and a bubble is focused, open its links */
    if (empty && qh->focused_bubble >= 0 &&
        qh->focused_bubble < (int)qh->bubble_links->len) {
        GPtrArray *links = g_ptr_array_index(qh->bubble_links,
                                              qh->focused_bubble);
        if (links->len == 1) {
            const char *url = g_ptr_array_index(links, 0);
            GtkUriLauncher *launcher = gtk_uri_launcher_new(url);
            gtk_uri_launcher_launch(launcher, qh->window, NULL, NULL, NULL);
            g_object_unref(launcher);
        } else if (links->len > 1) {
            show_link_picker(qh, links);
        }
        return TRUE;
    }

    on_submit(qh);
    return TRUE;
}

static gboolean handle_clipboard_paste(QuickHelpWindow *qh) {
    GdkClipboard *clip = gdk_display_get_clipboard(gdk_display_get_default());
    GdkContentFormats *formats = gdk_clipboard_get_formats(clip);
    if (gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE)) {
        paste_clipboard_image(qh);
        return TRUE;
    }
    return FALSE; /* let text view handle text paste */
}

static gboolean handle_new_conversation(QuickHelpWindow *qh) {
    clear_conversation(qh);
    return TRUE;
}

static gboolean handle_focus_input(QuickHelpWindow *qh) {
    qh->focused_bubble = -1;
    render_conversation(qh, NULL);
    gtk_widget_grab_focus(GTK_WIDGET(qh->text_view));
    return TRUE;
}

static gboolean handle_tab_navigation(QuickHelpWindow *qh,
                                      GdkModifierType state, guint keyval) {
    if (qh->streaming)
        return FALSE;

    gboolean backward = (keyval == GDK_KEY_ISO_Left_Tab ||
                          (state & GDK_SHIFT_MASK));
    GtkWidget *focus = gtk_window_get_focus(qh->window);
    gboolean in_textview = (focus &&
        (focus == GTK_WIDGET(qh->text_view) ||
         gtk_widget_is_ancestor(focus, GTK_WIDGET(qh->text_view))));
    gboolean in_dropdown = (focus &&
        (focus == GTK_WIDGET(qh->model_dropdown) ||
         gtk_widget_is_ancestor(focus, GTK_WIDGET(qh->model_dropdown))));

    if (!backward) {
        if (qh->focused_bubble >= 0) {
            qh->focused_bubble++;
            if (qh->focused_bubble >= qh->bubble_count) {
                qh->focused_bubble = -1;
                render_conversation(qh, NULL);
                gtk_widget_grab_focus(GTK_WIDGET(qh->text_view));
            } else {
                render_conversation(qh, NULL);
            }
            return TRUE;
        } else if (in_textview) {
            gtk_widget_grab_focus(GTK_WIDGET(qh->model_dropdown));
            return TRUE;
        } else if (in_dropdown) {
            if (qh->bubble_count > 0) {
                qh->focused_bubble = 0;
                render_conversation(qh, NULL);
            } else {
                gtk_widget_grab_focus(GTK_WIDGET(qh->text_view));
            }
            return TRUE;
        }
    } else {
        if (qh->focused_bubble >= 0) {
            qh->focused_bubble--;
            if (qh->focused_bubble < 0) {
                render_conversation(qh, NULL);
                gtk_widget_grab_focus(GTK_WIDGET(qh->model_dropdown));
            } else {
                render_conversation(qh, NULL);
            }
            return TRUE;
        } else if (in_textview) {
            if (qh->bubble_count > 0) {
                qh->focused_bubble = qh->bubble_count - 1;
                render_conversation(qh, NULL);
            } else {
                gtk_widget_grab_focus(GTK_WIDGET(qh->model_dropdown));
            }
            return TRUE;
        } else if (in_dropdown) {
            gtk_widget_grab_focus(GTK_WIDGET(qh->text_view));
            return TRUE;
        }
    }

    return FALSE; /* let GTK handle Tab normally */
}

static gboolean handle_jump_to_message(QuickHelpWindow *qh, guint keyval) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(qh->chat_box));
    GPtrArray *user_widgets = g_ptr_array_new();
    for (int i = 0; i < qh->msg_count && child; i++) {
        if (g_strcmp0(qh->messages[i].role, "user") == 0)
            g_ptr_array_add(user_widgets, child);
        child = gtk_widget_get_next_sibling(child);
    }
    if (user_widgets->len > 0) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
        double cur = gtk_adjustment_get_value(adj);
        GtkWidget *target = NULL;
        graphene_point_t p = GRAPHENE_POINT_INIT(0, 0);
        graphene_point_t out;

        if (keyval == GDK_KEY_Down) {
            for (guint i = 0; i < user_widgets->len; i++) {
                GtkWidget *w = g_ptr_array_index(user_widgets, i);
                if (gtk_widget_compute_point(w, GTK_WIDGET(qh->chat_box),
                                             &p, &out) &&
                    out.y > cur + 1.0) { target = w; break; }
            }
        } else {
            for (int i = user_widgets->len - 1; i >= 0; i--) {
                GtkWidget *w = g_ptr_array_index(user_widgets, i);
                if (gtk_widget_compute_point(w, GTK_WIDGET(qh->chat_box),
                                             &p, &out) &&
                    out.y < cur - 1.0) { target = w; break; }
            }
        }
        if (target &&
            gtk_widget_compute_point(target, GTK_WIDGET(qh->chat_box),
                                     &p, &out))
            gtk_adjustment_set_value(adj, out.y);
    }
    g_ptr_array_unref(user_widgets);
    return TRUE;
}

static gboolean handle_scroll_home_end(QuickHelpWindow *qh, guint keyval) {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
    if (keyval == GDK_KEY_Home)
        gtk_adjustment_set_value(adj, gtk_adjustment_get_lower(adj));
    else
        scroll_to_bottom(qh);
    return TRUE;
}

static gboolean handle_scroll_step(QuickHelpWindow *qh, guint keyval) {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
    double val = gtk_adjustment_get_value(adj);
    double step = (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down)
        ? gtk_adjustment_get_page_size(adj)
        : SCROLL_STEP;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Page_Up)
        val -= step;
    else
        val += step;
    double lower = gtk_adjustment_get_lower(adj);
    double upper = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
    if (val < lower) val = lower;
    if (val > upper) val = upper;
    gtk_adjustment_set_value(adj, val);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Key event dispatcher                                               */
/* ------------------------------------------------------------------ */

gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                        guint keycode, GdkModifierType state,
                        gpointer data) {
    (void)ctrl; (void)keycode;
    QuickHelpWindow *qh = data;

    /* Let the model dropdown handle its own keys, except Tab */
    GtkWidget *focus = gtk_window_get_focus(qh->window);
    if (focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(qh->model_dropdown))
        && keyval != GDK_KEY_Tab && keyval != GDK_KEY_ISO_Left_Tab)
        return FALSE;

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
        return handle_enter_key(qh, state);

    if (keyval == GDK_KEY_v && (state & GDK_CONTROL_MASK))
        return handle_clipboard_paste(qh);

    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(qh->window);
        return TRUE;
    }

    if (keyval == GDK_KEY_n && (state & GDK_CONTROL_MASK))
        return handle_new_conversation(qh);

    if (keyval == GDK_KEY_l && (state & GDK_CONTROL_MASK))
        return handle_focus_input(qh);

    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab)
        return handle_tab_navigation(qh, state, keyval);

    if ((keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) &&
        (state & GDK_CONTROL_MASK) && (state & GDK_ALT_MASK))
        return handle_jump_to_message(qh, keyval);

    if ((keyval == GDK_KEY_Home || keyval == GDK_KEY_End) &&
        (state & GDK_CONTROL_MASK))
        return handle_scroll_home_end(qh, keyval);

    if ((keyval == GDK_KEY_Up || keyval == GDK_KEY_Down ||
         keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down) &&
        (state & GDK_CONTROL_MASK))
        return handle_scroll_step(qh, keyval);

    return FALSE;
}

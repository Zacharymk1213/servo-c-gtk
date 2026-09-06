/*
 * ServoGtkWebView: the GTK3 half.
 *
 * Cursors, drawing and input plumbing, plus the built-in dialog, file chooser
 * and context menu windows. Everything else is in servo-gtk-view-common.c.
 */
#include "servo-gtk-view-private.h"

#include <gdk/gdkkeysyms.h>

void
servo_gtk_web_view_on_cursor_changed(const char *name, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    GtkWidget       *widget = GTK_WIDGET(self);
    GdkWindow       *window = gtk_widget_get_window(widget);

    if (window == NULL) {
        return;
    }

    GdkCursor *cursor = gdk_cursor_new_from_name(gtk_widget_get_display(widget), name);
    gdk_window_set_cursor(window, cursor);
    if (cursor != NULL) {
        g_object_unref(cursor);
    }
}

/* Take a dialog window down; ::destroy cancels it if it is still unanswered. */
void
servo_gtk_web_view_destroy_dialog(GtkWidget *window)
{
    gtk_widget_destroy(window);
}

static void
on_script_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
    (void) user_data;

    script_dialog_respond(GTK_WIDGET(dialog), response_id == GTK_RESPONSE_OK);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/*
 * Class closure for ::script-dialog: present the dialog ourselves. Runs only
 * when no handler claimed the dialog by returning TRUE.
 */
gboolean
servo_gtk_web_view_default_script_dialog(ServoGtkWebView          *self,
                                         ServoGtkScriptDialogType  dialog_type,
                                         const gchar              *message,
                                         const gchar              *default_value,
                                         guint64                   request_id)
{
    ScriptDialogClosure *closure;
    GtkWidget           *dialog;
    GtkWidget           *content;
    GtkWidget           *label;
    GtkWidget           *toplevel;
    guint64             *key;

    dialog = gtk_dialog_new();
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    /*
     * The text is page-controlled, so the window is titled generically and the
     * message is shown as a plain, non-markup label: content must not be able
     * to dress its dialog up as browser UI.
     */
    gtk_window_set_title(GTK_WINDOW(dialog), "JavaScript");

    toplevel = gtk_widget_get_toplevel(GTK_WIDGET(self));
    if (toplevel != NULL && gtk_widget_is_toplevel(toplevel)) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(toplevel));
    }

    /* alert() has a single button; confirm() and prompt() can be cancelled. */
    if (dialog_type != SERVO_GTK_SCRIPT_DIALOG_ALERT) {
        gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL);
    }
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_OK", GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_set_spacing(GTK_BOX(content), 12);
    gtk_widget_set_margin_start(content, 18);
    gtk_widget_set_margin_end(content, 18);
    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 18);

    label = gtk_label_new(message != NULL ? message : "");
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    closure = g_new0(ScriptDialogClosure, 1);
    closure->web_view = g_object_ref(self);
    closure->request_id = request_id;

    if (dialog_type == SERVO_GTK_SCRIPT_DIALOG_PROMPT) {
        closure->entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(closure->entry),
                           default_value != NULL ? default_value : "");
        gtk_entry_set_activates_default(GTK_ENTRY(closure->entry), TRUE);
        gtk_box_pack_start(GTK_BOX(content), closure->entry, FALSE, FALSE, 0);
    }

    g_object_set_data_full(G_OBJECT(dialog), "servo-gtk-script-dialog",
                           closure, script_dialog_closure_free);

    g_signal_connect(dialog, "response", G_CALLBACK(on_script_dialog_response), NULL);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_script_dialog_destroy), NULL);

    key = g_new(guint64, 1);
    *key = request_id;
    g_hash_table_insert(self->priv->dialogs, key, dialog);

    gtk_widget_show_all(dialog);
    if (closure->entry != NULL) {
        gtk_widget_grab_focus(closure->entry);
    }

    return TRUE;
}

/* Matches GClosureNotify; released when the response handler goes away. */
static void
file_chooser_closure_free(gpointer data, GClosure *unused)
{
    FileChooserClosure *closure = data;

    (void) unused;

    g_object_unref(closure->web_view);
    g_free(closure);
}

/*
 * The native chooser closed. Turn the selection into the NULL-terminated path
 * array the widget answers with; anything but "accept" answers empty.
 */
static void
on_file_chooser_response(GtkNativeDialog *native, gint response_id, gpointer user_data)
{
    FileChooserClosure *closure = user_data;
    GPtrArray          *paths = g_ptr_array_new_with_free_func(g_free);

    if (response_id == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(native));

        for (GSList *item = files; item != NULL; item = item->next) {
            g_ptr_array_add(paths, item->data);
        }
        g_slist_free(files);
    }

    g_ptr_array_add(paths, NULL);
    servo_gtk_web_view_respond_to_file_chooser(
        closure->web_view, closure->request_id, (const gchar *const *) paths->pdata);

    g_ptr_array_free(paths, TRUE);
    g_object_unref(native);
}

/*
 * Class closure for ::run-file-chooser: present the chooser ourselves. Runs
 * only when no handler claimed it by returning TRUE.
 */
gboolean
servo_gtk_web_view_default_run_file_chooser(ServoGtkWebView    *self,
                                            const gchar *const *filter_patterns,
                                            gboolean            allow_multiple,
                                            guint64             request_id)
{
    FileChooserClosure  *closure;
    GtkFileChooserNative *native;
    GtkFileFilter       *filter;
    GtkWidget           *toplevel;
    GtkWindow           *parent = NULL;

    toplevel = gtk_widget_get_toplevel(GTK_WIDGET(self));
    if (toplevel != NULL && gtk_widget_is_toplevel(toplevel)) {
        parent = GTK_WINDOW(toplevel);
    }

    native = gtk_file_chooser_native_new("Select File", parent,
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         "_Open", "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(native), allow_multiple);

    filter = servo_gtk_web_view_build_file_filter(filter_patterns);
    if (filter != NULL) {
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(native), filter);
    }

    closure = g_new0(FileChooserClosure, 1);
    closure->web_view = g_object_ref(self);
    closure->request_id = request_id;

    g_signal_connect_data(native, "response",
                          G_CALLBACK(on_file_chooser_response), closure,
                          file_chooser_closure_free, 0);

    gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));

    return TRUE;
}

static void
on_auth_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
    (void) user_data;

    auth_respond(GTK_WIDGET(dialog), response_id == GTK_RESPONSE_OK);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/* Class closure for ::authenticate: prompt for credentials ourselves. */
gboolean
servo_gtk_web_view_default_authenticate(ServoGtkWebView *self,
                                        const gchar     *uri,
                                        gboolean         for_proxy,
                                        guint64          request_id)
{
    AuthClosure *closure;
    GtkWidget   *dialog;
    GtkWidget   *content;
    GtkWidget   *label;
    GtkWidget   *toplevel;
    gchar       *text;
    guint64     *key;

    dialog = gtk_dialog_new();
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_title(GTK_WINDOW(dialog), "Authentication Required");

    toplevel = gtk_widget_get_toplevel(GTK_WIDGET(self));
    if (toplevel != NULL && gtk_widget_is_toplevel(toplevel)) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(toplevel));
    }

    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Authenticate", GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_set_spacing(GTK_BOX(content), 12);
    gtk_widget_set_margin_start(content, 18);
    gtk_widget_set_margin_end(content, 18);
    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 18);

    /*
     * Say plainly which side is asking: credentials meant for an origin must
     * not be handed to a proxy, or the other way round.
     */
    text = g_strdup_printf(for_proxy
                               ? "The proxy for %s requires a username and password."
                               : "%s requires a username and password.",
                           uri != NULL ? uri : "this site");
    label = gtk_label_new(text);
    g_free(text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    closure = g_new0(AuthClosure, 1);
    closure->web_view = g_object_ref(self);
    closure->request_id = request_id;

    closure->username = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(closure->username), "Username");
    gtk_entry_set_activates_default(GTK_ENTRY(closure->username), TRUE);
    gtk_box_pack_start(GTK_BOX(content), closure->username, FALSE, FALSE, 0);

    closure->password = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(closure->password), "Password");
    gtk_entry_set_visibility(GTK_ENTRY(closure->password), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(closure->password), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(closure->password), TRUE);
    gtk_box_pack_start(GTK_BOX(content), closure->password, FALSE, FALSE, 0);

    g_object_set_data_full(G_OBJECT(dialog), "servo-gtk-auth",
                           closure, auth_closure_free);

    g_signal_connect(dialog, "response", G_CALLBACK(on_auth_response), NULL);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_auth_destroy), NULL);

    /* Share the dialog table so widget disposal takes this window down too. */
    key = g_new(guint64, 1);
    *key = request_id;
    g_hash_table_insert(self->priv->dialogs, key, dialog);

    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(closure->username);

    return TRUE;
}

static void
on_context_menu_item_activate(GtkMenuItem *item, gpointer user_data)
{
    GtkWidget *menu = user_data;
    gsize      index =
        GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(item), "servo-gtk-item-index"));

    context_menu_respond(menu, index);
}

static void
on_context_menu_selection_done(GtkMenuShell *menu, gpointer user_data)
{
    (void) user_data;

    /* Closing without a choice dismisses; choosing already answered. */
    context_menu_respond(GTK_WIDGET(menu), SERVO_GTK_CONTEXT_MENU_NO_SELECTION);

    gtk_widget_destroy(GTK_WIDGET(menu));
}

/* Class closure for ::context-menu: present the menu ourselves. */
gboolean
servo_gtk_web_view_default_context_menu(ServoGtkWebView    *self,
                                        gint                x,
                                        gint                y,
                                        const gchar *const *labels,
                                        guint64             request_id)
{
    const ServoContextMenuItem *items = self->priv->menu_items;
    gsize                       item_count = self->priv->menu_item_count;
    ContextMenuClosure         *closure;
    GtkWidget                  *menu;
    GdkRectangle                anchor = { x, y, 1, 1 };
    GdkWindow                  *window;

    (void) labels;

    if (items == NULL || item_count == 0) {
        /* Nothing to show; dismissing keeps the page from waiting on us. */
        servo_gtk_web_view_respond_to_context_menu(
            self, request_id, SERVO_GTK_CONTEXT_MENU_NO_SELECTION);
        return TRUE;
    }

    menu = gtk_menu_new();

    for (gsize i = 0; i < item_count; i++) {
        GtkWidget *item;

        if (items[i].label == NULL) {
            item = gtk_separator_menu_item_new();
        } else {
            item = gtk_menu_item_new_with_label(items[i].label);
            gtk_widget_set_sensitive(item, items[i].enabled);
            g_object_set_data(G_OBJECT(item), "servo-gtk-item-index", GSIZE_TO_POINTER(i));
            g_signal_connect(item, "activate",
                             G_CALLBACK(on_context_menu_item_activate), menu);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    closure = g_new0(ContextMenuClosure, 1);
    closure->web_view = g_object_ref(self);
    closure->request_id = request_id;
    g_object_set_data_full(G_OBJECT(menu), "servo-gtk-context-menu",
                           closure, context_menu_closure_free);

    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(self), NULL);
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(on_context_menu_selection_done), NULL);
    gtk_widget_show_all(menu);

    window = gtk_widget_get_window(GTK_WIDGET(self));
    if (window != NULL) {
        gtk_menu_popup_at_rect(GTK_MENU(menu), window, &anchor,
                               GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
    } else {
        gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(self),
                                 GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
    }

    return TRUE;
}

static gboolean
servo_gtk_web_view_draw(GtkWidget *widget, cairo_t *cr)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    if (self->frame != NULL) {
        int width = gtk_widget_get_allocated_width(widget);
        int height = gtk_widget_get_allocated_height(widget);
        int frame_width = gdk_pixbuf_get_width(self->frame);
        int frame_height = gdk_pixbuf_get_height(self->frame);

        /*
         * Servo renders at device resolution while cairo draws in logical
         * units, so on a HiDPI display the frame is larger than the widget.
         * Scaling by the measured ratio rather than by the scale factor also
         * stretches a frame that is still at the pre-resize size, instead of
         * painting it 1:1 in a corner until the next one arrives.
         */
        cairo_save(cr);
        if (frame_width > 0 && frame_height > 0 &&
            (frame_width != width || frame_height != height)) {
            cairo_scale(cr,
                        (double) width / frame_width,
                        (double) height / frame_height);
        }
        gdk_cairo_set_source_pixbuf(cr, self->frame, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        /* No frame yet: paint a neutral background. */
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);
    }

    return FALSE;
}

/*
 * The Servo instance is created lazily on the first allocation, when the real
 * widget size is known, and resized on subsequent allocations.
 */
static void
servo_gtk_web_view_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
    servo_gtk_web_view_parent_widget_class()->size_allocate(widget, allocation);

    servo_gtk_web_view_sync_surface(
        SERVO_GTK_WEB_VIEW(widget), allocation->width, allocation->height);
}

static gboolean
servo_gtk_web_view_motion_notify(GtkWidget *widget, GdkEventMotion *event)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    if (self->servo != NULL) {
        servo_webview_pointer_move(self->servo,
                                   servo_gtk_web_view_to_device(self, event->x),
                                   servo_gtk_web_view_to_device(self, event->y));
    }

    return TRUE;
}

static gboolean
servo_gtk_web_view_button_press(GtkWidget *widget, GdkEventButton *event)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    gtk_widget_grab_focus(widget);

    if (self->servo != NULL) {
        servo_webview_pointer_button(self->servo,
                                     event->button,
                                     TRUE,
                                     servo_gtk_web_view_to_device(self, event->x),
                                     servo_gtk_web_view_to_device(self, event->y));
    }

    return TRUE;
}

static gboolean
servo_gtk_web_view_button_release(GtkWidget *widget, GdkEventButton *event)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    if (self->servo != NULL) {
        servo_webview_pointer_button(self->servo,
                                     event->button,
                                     FALSE,
                                     servo_gtk_web_view_to_device(self, event->x),
                                     servo_gtk_web_view_to_device(self, event->y));
    }

    return TRUE;
}

static gboolean
servo_gtk_web_view_scroll(GtkWidget *widget, GdkEventScroll *event)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);
    gdouble dx = 0.0;
    gdouble dy = 0.0;

    if (!gdk_event_get_scroll_deltas((GdkEvent *) event, &dx, &dy)) {
        switch (event->direction) {
        case GDK_SCROLL_UP:    dy = -1.0; break;
        case GDK_SCROLL_DOWN:  dy =  1.0; break;
        case GDK_SCROLL_LEFT:  dx = -1.0; break;
        case GDK_SCROLL_RIGHT: dx =  1.0; break;
        default: break;
        }
    }

    if (self->servo != NULL) {
        servo_webview_scroll(self->servo,
                             servo_gtk_web_view_to_device(self, dx),
                             servo_gtk_web_view_to_device(self, dy));
    }

    return TRUE;
}

static gboolean
servo_gtk_web_view_key_press(GtkWidget *widget, GdkEventKey *event)
{
    return servo_gtk_web_view_key(
        SERVO_GTK_WEB_VIEW(widget), event->keyval, event->state, TRUE);
}

static gboolean
servo_gtk_web_view_key_release(GtkWidget *widget, GdkEventKey *event)
{
    return servo_gtk_web_view_key(
        SERVO_GTK_WEB_VIEW(widget), event->keyval, event->state, FALSE);
}

/*
 * GTK3 delivers each touch point as a GdkEventTouch carrying its own sequence.
 * Returning FALSE lets GTK go on synthesising pointer events from the first
 * touch, so a page with no touch handling still works with a finger.
 */
static gboolean
servo_gtk_web_view_touch_event(GtkWidget *widget, GdkEventTouch *event)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);
    ServoTouchPhase  phase;

    switch (event->type) {
    case GDK_TOUCH_BEGIN:  phase = SERVO_TOUCH_DOWN; break;
    case GDK_TOUCH_UPDATE: phase = SERVO_TOUCH_MOVE; break;
    case GDK_TOUCH_END:    phase = SERVO_TOUCH_UP; break;
    case GDK_TOUCH_CANCEL: phase = SERVO_TOUCH_CANCEL; break;
    default:
        return FALSE;
    }

    servo_gtk_web_view_touch(self, phase, event->sequence, event->x, event->y);

    return FALSE;
}

void
servo_gtk_web_view_class_init_toolkit(ServoGtkWebViewClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    /* GTK3 draws and receives input through widget class vfuncs. */
    widget_class->draw = servo_gtk_web_view_draw;
    widget_class->size_allocate = servo_gtk_web_view_size_allocate;
    widget_class->motion_notify_event = servo_gtk_web_view_motion_notify;
    widget_class->button_press_event = servo_gtk_web_view_button_press;
    widget_class->button_release_event = servo_gtk_web_view_button_release;
    widget_class->scroll_event = servo_gtk_web_view_scroll;
    widget_class->key_press_event = servo_gtk_web_view_key_press;
    widget_class->key_release_event = servo_gtk_web_view_key_release;
    widget_class->touch_event = servo_gtk_web_view_touch_event;
}

void
servo_gtk_web_view_init_toolkit(ServoGtkWebView *self)
{
    GtkWidget *widget = GTK_WIDGET(self);

    gtk_widget_set_can_focus(widget, TRUE);

    /* GTK3 only delivers the events the widget has asked for. */
    gtk_widget_add_events(
        widget,
        GDK_POINTER_MOTION_MASK
        | GDK_BUTTON_PRESS_MASK
        | GDK_BUTTON_RELEASE_MASK
        | GDK_SCROLL_MASK
        | GDK_SMOOTH_SCROLL_MASK
        | GDK_KEY_PRESS_MASK
        | GDK_KEY_RELEASE_MASK
        | GDK_TOUCH_MASK
    );
}

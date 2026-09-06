/*
 * ServoGtkWebView: the GTK4 half.
 *
 * Cursors, drawing and input plumbing, plus the built-in dialog, file chooser
 * and context menu windows. Everything else is in servo-gtk-view-common.c.
 */
#include "servo-gtk-view-private.h"

#include <gdk/gdkkeysyms.h>

/*
 * Servo asked the embedder to change the pointer cursor. GTK4 resolves a named
 * cursor for the widget directly, with no GdkWindow round-trip.
 */
void
servo_gtk_web_view_on_cursor_changed(const char *name, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), name);
}

/* Take a dialog window down; ::destroy cancels it if it is still unanswered. */
void
servo_gtk_web_view_destroy_dialog(GtkWidget *window)
{
    gtk_window_destroy(GTK_WINDOW(window));
}

static void
on_script_dialog_accept(GtkButton *button, gpointer user_data)
{
    (void) button;

    script_dialog_respond(GTK_WIDGET(user_data), TRUE);
    gtk_window_destroy(GTK_WINDOW(user_data));
}

static void
on_script_dialog_cancel(GtkButton *button, gpointer user_data)
{
    (void) button;

    script_dialog_respond(GTK_WIDGET(user_data), FALSE);
    gtk_window_destroy(GTK_WINDOW(user_data));
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
    GtkWidget           *window;
    GtkWidget           *box;
    GtkWidget           *label;
    GtkWidget           *button_box;
    GtkWidget           *accept_button;
    GtkRoot             *root;
    guint64             *key;

    window = gtk_window_new();
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    /*
     * The text is page-controlled, so the window is titled generically and the
     * message is shown as a plain, non-markup label: content must not be able
     * to dress its dialog up as browser UI.
     */
    gtk_window_set_title(GTK_WINDOW(window), "JavaScript");

    root = gtk_widget_get_root(GTK_WIDGET(self));
    if (root != NULL && GTK_IS_WINDOW(root)) {
        gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(root));
    }

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_window_set_child(GTK_WINDOW(window), box);

    label = gtk_label_new(message != NULL ? message : "");
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_append(GTK_BOX(box), label);

    closure = g_new0(ScriptDialogClosure, 1);
    closure->web_view = g_object_ref(self);
    closure->request_id = request_id;

    if (dialog_type == SERVO_GTK_SCRIPT_DIALOG_PROMPT) {
        closure->entry = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(closure->entry),
                              default_value != NULL ? default_value : "");
        gtk_entry_set_activates_default(GTK_ENTRY(closure->entry), TRUE);
        gtk_box_append(GTK_BOX(box), closure->entry);
    }

    g_object_set_data_full(G_OBJECT(window), "servo-gtk-script-dialog",
                           closure, script_dialog_closure_free);

    button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(button_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), button_box);

    /* alert() has a single button; confirm() and prompt() can be cancelled. */
    if (dialog_type != SERVO_GTK_SCRIPT_DIALOG_ALERT) {
        GtkWidget *cancel_button = gtk_button_new_with_mnemonic("_Cancel");
        g_signal_connect(cancel_button, "clicked",
                         G_CALLBACK(on_script_dialog_cancel), window);
        gtk_box_append(GTK_BOX(button_box), cancel_button);
    }

    accept_button = gtk_button_new_with_mnemonic("_OK");
    gtk_widget_add_css_class(accept_button, "suggested-action");
    g_signal_connect(accept_button, "clicked",
                     G_CALLBACK(on_script_dialog_accept), window);
    gtk_box_append(GTK_BOX(button_box), accept_button);

    g_signal_connect(window, "destroy", G_CALLBACK(on_script_dialog_destroy), NULL);

    key = g_new(guint64, 1);
    *key = request_id;
    g_hash_table_insert(self->priv->dialogs, key, window);

    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(closure->entry != NULL ? closure->entry : accept_button);

    return TRUE;
}

/*
 * GtkFileDialog finished. Turn the result into the NULL-terminated path array
 * the widget answers with; an error (including the user dismissing the dialog)
 * answers with no selection.
 */
static void
on_file_chooser_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FileChooserClosure *closure = user_data;
    GtkFileDialog      *dialog = GTK_FILE_DIALOG(source);
    GPtrArray          *paths = g_ptr_array_new_with_free_func(g_free);
    GListModel         *files = NULL;
    GFile              *file = NULL;

    if (g_object_get_data(G_OBJECT(dialog), "servo-gtk-allow-multiple") != NULL) {
        files = gtk_file_dialog_open_multiple_finish(dialog, result, NULL);
    } else {
        file = gtk_file_dialog_open_finish(dialog, result, NULL);
    }

    if (files != NULL) {
        guint n_files = g_list_model_get_n_items(files);

        for (guint i = 0; i < n_files; i++) {
            GFile *item = g_list_model_get_item(files, i);
            gchar *path = g_file_get_path(item);

            /* Servo takes filesystem paths, so a non-local file is unusable. */
            if (path != NULL) {
                g_ptr_array_add(paths, path);
            }
            g_object_unref(item);
        }
        g_object_unref(files);
    } else if (file != NULL) {
        gchar *path = g_file_get_path(file);

        if (path != NULL) {
            g_ptr_array_add(paths, path);
        }
        g_object_unref(file);
    }

    g_ptr_array_add(paths, NULL);
    servo_gtk_web_view_respond_to_file_chooser(
        closure->web_view, closure->request_id, (const gchar *const *) paths->pdata);

    g_ptr_array_free(paths, TRUE);
    g_object_unref(closure->web_view);
    g_free(closure);
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
    FileChooserClosure *closure;
    GtkFileDialog      *dialog;
    GtkFileFilter      *filter;
    GtkRoot            *root;
    GtkWindow          *parent = NULL;

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select File");
    gtk_file_dialog_set_modal(dialog, TRUE);

    filter = servo_gtk_web_view_build_file_filter(filter_patterns);
    if (filter != NULL) {
        GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

        g_list_store_append(filters, filter);
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        gtk_file_dialog_set_default_filter(dialog, filter);
        g_object_unref(filters);
        g_object_unref(filter);
    }

    root = gtk_widget_get_root(GTK_WIDGET(self));
    if (root != NULL && GTK_IS_WINDOW(root)) {
        parent = GTK_WINDOW(root);
    }

    closure = g_new0(FileChooserClosure, 1);
    closure->web_view = g_object_ref(self);
    closure->request_id = request_id;

    if (allow_multiple) {
        /* Read back in the finish callback to pick the matching _finish(). */
        g_object_set_data(G_OBJECT(dialog), "servo-gtk-allow-multiple",
                          GINT_TO_POINTER(1));
        gtk_file_dialog_open_multiple(dialog, parent, NULL,
                                      on_file_chooser_ready, closure);
    } else {
        gtk_file_dialog_open(dialog, parent, NULL, on_file_chooser_ready, closure);
    }

    g_object_unref(dialog);

    return TRUE;
}

static void
on_auth_accept(GtkButton *button, gpointer user_data)
{
    (void) button;

    auth_respond(GTK_WIDGET(user_data), TRUE);
    gtk_window_destroy(GTK_WINDOW(user_data));
}

static void
on_auth_cancel(GtkButton *button, gpointer user_data)
{
    (void) button;

    auth_respond(GTK_WIDGET(user_data), FALSE);
    gtk_window_destroy(GTK_WINDOW(user_data));
}

/* Class closure for ::authenticate: prompt for credentials ourselves. */
gboolean
servo_gtk_web_view_default_authenticate(ServoGtkWebView *self,
                                        const gchar     *uri,
                                        gboolean         for_proxy,
                                        guint64          request_id)
{
    AuthClosure *closure;
    GtkWidget   *window;
    GtkWidget   *box;
    GtkWidget   *label;
    GtkWidget   *button_box;
    GtkWidget   *accept_button;
    GtkWidget   *cancel_button;
    GtkRoot     *root;
    gchar       *text;
    guint64     *key;

    window = gtk_window_new();
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_title(GTK_WINDOW(window), "Authentication Required");

    root = gtk_widget_get_root(GTK_WIDGET(self));
    if (root != NULL && GTK_IS_WINDOW(root)) {
        gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(root));
    }

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_window_set_child(GTK_WINDOW(window), box);

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
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_append(GTK_BOX(box), label);

    closure = g_new0(AuthClosure, 1);
    closure->web_view = g_object_ref(self);
    closure->request_id = request_id;

    closure->username = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(closure->username), "Username");
    gtk_box_append(GTK_BOX(box), closure->username);

    closure->password = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(closure->password), "Password");
    gtk_entry_set_visibility(GTK_ENTRY(closure->password), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(closure->password), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_box_append(GTK_BOX(box), closure->password);

    g_object_set_data_full(G_OBJECT(window), "servo-gtk-auth",
                           closure, auth_closure_free);

    button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(button_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), button_box);

    cancel_button = gtk_button_new_with_mnemonic("_Cancel");
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(on_auth_cancel), window);
    gtk_box_append(GTK_BOX(button_box), cancel_button);

    accept_button = gtk_button_new_with_mnemonic("_Authenticate");
    gtk_widget_add_css_class(accept_button, "suggested-action");
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_auth_accept), window);
    gtk_box_append(GTK_BOX(button_box), accept_button);

    g_signal_connect(window, "destroy", G_CALLBACK(on_auth_destroy), NULL);

    /* Share the dialog table so widget disposal takes this window down too. */
    key = g_new(guint64, 1);
    *key = request_id;
    g_hash_table_insert(self->priv->dialogs, key, window);

    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(closure->username);

    return TRUE;
}

/* Drop the popover once GTK has finished with it. */
static gboolean
context_menu_unparent(gpointer data)
{
    GtkWidget *popover = data;

    gtk_widget_unparent(popover);
    g_object_unref(popover);

    return G_SOURCE_REMOVE;
}

static void
on_context_menu_closed(GtkPopover *popover, gpointer user_data)
{
    (void) user_data;

    /* Closing without a choice dismisses; choosing already answered. */
    context_menu_respond(GTK_WIDGET(popover), SERVO_GTK_CONTEXT_MENU_NO_SELECTION);

    /* Unparenting from inside ::closed is not safe, so defer it. */
    g_idle_add(context_menu_unparent, g_object_ref(popover));
}

static void
on_context_menu_item_activate(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    GtkWidget *popover = user_data;
    gsize      index =
        GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(action), "servo-gtk-item-index"));

    (void) parameter;

    context_menu_respond(popover, index);
    gtk_popover_popdown(GTK_POPOVER(popover));
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
    GtkWidget                  *popover;
    GSimpleActionGroup         *actions;
    GMenu                      *menu;
    GMenu                      *section;
    GdkRectangle                anchor = { x, y, 1, 1 };

    (void) labels;

    if (items == NULL || item_count == 0) {
        /* Nothing to show; dismissing keeps the page from waiting on us. */
        servo_gtk_web_view_respond_to_context_menu(
            self, request_id, SERVO_GTK_CONTEXT_MENU_NO_SELECTION);
        return TRUE;
    }

    popover = gtk_popover_menu_new_from_model(NULL);
    actions = g_simple_action_group_new();
    menu = g_menu_new();
    section = g_menu_new();

    for (gsize i = 0; i < item_count; i++) {
        GSimpleAction *action;
        GMenuItem     *menu_item;
        gchar         *action_name;
        gchar         *detailed_name;

        if (items[i].label == NULL) {
            /* A separator closes the current section and opens the next. */
            g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
            g_object_unref(section);
            section = g_menu_new();
            continue;
        }

        action_name = g_strdup_printf("item%" G_GSIZE_FORMAT, i);
        action = g_simple_action_new(action_name, NULL);
        /*
         * A disabled action is what greys the item out: GMenu carries no
         * per-item sensitivity of its own.
         */
        g_simple_action_set_enabled(action, items[i].enabled);
        g_object_set_data(G_OBJECT(action), "servo-gtk-item-index", GSIZE_TO_POINTER(i));
        g_signal_connect(action, "activate",
                         G_CALLBACK(on_context_menu_item_activate), popover);
        g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));
        g_object_unref(action);

        detailed_name = g_strconcat("servo.", action_name, NULL);
        menu_item = g_menu_item_new(items[i].label, detailed_name);
        g_menu_append_item(section, menu_item);
        g_object_unref(menu_item);
        g_free(detailed_name);
        g_free(action_name);
    }

    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);

    gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(popover), G_MENU_MODEL(menu));
    g_object_unref(menu);

    gtk_widget_insert_action_group(popover, "servo", G_ACTION_GROUP(actions));
    g_object_unref(actions);

    closure = g_new0(ContextMenuClosure, 1);
    closure->web_view = g_object_ref(self);
    closure->request_id = request_id;
    g_object_set_data_full(G_OBJECT(popover), "servo-gtk-context-menu",
                           closure, context_menu_closure_free);

    gtk_widget_set_parent(popover, GTK_WIDGET(self));
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &anchor);
    g_signal_connect(popover, "closed", G_CALLBACK(on_context_menu_closed), NULL);

    gtk_popover_popup(GTK_POPOVER(popover));

    return TRUE;
}

/*
 * GtkDrawingArea draw func (GTK4): the widget snapshots itself into this cairo
 * context. Paint the latest Servo frame, or a neutral background if none has
 * arrived yet.
 */
static void
servo_gtk_web_view_draw(GtkDrawingArea *area,
                        cairo_t        *cr,
                        int             width,
                        int             height,
                        gpointer        user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(area);

    (void) user_data;

    if (self->frame != NULL) {
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
}

/*
 * GtkDrawingArea::resize (GTK4) reports the widget's new logical size. The
 * Servo instance is created lazily here, on the first resize, when the real
 * widget size is known.
 */
static void
servo_gtk_web_view_on_resize(GtkDrawingArea *area,
                             int             width,
                             int             height,
                             gpointer        user_data)
{
    (void) user_data;

    servo_gtk_web_view_sync_surface(SERVO_GTK_WEB_VIEW(area), width, height);
}

/* GtkEventControllerMotion::motion: forward the pointer position to Servo. */
static void
servo_gtk_web_view_on_motion(GtkEventControllerMotion *controller,
                             gdouble                   x,
                             gdouble                   y,
                             gpointer                  user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) controller;

    if (self->servo != NULL) {
        servo_webview_pointer_move(self->servo,
                                   servo_gtk_web_view_to_device(self, x),
                                   servo_gtk_web_view_to_device(self, y));
    }
}

/* GtkGestureClick::pressed: grab focus and forward the button press to Servo. */
static void
servo_gtk_web_view_on_pressed(GtkGestureClick *gesture,
                              gint             n_press,
                              gdouble          x,
                              gdouble          y,
                              gpointer         user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) n_press;

    gtk_widget_grab_focus(GTK_WIDGET(self));

    if (self->servo != NULL) {
        guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
        servo_webview_pointer_button(self->servo,
                                     button,
                                     TRUE,
                                     servo_gtk_web_view_to_device(self, x),
                                     servo_gtk_web_view_to_device(self, y));
    }
}

/* GtkGestureClick::released: forward the button release to Servo. */
static void
servo_gtk_web_view_on_released(GtkGestureClick *gesture,
                               gint             n_press,
                               gdouble          x,
                               gdouble          y,
                               gpointer         user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) n_press;

    if (self->servo != NULL) {
        guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
        servo_webview_pointer_button(self->servo,
                                     button,
                                     FALSE,
                                     servo_gtk_web_view_to_device(self, x),
                                     servo_gtk_web_view_to_device(self, y));
    }
}

/*
 * GtkEventControllerScroll::scroll: the controller already delivers scroll
 * deltas (dx, dy), including smooth-scroll steps, so no direction decoding is
 * needed as it was under GTK3's GdkEventScroll.
 */
static gboolean
servo_gtk_web_view_on_scroll(GtkEventControllerScroll *controller,
                             gdouble                   dx,
                             gdouble                   dy,
                             gpointer                  user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) controller;

    if (self->servo != NULL) {
        servo_webview_scroll(self->servo,
                             servo_gtk_web_view_to_device(self, dx),
                             servo_gtk_web_view_to_device(self, dy));
    }

    return TRUE;
}

/* GtkEventControllerKey::key-pressed (returns whether the key was handled). */
static gboolean
servo_gtk_web_view_on_key_pressed(GtkEventControllerKey *controller,
                                  guint                  keyval,
                                  guint                  keycode,
                                  GdkModifierType        state,
                                  gpointer               user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) controller;
    (void) keycode;

    return servo_gtk_web_view_key(self, keyval, state, TRUE);
}

/* GtkEventControllerKey::key-released (void return). */
static void
servo_gtk_web_view_on_key_released(GtkEventControllerKey *controller,
                                   guint                  keyval,
                                   guint                  keycode,
                                   GdkModifierType        state,
                                   gpointer               user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) controller;
    (void) keycode;

    servo_gtk_web_view_key(self, keyval, state, FALSE);
}

void
servo_gtk_web_view_class_init_toolkit(ServoGtkWebViewClass *klass)
{
    /*
     * Nothing to override: GTK4 draws through a draw func and receives input
     * through event controllers, both set up per instance below rather than as
     * class vfuncs.
     */
    (void) klass;
}

void
servo_gtk_web_view_init_toolkit(ServoGtkWebView *self)
{
    GtkWidget *widget = GTK_WIDGET(self);

    gtk_widget_set_focusable(widget, TRUE);

    /* GTK4: the drawing area renders through a draw func rather than a vfunc. */
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(self), servo_gtk_web_view_draw, NULL, NULL);

    /* Lazily create/resize the Servo instance as the widget is allocated. */
    g_signal_connect(self, "resize", G_CALLBACK(servo_gtk_web_view_on_resize), NULL);

    /*
     * GTK4 delivers input through event controllers rather than event masks and
     * per-event widget vfuncs. Each controller is owned by the widget once
     * added, so there is nothing to explicitly unref.
     */
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(servo_gtk_web_view_on_motion), self);
    gtk_widget_add_controller(widget, motion);

    GtkGesture *click = gtk_gesture_click_new();
    /* Button 0 == report every button, matching the GTK3 button handlers. */
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(servo_gtk_web_view_on_pressed), self);
    g_signal_connect(click, "released", G_CALLBACK(servo_gtk_web_view_on_released), self);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));

    GtkEventController *scroll =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(servo_gtk_web_view_on_scroll), self);
    gtk_widget_add_controller(widget, scroll);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(servo_gtk_web_view_on_key_pressed), self);
    g_signal_connect(key, "key-released", G_CALLBACK(servo_gtk_web_view_on_key_released), self);
    gtk_widget_add_controller(widget, key);
}

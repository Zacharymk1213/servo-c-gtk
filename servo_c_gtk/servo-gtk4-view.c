#include "servo-gtk4-view.h"

#include "servo-webview.h"

#include <gdk/gdkkeysyms.h>

enum {
    PROP_0,
    PROP_URI,
    PROP_TITLE,
    PROP_IS_LOADING,
    PROP_CAN_GO_BACK,
    PROP_CAN_GO_FORWARD,
    PROP_ZOOM_LEVEL,
    N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = { NULL };

enum {
    URI_CHANGED,
    LOAD_CHANGED,
    SCRIPT_DIALOG,
    SCRIPT_DIALOG_CANCELLED,
    RUN_FILE_CHOOSER,
    AUTHENTICATE,
    PERMISSION_REQUEST,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/*
 * Embedder-visible page state mirrored from Servo. Kept behind the instance's
 * `priv` pointer rather than in the public struct so it can grow without
 * changing the size of #ServoGtkWebView.
 */
struct _ServoGtkWebViewPrivate {
    gchar    *title;
    gboolean  is_loading;
    gboolean  can_go_back;
    gboolean  can_go_forward;
    /*
     * Mirrored rather than read back from Servo so the property is readable
     * before the widget is allocated and Servo exists.
     */
    gdouble   zoom_level;
    /*
     * Dialog windows the built-in handler put up, keyed by request id, so a
     * dialog Servo withdraws can be taken back down. Values are unowned: each
     * window removes itself from here when it is destroyed.
     */
    GHashTable *dialogs;
};

/*
 * Register ServoGtkLoadEvent as a GType so the ::load-changed signal carries a
 * proper enumeration rather than a bare integer.
 */
GType
servo_gtk_load_event_get_type(void)
{
    static gsize type_id = 0;

    if (g_once_init_enter(&type_id)) {
        static const GEnumValue values[] = {
            { SERVO_GTK_LOAD_STARTED,   "SERVO_GTK_LOAD_STARTED",   "started" },
            { SERVO_GTK_LOAD_COMMITTED, "SERVO_GTK_LOAD_COMMITTED", "committed" },
            { SERVO_GTK_LOAD_FINISHED,  "SERVO_GTK_LOAD_FINISHED",  "finished" },
            { 0, NULL, NULL }
        };
        GType id = g_enum_register_static("ServoGtkLoadEvent", values);
        g_once_init_leave(&type_id, id);
    }

    return (GType) type_id;
}

/*
 * Register ServoGtkScriptDialogType as a GType so the ::script-dialog signal
 * carries a proper enumeration rather than a bare integer.
 */
GType
servo_gtk_script_dialog_type_get_type(void)
{
    static gsize type_id = 0;

    if (g_once_init_enter(&type_id)) {
        static const GEnumValue values[] = {
            { SERVO_GTK_SCRIPT_DIALOG_ALERT,
              "SERVO_GTK_SCRIPT_DIALOG_ALERT",   "alert" },
            { SERVO_GTK_SCRIPT_DIALOG_CONFIRM,
              "SERVO_GTK_SCRIPT_DIALOG_CONFIRM", "confirm" },
            { SERVO_GTK_SCRIPT_DIALOG_PROMPT,
              "SERVO_GTK_SCRIPT_DIALOG_PROMPT",  "prompt" },
            { 0, NULL, NULL }
        };
        GType id = g_enum_register_static("ServoGtkScriptDialogType", values);
        g_once_init_leave(&type_id, id);
    }

    return (GType) type_id;
}

/*
 * Register ServoGtkPermissionFeature as a GType so the ::permission-request
 * signal carries a proper enumeration rather than a bare integer.
 */
GType
servo_gtk_permission_feature_get_type(void)
{
    static gsize type_id = 0;

    if (g_once_init_enter(&type_id)) {
        static const GEnumValue values[] = {
#define SERVO_GTK_PERMISSION_VALUE(name, nick) \
            { SERVO_GTK_PERMISSION_##name, "SERVO_GTK_PERMISSION_" #name, nick }
            SERVO_GTK_PERMISSION_VALUE(GEOLOCATION,        "geolocation"),
            SERVO_GTK_PERMISSION_VALUE(NOTIFICATIONS,      "notifications"),
            SERVO_GTK_PERMISSION_VALUE(PUSH,               "push"),
            SERVO_GTK_PERMISSION_VALUE(MIDI,               "midi"),
            SERVO_GTK_PERMISSION_VALUE(CAMERA,             "camera"),
            SERVO_GTK_PERMISSION_VALUE(MICROPHONE,         "microphone"),
            SERVO_GTK_PERMISSION_VALUE(SPEAKER,            "speaker"),
            SERVO_GTK_PERMISSION_VALUE(DEVICE_INFO,        "device-info"),
            SERVO_GTK_PERMISSION_VALUE(BACKGROUND_SYNC,    "background-sync"),
            SERVO_GTK_PERMISSION_VALUE(BLUETOOTH,          "bluetooth"),
            SERVO_GTK_PERMISSION_VALUE(PERSISTENT_STORAGE, "persistent-storage"),
            SERVO_GTK_PERMISSION_VALUE(SCREEN_WAKE_LOCK,   "screen-wake-lock"),
#undef SERVO_GTK_PERMISSION_VALUE
            { 0, NULL, NULL }
        };
        GType id = g_enum_register_static("ServoGtkPermissionFeature", values);
        g_once_init_leave(&type_id, id);
    }

    return (GType) type_id;
}

G_DEFINE_TYPE(ServoGtkWebView, servo_gtk_web_view, GTK_TYPE_DRAWING_AREA)

/* Matches GdkPixbufDestroyNotify; frees the RGBA buffer owned by the pixbuf. */
static void
servo_gtk_web_view_free_frame_data(guchar *pixels, gpointer data)
{
    (void) data;
    g_free(pixels);
}

/*
 * Servo delivers a finished frame as a tightly-packed RGBA8 buffer that is only
 * valid for the duration of the callback, so we copy it into a GdkPixbuf (which
 * stores RGBA natively) and request a redraw. Runs on the main thread, inside
 * servo_webview_spin() from the tick callback.
 */
static void
servo_gtk_web_view_on_frame_ready(const guint8 *rgba,
                                  guint32       width,
                                  guint32       height,
                                  gpointer      user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    gsize    size = (gsize) width * height * 4;
    guint8  *copy = g_memdup2(rgba, size);
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(
        copy,
        GDK_COLORSPACE_RGB,
        TRUE,                 /* has_alpha */
        8,                    /* bits_per_sample */
        (int) width,
        (int) height,
        (int) (width * 4),    /* rowstride */
        servo_gtk_web_view_free_frame_data,
        NULL
    );

    g_clear_object(&self->frame);
    self->frame = pixbuf;

    gtk_widget_queue_draw(GTK_WIDGET(self));
}

/*
 * Servo asked the embedder to change the pointer cursor. GTK4 resolves a named
 * cursor for the widget directly, with no GdkWindow round-trip.
 */
static void
servo_gtk_web_view_on_cursor_changed(const char *name, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), name);
}

/*
 * Servo navigated to a new URL (link, redirect, history traversal or an
 * embedder-issued load). Keep the "uri" property in sync and emit the
 * "uri-changed" signal so observers can react.
 */
static void
servo_gtk_web_view_on_url_changed(const char *url, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    if (g_strcmp0(self->uri, url) == 0) {
        return;
    }

    g_free(self->uri);
    self->uri = g_strdup(url);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_URI]);
    g_signal_emit(self, signals[URI_CHANGED], 0, self->uri);
}

/*
 * Servo reported a new page title (NULL when the page has none). Cache it for
 * the "title" property and notify.
 */
static void
servo_gtk_web_view_on_title_changed(const char *title, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    if (g_strcmp0(self->priv->title, title) == 0) {
        return;
    }

    g_free(self->priv->title);
    self->priv->title = g_strdup(title);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TITLE]);
}

/*
 * A load started, had its <head> parsed, or completed. Translate Servo's status
 * to a #ServoGtkLoadEvent, keep "is-loading" in sync and emit ::load-changed.
 */
static void
servo_gtk_web_view_on_load_status_changed(guint32 status, gpointer user_data)
{
    ServoGtkWebView  *self = SERVO_GTK_WEB_VIEW(user_data);
    ServoGtkLoadEvent load_event;
    gboolean          is_loading;

    switch (status) {
    case SERVO_LOAD_STATUS_STARTED:
        load_event = SERVO_GTK_LOAD_STARTED;
        is_loading = TRUE;
        break;
    case SERVO_LOAD_STATUS_HEAD_PARSED:
        load_event = SERVO_GTK_LOAD_COMMITTED;
        is_loading = TRUE;
        break;
    case SERVO_LOAD_STATUS_COMPLETE:
        load_event = SERVO_GTK_LOAD_FINISHED;
        is_loading = FALSE;
        break;
    default:
        /* An unknown status from a newer library: ignore rather than guess. */
        return;
    }

    if (self->priv->is_loading != is_loading) {
        self->priv->is_loading = is_loading;
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IS_LOADING]);
    }

    g_signal_emit(self, signals[LOAD_CHANGED], 0, load_event);
}

/*
 * The session history changed (navigation or traversal). Refresh the
 * "can-go-back" / "can-go-forward" properties.
 */
static void
servo_gtk_web_view_on_history_changed(bool     can_go_back,
                                      bool     can_go_forward,
                                      gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    if (self->priv->can_go_back != (gboolean) can_go_back) {
        self->priv->can_go_back = can_go_back;
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_CAN_GO_BACK]);
    }

    if (self->priv->can_go_forward != (gboolean) can_go_forward) {
        self->priv->can_go_forward = can_go_forward;
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_CAN_GO_FORWARD]);
    }
}

/* ------------------------------------------------------------------ *
 * Built-in script dialog
 *
 * Servo blocks the page's script until a dialog is answered, so the window
 * below must answer exactly once no matter how it goes away: the buttons
 * answer, and ::destroy answers with "cancelled" for a window closed through
 * the window manager or torn down with the widget.
 * ------------------------------------------------------------------ */

typedef struct {
    ServoGtkWebView *web_view;    /* holds a reference */
    GtkWidget       *entry;       /* unowned; NULL unless this is a prompt */
    guint64          request_id;
    gboolean         responded;
} ScriptDialogClosure;

static void
script_dialog_closure_free(gpointer data)
{
    ScriptDialogClosure *closure = data;

    g_object_unref(closure->web_view);
    g_free(closure);
}

/* Answer the dialog this window is showing, at most once. */
static void
script_dialog_respond(GtkWidget *window, gboolean accepted)
{
    ScriptDialogClosure *closure =
        g_object_get_data(G_OBJECT(window), "servo-gtk-script-dialog");
    const gchar *text = NULL;

    if (closure == NULL || closure->responded) {
        return;
    }
    closure->responded = TRUE;

    if (closure->entry != NULL) {
        text = gtk_editable_get_text(GTK_EDITABLE(closure->entry));
    }

    servo_gtk_web_view_respond_to_dialog(
        closure->web_view, closure->request_id, accepted, text);
}

/* Take a dialog window down; ::destroy cancels it if it is still unanswered. */
static void
servo_gtk_web_view_destroy_dialog(GtkWidget *window)
{
    gtk_window_destroy(GTK_WINDOW(window));
}

static void
on_script_dialog_destroy(GtkWidget *window, gpointer user_data)
{
    (void) user_data;

    script_dialog_respond(window, FALSE);
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
static gboolean
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
 * Web content opened alert()/confirm()/prompt(). Servo blocks the page's script
 * until the dialog is answered, so this only reports it; the answer travels
 * back later through servo_gtk_web_view_respond_to_dialog(). Emitting the
 * signal runs the built-in dialog as the class closure unless a handler
 * returns TRUE to present its own.
 */
static void
servo_gtk_web_view_on_dialog(guint64     request_id,
                             guint32     dialog_type,
                             const char *message,
                             const char *default_value,
                             gpointer    user_data)
{
    ServoGtkWebView         *self = SERVO_GTK_WEB_VIEW(user_data);
    ServoGtkScriptDialogType type;
    gboolean                 handled = FALSE;

    switch (dialog_type) {
    case SERVO_DIALOG_ALERT:   type = SERVO_GTK_SCRIPT_DIALOG_ALERT; break;
    case SERVO_DIALOG_CONFIRM: type = SERVO_GTK_SCRIPT_DIALOG_CONFIRM; break;
    case SERVO_DIALOG_PROMPT:  type = SERVO_GTK_SCRIPT_DIALOG_PROMPT; break;
    default:
        /* An unknown dialog kind from a newer library: cancel rather than guess. */
        servo_gtk_web_view_respond_to_dialog(self, request_id, FALSE, NULL);
        return;
    }

    g_signal_emit(self, signals[SCRIPT_DIALOG], 0,
                  type, message, default_value, request_id, &handled);
}

/*
 * Servo withdrew a dialog before it was answered (the page navigated away).
 * Take down the built-in window if it is the one showing, and tell handlers
 * that took the dialog over.
 */
static void
servo_gtk_web_view_on_request_cancelled(guint64 request_id, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    GtkWidget       *window =
        g_hash_table_lookup(self->priv->dialogs, &request_id);

    if (window != NULL) {
        servo_gtk_web_view_destroy_dialog(window);
    }

    g_signal_emit(self, signals[SCRIPT_DIALOG_CANCELLED], 0, request_id);
}

/* ------------------------------------------------------------------ *
 * Built-in file chooser
 * ------------------------------------------------------------------ */

typedef struct {
    ServoGtkWebView *web_view;    /* holds a reference */
    guint64          request_id;
} FileChooserClosure;

/* Build a GtkFileFilter from Servo's bare extensions, or NULL for "any file". */
static GtkFileFilter *
servo_gtk_web_view_build_file_filter(const gchar *const *filter_patterns)
{
    GtkFileFilter *filter;

    if (filter_patterns == NULL || filter_patterns[0] == NULL) {
        return NULL;
    }

    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Accepted files");

    for (gsize i = 0; filter_patterns[i] != NULL; i++) {
        /* Servo reports extensions without the dot; GTK wants a glob. */
        gchar *glob = g_strconcat("*.", filter_patterns[i], NULL);
        gtk_file_filter_add_pattern(filter, glob);
        g_free(glob);
    }

    return filter;
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
static gboolean
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

/*
 * Web content activated an <input type=file>. As with dialogs this only reports
 * the request; the chosen paths travel back later through
 * servo_gtk_web_view_respond_to_file_chooser().
 */
static void
servo_gtk_web_view_on_file_picker(guint64            request_id,
                                  const char *const *filter_patterns,
                                  gsize              filter_pattern_count,
                                  bool               allow_multiple,
                                  gpointer           user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    gboolean         handled = FALSE;
    GPtrArray       *patterns = g_ptr_array_new();

    for (gsize i = 0; i < filter_pattern_count; i++) {
        g_ptr_array_add(patterns, (gpointer) filter_patterns[i]);
    }
    g_ptr_array_add(patterns, NULL);

    g_signal_emit(self, signals[RUN_FILE_CHOOSER], 0,
                  patterns->pdata, (gboolean) allow_multiple, request_id, &handled);

    g_ptr_array_free(patterns, TRUE);
}

/* ------------------------------------------------------------------ *
 * Built-in credentials prompt
 * ------------------------------------------------------------------ */

typedef struct {
    ServoGtkWebView *web_view;    /* holds a reference */
    GtkWidget       *username;    /* unowned */
    GtkWidget       *password;    /* unowned */
    guint64          request_id;
    gboolean         responded;
} AuthClosure;

static void
auth_closure_free(gpointer data)
{
    AuthClosure *closure = data;

    g_object_unref(closure->web_view);
    g_free(closure);
}

/*
 * Answer the challenge this window is showing, at most once. Cancelling sends
 * NULL credentials, which fails the load as unauthenticated rather than
 * retrying it unauthenticated behind the user's back.
 */
static void
auth_respond(GtkWidget *window, gboolean accepted)
{
    AuthClosure *closure = g_object_get_data(G_OBJECT(window), "servo-gtk-auth");

    if (closure == NULL || closure->responded) {
        return;
    }
    closure->responded = TRUE;

    if (accepted) {
        servo_gtk_web_view_respond_to_authentication(
            closure->web_view, closure->request_id,
            gtk_editable_get_text(GTK_EDITABLE(closure->username)),
            gtk_editable_get_text(GTK_EDITABLE(closure->password)));
    } else {
        servo_gtk_web_view_respond_to_authentication(
            closure->web_view, closure->request_id, NULL, NULL);
    }
}

static void
on_auth_destroy(GtkWidget *window, gpointer user_data)
{
    (void) user_data;

    auth_respond(window, FALSE);
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
static gboolean
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

/*
 * A server or proxy asked for credentials. Only reports the challenge; the
 * credentials travel back later through
 * servo_gtk_web_view_respond_to_authentication().
 */
static void
servo_gtk_web_view_on_authentication(guint64     request_id,
                                     const char *url,
                                     bool        for_proxy,
                                     gpointer    user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    gboolean         handled = FALSE;

    g_signal_emit(self, signals[AUTHENTICATE], 0,
                  url, (gboolean) for_proxy, request_id, &handled);
}

/*
 * A page asked for a permission-gated capability. Unanswered requests are
 * denied, so an unknown feature is refused rather than guessed at.
 */
static void
servo_gtk_web_view_on_permission(guint64  request_id,
                                 guint32  feature,
                                 gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    gboolean         handled = FALSE;

    if (feature > SERVO_GTK_PERMISSION_SCREEN_WAKE_LOCK) {
        servo_gtk_web_view_respond_to_permission_request(self, request_id, FALSE);
        return;
    }

    g_signal_emit(self, signals[PERMISSION_REQUEST], 0,
                  (ServoGtkPermissionFeature) feature, request_id, &handled);
}

/*
 * Class closure for ::permission-request. Nothing here can know whether the
 * user wants to grant a capability, so it refuses: an application that wants to
 * prompt connects to the signal and answers there.
 */
static gboolean
servo_gtk_web_view_default_permission_request(ServoGtkWebView           *self,
                                              ServoGtkPermissionFeature  feature,
                                              guint64                    request_id)
{
    (void) feature;

    servo_gtk_web_view_respond_to_permission_request(self, request_id, FALSE);

    return TRUE;
}

/* Pump Servo's event loop once per frame clock tick. */
static gboolean
servo_gtk_web_view_tick(GtkWidget     *widget,
                        GdkFrameClock *frame_clock,
                        gpointer       user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    (void) frame_clock;
    (void) user_data;

    if (self->servo != NULL) {
        servo_webview_spin(self->servo);
    }

    return G_SOURCE_CONTINUE;
}

static void
servo_gtk_web_view_set_property(GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(object);

    switch (property_id) {
    case PROP_URI:
        servo_gtk_web_view_load_uri(self, g_value_get_string(value));
        break;

    case PROP_ZOOM_LEVEL:
        servo_gtk_web_view_set_zoom_level(self, g_value_get_double(value));
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
servo_gtk_web_view_get_property(GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(object);

    switch (property_id) {
    case PROP_URI:
        g_value_set_string(value, self->uri);
        break;

    case PROP_TITLE:
        g_value_set_string(value, self->priv->title);
        break;

    case PROP_IS_LOADING:
        g_value_set_boolean(value, self->priv->is_loading);
        break;

    case PROP_CAN_GO_BACK:
        g_value_set_boolean(value, self->priv->can_go_back);
        break;

    case PROP_CAN_GO_FORWARD:
        g_value_set_boolean(value, self->priv->can_go_forward);
        break;

    case PROP_ZOOM_LEVEL:
        g_value_set_double(value, self->priv->zoom_level);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
servo_gtk_web_view_dispose(GObject *object)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(object);

    if (self->tick_id != 0) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }

    g_clear_object(&self->frame);

    /*
     * Take down any dialog still on screen before Servo goes away. Each window
     * cancels its request as it is destroyed, which removes it from the table,
     * so iterate over a snapshot of the values rather than the live table.
     */
    if (self->priv != NULL && self->priv->dialogs != NULL) {
        GList *windows = g_hash_table_get_values(self->priv->dialogs);

        for (GList *item = windows; item != NULL; item = item->next) {
            servo_gtk_web_view_destroy_dialog(GTK_WIDGET(item->data));
        }
        g_list_free(windows);
    }

    if (self->servo != NULL) {
        servo_webview_free(self->servo);
        self->servo = NULL;
    }

    G_OBJECT_CLASS(servo_gtk_web_view_parent_class)->dispose(object);
}

static void
servo_gtk_web_view_finalize(GObject *object)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(object);

    g_clear_pointer(&self->uri, g_free);
    g_clear_pointer(&self->priv->title, g_free);
    g_clear_pointer(&self->priv->dialogs, g_hash_table_unref);
    g_clear_pointer(&self->priv, g_free);

    G_OBJECT_CLASS(servo_gtk_web_view_parent_class)->finalize(object);
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
 * Create the Servo instance on first use, or resize it, for a widget whose
 * logical size is width x height. Servo's surface is sized in device pixels, so
 * the logical size is multiplied by the widget's scale factor and that same
 * factor is handed to Servo as the HiDPI scale — otherwise the page would be
 * laid out at logical size and merely upscaled, and window.devicePixelRatio
 * would be wrong.
 */
static void
servo_gtk_web_view_sync_surface(ServoGtkWebView *self, int width, int height)
{
    int   scale = MAX(1, gtk_widget_get_scale_factor(GTK_WIDGET(self)));
    guint w = (guint) MAX(1, width) * (guint) scale;
    guint h = (guint) MAX(1, height) * (guint) scale;

    if (self->servo == NULL) {
        /*
         * Pass any URI requested before allocation as the initial URL: Servo
         * creates the browsing context together with it. Issuing a separate
         * load here instead would race the context's creation and be dropped.
         */
        self->servo = servo_webview_new(w, h, self->uri);
        if (self->servo != NULL) {
            servo_webview_set_hidpi_scale_factor(self->servo, (float) scale);
            /* Any zoom set before allocation was only cached; apply it now. */
            if (self->priv->zoom_level != 1.0) {
                servo_webview_set_zoom_level(self->servo, (float) self->priv->zoom_level);
            }
            servo_webview_set_frame_ready_callback(
                self->servo, servo_gtk_web_view_on_frame_ready, self);
            servo_webview_set_cursor_changed_callback(
                self->servo, servo_gtk_web_view_on_cursor_changed, self);
            servo_webview_set_url_changed_callback(
                self->servo, servo_gtk_web_view_on_url_changed, self);
            servo_webview_set_title_changed_callback(
                self->servo, servo_gtk_web_view_on_title_changed, self);
            servo_webview_set_load_status_changed_callback(
                self->servo, servo_gtk_web_view_on_load_status_changed, self);
            servo_webview_set_history_changed_callback(
                self->servo, servo_gtk_web_view_on_history_changed, self);
            servo_webview_set_dialog_callback(
                self->servo, servo_gtk_web_view_on_dialog, self);
            servo_webview_set_file_picker_callback(
                self->servo, servo_gtk_web_view_on_file_picker, self);
            servo_webview_set_authentication_callback(
                self->servo, servo_gtk_web_view_on_authentication, self);
            servo_webview_set_permission_callback(
                self->servo, servo_gtk_web_view_on_permission, self);
            servo_webview_set_request_cancelled_callback(
                self->servo, servo_gtk_web_view_on_request_cancelled, self);
        }
    } else {
        servo_webview_set_hidpi_scale_factor(self->servo, (float) scale);
        servo_webview_resize(self->servo, w, h);
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

/*
 * The widget moved to a display with a different scale factor. The logical size
 * is unchanged, so ::resize does not fire; re-sync the surface so Servo renders
 * at the new device resolution.
 */
static void
servo_gtk_web_view_on_scale_factor_changed(GObject    *object,
                                           GParamSpec *pspec,
                                           gpointer    user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(object);
    GtkWidget       *widget = GTK_WIDGET(self);

    (void) pspec;
    (void) user_data;

    /* Not allocated yet: the first ::resize will pick the scale factor up. */
    if (self->servo == NULL) {
        return;
    }

    servo_gtk_web_view_sync_surface(
        self, gtk_widget_get_width(widget), gtk_widget_get_height(widget));
}

/* Convert a logical widget coordinate or delta to the device pixels Servo uses. */
static double
servo_gtk_web_view_to_device(ServoGtkWebView *self, double value)
{
    return value * MAX(1, gtk_widget_get_scale_factor(GTK_WIDGET(self)));
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

/* Translate a GDK modifier state mask to the Servo modifier bitmask. */
static uint32_t
servo_gtk_web_view_modifiers(GdkModifierType state)
{
    uint32_t mods = SERVO_MODIFIER_NONE;

    if (state & GDK_SHIFT_MASK) {
        mods |= SERVO_MODIFIER_SHIFT;
    }
    if (state & GDK_CONTROL_MASK) {
        mods |= SERVO_MODIFIER_CONTROL;
    }
    if (state & GDK_ALT_MASK) {
        mods |= SERVO_MODIFIER_ALT;
    }
    if (state & (GDK_META_MASK | GDK_SUPER_MASK)) {
        mods |= SERVO_MODIFIER_META;
    }

    return mods;
}

/*
 * Map a GDK keyval to a named ServoKey. Printable keys return
 * SERVO_KEY_CHARACTER; the caller then resolves the actual character via
 * gdk_keyval_to_unicode(). The non-printable keys handled here are intercepted
 * before that step so their control-character Unicode values never leak through.
 */
static ServoKey
servo_gtk_web_view_map_keyval(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:      return SERVO_KEY_ENTER;
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
    case GDK_KEY_ISO_Left_Tab:   return SERVO_KEY_TAB;
    case GDK_KEY_BackSpace:      return SERVO_KEY_BACKSPACE;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:      return SERVO_KEY_DELETE;
    case GDK_KEY_Escape:         return SERVO_KEY_ESCAPE;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:        return SERVO_KEY_ARROW_LEFT;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:       return SERVO_KEY_ARROW_RIGHT;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:          return SERVO_KEY_ARROW_UP;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:        return SERVO_KEY_ARROW_DOWN;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:        return SERVO_KEY_HOME;
    case GDK_KEY_End:
    case GDK_KEY_KP_End:         return SERVO_KEY_END;
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:     return SERVO_KEY_PAGE_UP;
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:   return SERVO_KEY_PAGE_DOWN;
    case GDK_KEY_Insert:
    case GDK_KEY_KP_Insert:      return SERVO_KEY_INSERT;
    case GDK_KEY_F1:             return SERVO_KEY_F1;
    case GDK_KEY_F2:             return SERVO_KEY_F2;
    case GDK_KEY_F3:             return SERVO_KEY_F3;
    case GDK_KEY_F4:             return SERVO_KEY_F4;
    case GDK_KEY_F5:             return SERVO_KEY_F5;
    case GDK_KEY_F6:             return SERVO_KEY_F6;
    case GDK_KEY_F7:             return SERVO_KEY_F7;
    case GDK_KEY_F8:             return SERVO_KEY_F8;
    case GDK_KEY_F9:             return SERVO_KEY_F9;
    case GDK_KEY_F10:            return SERVO_KEY_F10;
    case GDK_KEY_F11:            return SERVO_KEY_F11;
    case GDK_KEY_F12:            return SERVO_KEY_F12;
    default:                     return SERVO_KEY_CHARACTER;
    }
}

/* Shared handler for key press (pressed = TRUE) and release (FALSE). */
static gboolean
servo_gtk_web_view_key(ServoGtkWebView *self,
                       guint            keyval,
                       GdkModifierType  state,
                       gboolean         pressed)
{
    if (self->servo == NULL) {
        return FALSE;
    }

    ServoKey key = servo_gtk_web_view_map_keyval(keyval);
    guint32  unicode = 0;

    if (key == SERVO_KEY_CHARACTER) {
        unicode = gdk_keyval_to_unicode(keyval);
        /*
         * Bare modifiers (Shift, Control, ...) have no Unicode mapping; report
         * them as unidentified rather than as an empty character. Function keys
         * and Insert are already handled as named keys above.
         */
        if (unicode == 0) {
            key = SERVO_KEY_UNIDENTIFIED;
        }
    }

    servo_webview_key(self->servo,
                      (uint32_t) key,
                      unicode,
                      servo_gtk_web_view_modifiers(state),
                      pressed);

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

static void
servo_gtk_web_view_class_init(ServoGtkWebViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = servo_gtk_web_view_set_property;
    object_class->get_property = servo_gtk_web_view_get_property;
    object_class->dispose = servo_gtk_web_view_dispose;
    object_class->finalize = servo_gtk_web_view_finalize;

    klass->script_dialog = servo_gtk_web_view_default_script_dialog;
    klass->run_file_chooser = servo_gtk_web_view_default_run_file_chooser;
    klass->authenticate = servo_gtk_web_view_default_authenticate;
    klass->permission_request = servo_gtk_web_view_default_permission_request;

    properties[PROP_URI] =
        g_param_spec_string(
            "uri",
            "URI",
            "The currently loaded URI",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
        );

    properties[PROP_TITLE] =
        g_param_spec_string(
            "title",
            "Title",
            "The title of the loaded page",
            NULL,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS
        );

    properties[PROP_IS_LOADING] =
        g_param_spec_boolean(
            "is-loading",
            "Is loading",
            "Whether a load is currently in progress",
            FALSE,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS
        );

    properties[PROP_CAN_GO_BACK] =
        g_param_spec_boolean(
            "can-go-back",
            "Can go back",
            "Whether there is a previous entry in the session history",
            FALSE,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS
        );

    properties[PROP_CAN_GO_FORWARD] =
        g_param_spec_boolean(
            "can-go-forward",
            "Can go forward",
            "Whether there is a following entry in the session history",
            FALSE,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS
        );

    properties[PROP_ZOOM_LEVEL] =
        g_param_spec_double(
            "zoom-level",
            "Zoom level",
            "The page zoom level, where 1.0 is unzoomed",
            0.1, 10.0, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
        );

    g_object_class_install_properties(object_class, N_PROPERTIES, properties);

    /**
     * ServoGtkWebView::uri-changed:
     * @self: the #ServoGtkWebView
     * @uri: the new URI
     *
     * Emitted whenever the webview navigates to a new URL (link activation,
     * redirect, history traversal or an embedder-issued load).
     */
    signals[URI_CHANGED] =
        g_signal_new(
            "uri-changed",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_FIRST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, uri_changed),
            NULL, NULL, /* accumulator */
            NULL,       /* default (generic) C marshaller */
            G_TYPE_NONE,
            1,
            G_TYPE_STRING
        );

    /**
     * ServoGtkWebView::load-changed:
     * @self: the #ServoGtkWebView
     * @load_event: the stage the load has reached
     *
     * Emitted as a load progresses: once with %SERVO_GTK_LOAD_STARTED, once
     * with %SERVO_GTK_LOAD_COMMITTED when the document body becomes reachable,
     * and once with %SERVO_GTK_LOAD_FINISHED when the page and all of its
     * subresources have loaded.
     */
    signals[LOAD_CHANGED] =
        g_signal_new(
            "load-changed",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_FIRST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, load_changed),
            NULL, NULL, /* accumulator */
            NULL,       /* default (generic) C marshaller */
            G_TYPE_NONE,
            1,
            SERVO_GTK_TYPE_LOAD_EVENT
        );

    /**
     * ServoGtkWebView::script-dialog:
     * @self: the #ServoGtkWebView
     * @dialog_type: which of alert(), confirm() or prompt() the page called
     * @message: the message the page supplied
     * @default_value: (nullable): a prompt's initial text, %NULL otherwise
     * @request_id: identifies this dialog when answering it
     *
     * Emitted when web content opens a dialog. The default handler presents a
     * modal window and answers the dialog itself.
     *
     * Return %TRUE from a handler to present your own dialog instead; you must
     * then call servo_gtk_web_view_respond_to_dialog() with @request_id when
     * the user answers, since the page's script stays blocked until you do.
     * Watch #ServoGtkWebView::script-dialog-cancelled in case Servo withdraws
     * the dialog first.
     *
     * @message and @default_value are controlled by the page, so a dialog must
     * be presented in a way that cannot be mistaken for browser UI.
     *
     * Returns: %TRUE to stop the built-in dialog from being shown
     */
    signals[SCRIPT_DIALOG] =
        g_signal_new(
            "script-dialog",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_LAST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, script_dialog),
            g_signal_accumulator_true_handled, NULL,
            NULL,       /* default (generic) C marshaller */
            G_TYPE_BOOLEAN,
            4,
            SERVO_GTK_TYPE_SCRIPT_DIALOG_TYPE,
            G_TYPE_STRING,
            G_TYPE_STRING,
            G_TYPE_UINT64
        );

    /**
     * ServoGtkWebView::script-dialog-cancelled:
     * @self: the #ServoGtkWebView
     * @request_id: the dialog Servo withdrew
     *
     * Emitted when Servo withdraws a dialog that has not been answered, for
     * instance because the page navigated away. A handler that took the dialog
     * over should take its window down; answering @request_id afterwards is
     * harmless but does nothing.
     */
    signals[SCRIPT_DIALOG_CANCELLED] =
        g_signal_new(
            "script-dialog-cancelled",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_FIRST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, script_dialog_cancelled),
            NULL, NULL, /* accumulator */
            NULL,       /* default (generic) C marshaller */
            G_TYPE_NONE,
            1,
            G_TYPE_UINT64
        );

    /**
     * ServoGtkWebView::run-file-chooser:
     * @self: the #ServoGtkWebView
     * @filter_patterns: (array zero-terminated=1): bare filename extensions
     *   with no leading dot (e.g. "png"); empty if any file is acceptable
     * @allow_multiple: whether more than one file may be chosen
     * @request_id: identifies this chooser when answering it
     *
     * Emitted when web content activates an `&lt;input type=file&gt;`. The
     * default handler presents a file chooser and answers it.
     *
     * Return %TRUE from a handler to present your own chooser instead; you must
     * then call servo_gtk_web_view_respond_to_file_chooser() with @request_id,
     * since the page's script stays blocked until you do.
     *
     * Returns: %TRUE to stop the built-in chooser from being shown
     */
    signals[RUN_FILE_CHOOSER] =
        g_signal_new(
            "run-file-chooser",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_LAST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, run_file_chooser),
            g_signal_accumulator_true_handled, NULL,
            NULL,       /* default (generic) C marshaller */
            G_TYPE_BOOLEAN,
            3,
            G_TYPE_STRV,
            G_TYPE_BOOLEAN,
            G_TYPE_UINT64
        );

    /**
     * ServoGtkWebView::authenticate:
     * @self: the #ServoGtkWebView
     * @uri: the URI that triggered the challenge
     * @for_proxy: %TRUE for a proxy challenge, %FALSE for an origin one
     * @request_id: identifies this challenge when answering it
     *
     * Emitted when a server or proxy issues an HTTP authentication challenge.
     * The default handler prompts for a username and password and answers.
     *
     * Return %TRUE from a handler to prompt yourself; you must then call
     * servo_gtk_web_view_respond_to_authentication() with @request_id. Do not
     * offer credentials stored for an origin in answer to a proxy challenge, or
     * the other way round: @for_proxy distinguishes the two.
     *
     * Returns: %TRUE to stop the built-in prompt from being shown
     */
    signals[AUTHENTICATE] =
        g_signal_new(
            "authenticate",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_LAST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, authenticate),
            g_signal_accumulator_true_handled, NULL,
            NULL,       /* default (generic) C marshaller */
            G_TYPE_BOOLEAN,
            3,
            G_TYPE_STRING,
            G_TYPE_BOOLEAN,
            G_TYPE_UINT64
        );

    /**
     * ServoGtkWebView::permission-request:
     * @self: the #ServoGtkWebView
     * @feature: the capability the page asked for
     * @request_id: identifies this request when answering it
     *
     * Emitted when a page asks for a permission-gated capability such as
     * geolocation or the camera.
     *
     * The default handler refuses, since nothing at this level can know what
     * the user wants. Connect to the signal, return %TRUE, and answer with
     * servo_gtk_web_view_respond_to_permission_request() to prompt instead. A
     * request that is never answered is denied.
     *
     * Returns: %TRUE to stop the default refusal
     */
    signals[PERMISSION_REQUEST] =
        g_signal_new(
            "permission-request",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_LAST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, permission_request),
            g_signal_accumulator_true_handled, NULL,
            NULL,       /* default (generic) C marshaller */
            G_TYPE_BOOLEAN,
            2,
            SERVO_GTK_TYPE_PERMISSION_FEATURE,
            G_TYPE_UINT64
        );
}

static void
servo_gtk_web_view_init(ServoGtkWebView *self)
{
    GtkWidget *widget = GTK_WIDGET(self);

    self->priv = g_new0(ServoGtkWebViewPrivate, 1);
    self->priv->zoom_level = 1.0;
    self->priv->dialogs =
        g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

    gtk_widget_set_focusable(widget, TRUE);

    /* GTK4: the drawing area renders through a draw func rather than a vfunc. */
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(self), servo_gtk_web_view_draw, NULL, NULL);

    /* Lazily create/resize the Servo instance as the widget is allocated. */
    g_signal_connect(self, "resize", G_CALLBACK(servo_gtk_web_view_on_resize), NULL);
    g_signal_connect(self,
                     "notify::scale-factor",
                     G_CALLBACK(servo_gtk_web_view_on_scale_factor_changed),
                     NULL);

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

    self->tick_id = gtk_widget_add_tick_callback(widget, servo_gtk_web_view_tick, NULL, NULL);
}

ServoGtkWebView *
servo_gtk_web_view_new(void)
{
    return g_object_new(SERVO_GTK_TYPE_WEB_VIEW, NULL);
}

void
servo_gtk_web_view_load_uri(ServoGtkWebView *self, const gchar *uri)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    if (g_strcmp0(self->uri, uri) == 0) {
        return;
    }

    g_free(self->uri);
    self->uri = g_strdup(uri);

    /* If Servo is already up, load now; otherwise the resize handler picks it up. */
    if (self->servo != NULL && self->uri != NULL) {
        servo_webview_load_uri(self->servo, self->uri);
    }

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_URI]);
}

const gchar *
servo_gtk_web_view_get_uri(ServoGtkWebView *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_WEB_VIEW(self), NULL);

    return self->uri;
}

const gchar *
servo_gtk_web_view_get_title(ServoGtkWebView *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_WEB_VIEW(self), NULL);

    return self->priv->title;
}

gboolean
servo_gtk_web_view_is_loading(ServoGtkWebView *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_WEB_VIEW(self), FALSE);

    return self->priv->is_loading;
}

gboolean
servo_gtk_web_view_can_go_back(ServoGtkWebView *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_WEB_VIEW(self), FALSE);

    return self->priv->can_go_back;
}

gboolean
servo_gtk_web_view_can_go_forward(ServoGtkWebView *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_WEB_VIEW(self), FALSE);

    return self->priv->can_go_forward;
}

void
servo_gtk_web_view_set_zoom_level(ServoGtkWebView *self, gdouble zoom_level)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    zoom_level = CLAMP(zoom_level, 0.1, 10.0);

    if (self->priv->zoom_level == zoom_level) {
        return;
    }

    self->priv->zoom_level = zoom_level;

    /* Before allocation there is no Servo yet; the value is applied on create. */
    if (self->servo != NULL) {
        servo_webview_set_zoom_level(self->servo, (float) zoom_level);
    }

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ZOOM_LEVEL]);
}

gdouble
servo_gtk_web_view_get_zoom_level(ServoGtkWebView *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_WEB_VIEW(self), 1.0);

    return self->priv->zoom_level;
}

void
servo_gtk_web_view_respond_to_dialog(ServoGtkWebView *self,
                                     guint64          request_id,
                                     gboolean         accepted,
                                     const gchar     *text)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    g_hash_table_remove(self->priv->dialogs, &request_id);

    if (self->servo != NULL) {
        servo_webview_dialog_respond(self->servo, request_id, accepted, text);
    }
}

void
servo_gtk_web_view_respond_to_file_chooser(ServoGtkWebView    *self,
                                           guint64             request_id,
                                           const gchar *const *paths)
{
    gsize count = 0;

    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    if (self->servo == NULL) {
        return;
    }

    while (paths != NULL && paths[count] != NULL) {
        count++;
    }

    servo_webview_file_picker_respond(self->servo, request_id, paths, count);
}

void
servo_gtk_web_view_respond_to_authentication(ServoGtkWebView *self,
                                             guint64          request_id,
                                             const gchar     *username,
                                             const gchar     *password)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    g_hash_table_remove(self->priv->dialogs, &request_id);

    if (self->servo != NULL) {
        servo_webview_authentication_respond(self->servo, request_id, username, password);
    }
}

void
servo_gtk_web_view_respond_to_permission_request(ServoGtkWebView *self,
                                                 guint64          request_id,
                                                 gboolean         allowed)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    if (self->servo != NULL) {
        servo_webview_permission_respond(self->servo, request_id, allowed);
    }
}

/*
 * Session-history and reload entry points. Each is a thin forward to the
 * matching FFI call; Servo is created lazily on the first allocation, so a
 * call made before then has nothing to act on and is silently ignored.
 */
void
servo_gtk_web_view_reload(ServoGtkWebView *self)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    if (self->servo != NULL) {
        servo_webview_reload(self->servo);
    }
}

void
servo_gtk_web_view_go_back(ServoGtkWebView *self)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    if (self->servo != NULL) {
        servo_webview_go_back(self->servo);
    }
}

void
servo_gtk_web_view_go_forward(ServoGtkWebView *self)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    if (self->servo != NULL) {
        servo_webview_go_forward(self->servo);
    }
}

/*
 * One-shot context bridging the Servo FFI callback (which only knows a
 * user_data pointer) back to the public GTK callback (which also receives the
 * originating web view). Heap-allocated because Servo delivers the result
 * asynchronously, from a later servo_webview_spin().
 */
typedef struct {
    ServoGtkWebView              *web_view;
    ServoGtkScriptResultCallback  callback;
    gpointer                      user_data;
} ScriptResultClosure;

/*
 * Trampoline matching ServoScriptResultCallback. Forwards the result (or
 * error) to the user callback, then releases the closure and the reference it
 * held on the web view. Invoked exactly once per evaluate call.
 */
static void
servo_gtk_web_view_on_script_result(const char *result_json,
                                    const char *error,
                                    void       *user_data)
{
    ScriptResultClosure *closure = user_data;

    closure->callback(closure->web_view, result_json, error, closure->user_data);

    g_object_unref(closure->web_view);
    g_free(closure);
}

void
servo_gtk_web_view_evaluate_script(ServoGtkWebView              *self,
                                   const gchar                  *script,
                                   ServoGtkScriptResultCallback  callback,
                                   gpointer                      user_data)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    if (callback == NULL) {
        return;
    }

    /* Servo is created lazily on the first size allocation; without it there is
     * no browsing context to run the script in. */
    if (self->servo == NULL) {
        callback(self, NULL, "web view is not realized yet", user_data);
        return;
    }

    /* Keep the web view alive until the (asynchronous) result arrives. */
    ScriptResultClosure *closure = g_new0(ScriptResultClosure, 1);
    closure->web_view  = g_object_ref(self);
    closure->callback  = callback;
    closure->user_data = user_data;

    servo_webview_evaluate_script(self->servo,
                                  script,
                                  servo_gtk_web_view_on_script_result,
                                  closure);
}

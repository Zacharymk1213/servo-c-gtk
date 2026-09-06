/*
 * ServoGtkWebView: everything that does not depend on the GTK major version.
 *
 * Compiled once per GTK major with SERVO_GTK_MAJOR set, alongside
 * servo-gtk<N>-view.c which supplies the parts that do — cursors, drawing,
 * input plumbing and the built-in dialog, file chooser and context menu
 * windows. See servo-gtk-view-private.h for the split.
 */
#include "servo-gtk-view-private.h"

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
    CONTEXT_MENU,
    CREATE_WEB_VIEW,
    CLOSE,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

G_DEFINE_TYPE(ServoGtkWebView, servo_gtk_web_view, GTK_TYPE_DRAWING_AREA)

/*
 * Adopt an already-created Servo webview: store it, push the widget's current
 * state onto it and register every callback. Used both when the widget creates
 * its own webview on first allocation and when it adopts one built for a popup.
 */
static void servo_gtk_web_view_attach_servo(ServoGtkWebView    *self,
                                            ServoWebViewHandle *handle,
                                            gint                scale);

/*
 * The parent class, so the GTK3 source can chain up from size_allocate:
 * G_DEFINE_TYPE puts servo_gtk_web_view_parent_class in this translation unit.
 */
GtkWidgetClass *
servo_gtk_web_view_parent_widget_class(void)
{
    return GTK_WIDGET_CLASS(servo_gtk_web_view_parent_class);
}

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

void
script_dialog_closure_free(gpointer data)
{
    ScriptDialogClosure *closure = data;

    g_object_unref(closure->web_view);
    g_free(closure);
}

/* Answer the dialog this window is showing, at most once. */
void
script_dialog_respond(GtkWidget *window, gboolean accepted)
{
    ScriptDialogClosure *closure =
        g_object_get_data(G_OBJECT(window), SERVO_GTK_SCRIPT_DIALOG_DATA);
    const gchar *text = NULL;

    if (closure == NULL || closure->responded) {
        return;
    }
    closure->responded = TRUE;

    if (closure->entry != NULL) {
        text = SERVO_GTK_ENTRY_TEXT(closure->entry);
    }

    servo_gtk_web_view_respond_to_dialog(
        closure->web_view, closure->request_id, accepted, text);
}

void
on_script_dialog_destroy(GtkWidget *window, gpointer user_data)
{
    (void) user_data;

    script_dialog_respond(window, FALSE);
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

/* Build a GtkFileFilter from Servo's bare extensions, or NULL for "any file". */
GtkFileFilter *
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

void
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
void
auth_respond(GtkWidget *window, gboolean accepted)
{
    AuthClosure *closure = g_object_get_data(G_OBJECT(window), SERVO_GTK_AUTH_DATA);

    if (closure == NULL || closure->responded) {
        return;
    }
    closure->responded = TRUE;

    if (accepted) {
        servo_gtk_web_view_respond_to_authentication(
            closure->web_view, closure->request_id,
            SERVO_GTK_ENTRY_TEXT(closure->username),
            SERVO_GTK_ENTRY_TEXT(closure->password));
    } else {
        servo_gtk_web_view_respond_to_authentication(
            closure->web_view, closure->request_id, NULL, NULL);
    }
}

void
on_auth_destroy(GtkWidget *window, gpointer user_data)
{
    (void) user_data;

    auth_respond(window, FALSE);
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

void
context_menu_closure_free(gpointer data)
{
    ContextMenuClosure *closure = data;

    g_object_unref(closure->web_view);
    g_free(closure);
}

/* Answer the menu this window is showing, at most once. */
void
context_menu_respond(GtkWidget *menu, gsize item_index)
{
    ContextMenuClosure *closure =
        g_object_get_data(G_OBJECT(menu), SERVO_GTK_CONTEXT_MENU_DATA);

    if (closure == NULL || closure->responded) {
        return;
    }
    closure->responded = TRUE;

    servo_gtk_web_view_respond_to_context_menu(
        closure->web_view, closure->request_id, item_index);
}

/*
 * The user asked for a context menu. Only reports it; the chosen item travels
 * back later through servo_gtk_web_view_respond_to_context_menu().
 *
 * The signal carries the labels so a handler can present its own menu, while
 * the built-in one needs the enabled flags and separators too — those are
 * stashed for the duration of this synchronous emission rather than being
 * flattened into the signal's arguments.
 */
static void
servo_gtk_web_view_on_context_menu(guint64                     request_id,
                                   const ServoContextMenuItem *items,
                                   gsize                       item_count,
                                   gint32                      x,
                                   gint32                      y,
                                   gpointer                    user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    gboolean         handled = FALSE;
    GPtrArray       *labels = g_ptr_array_new();
    gint             scale = MAX(1, gtk_widget_get_scale_factor(GTK_WIDGET(self)));

    for (gsize i = 0; i < item_count; i++) {
        /* A separator has no label; report it as empty so indices still line up. */
        g_ptr_array_add(labels,
                        (gpointer) (items[i].label != NULL ? items[i].label : ""));
    }
    g_ptr_array_add(labels, NULL);

    self->priv->menu_items = items;
    self->priv->menu_item_count = item_count;

    /* Servo positions in device pixels; GTK widget coordinates are logical. */
    g_signal_emit(self, signals[CONTEXT_MENU], 0,
                  x / scale, y / scale, labels->pdata, request_id, &handled);

    self->priv->menu_items = NULL;
    self->priv->menu_item_count = 0;

    g_ptr_array_free(labels, TRUE);
}

/*
 * Web content asked to open a new webview. Ask the application for one through
 * ::create-web-view; the handler is expected to accept from inside the
 * emission, and anything unaccepted when it returns is refused.
 */
static void
servo_gtk_web_view_on_create_webview(guint64 request_id, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    gboolean         handled = FALSE;

    g_signal_emit(self, signals[CREATE_WEB_VIEW], 0, request_id, &handled);
}

/* The page closed its own webview. */
static void
servo_gtk_web_view_on_closed(gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    g_signal_emit(self, signals[CLOSE], 0);
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

static void
servo_gtk_web_view_attach_servo(ServoGtkWebView    *self,
                                ServoWebViewHandle *handle,
                                gint                scale)
{
    self->servo = handle;

    servo_webview_set_hidpi_scale_factor(handle, (float) scale);
    /* Any zoom set before the webview existed was only cached; apply it now. */
    if (self->priv->zoom_level != 1.0) {
        servo_webview_set_zoom_level(handle, (float) self->priv->zoom_level);
    }

    servo_webview_set_frame_ready_callback(
        handle, servo_gtk_web_view_on_frame_ready, self);
    servo_webview_set_cursor_changed_callback(
        handle, servo_gtk_web_view_on_cursor_changed, self);
    servo_webview_set_url_changed_callback(
        handle, servo_gtk_web_view_on_url_changed, self);
    servo_webview_set_title_changed_callback(
        handle, servo_gtk_web_view_on_title_changed, self);
    servo_webview_set_load_status_changed_callback(
        handle, servo_gtk_web_view_on_load_status_changed, self);
    servo_webview_set_history_changed_callback(
        handle, servo_gtk_web_view_on_history_changed, self);
    servo_webview_set_dialog_callback(
        handle, servo_gtk_web_view_on_dialog, self);
    servo_webview_set_file_picker_callback(
        handle, servo_gtk_web_view_on_file_picker, self);
    servo_webview_set_authentication_callback(
        handle, servo_gtk_web_view_on_authentication, self);
    servo_webview_set_permission_callback(
        handle, servo_gtk_web_view_on_permission, self);
    servo_webview_set_context_menu_callback(
        handle, servo_gtk_web_view_on_context_menu, self);
    servo_webview_set_create_webview_callback(
        handle, servo_gtk_web_view_on_create_webview, self);
    servo_webview_set_closed_callback(
        handle, servo_gtk_web_view_on_closed, self);
    servo_webview_set_request_cancelled_callback(
        handle, servo_gtk_web_view_on_request_cancelled, self);
}

/*
 * Create the Servo instance on first use, or resize it, for a widget whose
 * logical size is width x height. Servo's surface is sized in device pixels, so
 * the logical size is multiplied by the widget's scale factor and that same
 * factor is handed to Servo as the HiDPI scale — otherwise the page would be
 * laid out at logical size and merely upscaled, and window.devicePixelRatio
 * would be wrong.
 */
void
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
        ServoWebViewHandle *handle = servo_webview_new(w, h, self->uri);

        if (handle != NULL) {
            servo_gtk_web_view_attach_servo(self, handle, scale);
        }
    } else {
        servo_webview_set_hidpi_scale_factor(self->servo, (float) scale);
        servo_webview_resize(self->servo, w, h);
    }
}

/*
 * The widget moved to a display with a different scale factor. The logical size
 * is unchanged, so no allocation happens; re-sync the surface so Servo renders
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

    /* Not allocated yet: the first allocation will pick the scale factor up. */
    if (self->servo == NULL) {
        return;
    }

    servo_gtk_web_view_sync_surface(
        self, SERVO_GTK_WIDGET_WIDTH(widget), SERVO_GTK_WIDGET_HEIGHT(widget));
}

/* Convert a logical widget coordinate or delta to the device pixels Servo uses. */
double
servo_gtk_web_view_to_device(ServoGtkWebView *self, double value)
{
    return value * MAX(1, gtk_widget_get_scale_factor(GTK_WIDGET(self)));
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
    if (state & SERVO_GTK_ALT_MASK) {
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
gboolean
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

    /* If Servo is already up, load now; otherwise allocation picks it up. */
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

void
servo_gtk_web_view_respond_to_context_menu(ServoGtkWebView *self,
                                           guint64          request_id,
                                           gsize            item_index)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    if (self->servo != NULL) {
        servo_webview_context_menu_respond(self->servo, request_id, item_index);
    }
}

gboolean
servo_gtk_web_view_accept_new_web_view(ServoGtkWebView *self,
                                       guint64          request_id,
                                       ServoGtkWebView *popup)
{
    ServoWebViewHandle *handle;
    GtkWidget          *widget;
    gint                scale;
    guint               width;
    guint               height;

    g_return_val_if_fail(SERVO_GTK_IS_WEB_VIEW(self), FALSE);
    g_return_val_if_fail(SERVO_GTK_IS_WEB_VIEW(popup), FALSE);
    g_return_val_if_fail(popup->servo == NULL, FALSE);

    if (self->servo == NULL) {
        return FALSE;
    }

    widget = GTK_WIDGET(self);
    scale = MAX(1, gtk_widget_get_scale_factor(widget));

    /*
     * The popup widget may not be allocated yet, so start it at the parent's
     * size; its own allocation resizes it.
     */
    width = (guint) MAX(1, SERVO_GTK_WIDGET_WIDTH(widget)) * (guint) scale;
    height = (guint) MAX(1, SERVO_GTK_WIDGET_HEIGHT(widget)) * (guint) scale;

    handle = servo_webview_create_popup(self->servo, request_id, width, height);
    if (handle == NULL) {
        return FALSE;
    }

    servo_gtk_web_view_attach_servo(popup, handle, scale);

    return TRUE;
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
    klass->context_menu = servo_gtk_web_view_default_context_menu;

    /* Drawing and input are wired up differently by each toolkit version. */
    servo_gtk_web_view_class_init_toolkit(klass);

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

    /**
     * ServoGtkWebView::context-menu:
     * @self: the #ServoGtkWebView
     * @x: the x coordinate to open the menu at, in widget coordinates
     * @y: the y coordinate to open the menu at, in widget coordinates
     * @labels: (array zero-terminated=1): the item labels, in order; a
     *   separator appears as an empty string so that indices line up
     * @request_id: identifies this menu when answering it
     *
     * Emitted when the user asks for a context menu on web content. The default
     * handler presents the menu and answers it.
     *
     * Return %TRUE from a handler to present your own menu; you must then call
     * servo_gtk_web_view_respond_to_context_menu() with @request_id and the
     * index of the chosen item, or %SERVO_GTK_CONTEXT_MENU_NO_SELECTION to
     * dismiss. Items of your own that Servo knows nothing about are yours to
     * act on; dismiss Servo's menu in that case.
     *
     * Returns: %TRUE to stop the built-in menu from being shown
     */
    signals[CONTEXT_MENU] =
        g_signal_new(
            "context-menu",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_LAST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, context_menu),
            g_signal_accumulator_true_handled, NULL,
            NULL,       /* default (generic) C marshaller */
            G_TYPE_BOOLEAN,
            4,
            G_TYPE_INT,
            G_TYPE_INT,
            G_TYPE_STRV,
            G_TYPE_UINT64
        );

    /**
     * ServoGtkWebView::create-web-view:
     * @self: the #ServoGtkWebView the request came from
     * @request_id: identifies this popup request when accepting it
     *
     * Emitted when web content asks to open a new webview — `window.open()`, or
     * a link with `target="_blank"`.
     *
     * To open the popup, create a #ServoGtkWebView, place it in a window and
     * call servo_gtk_web_view_accept_new_web_view() with @request_id from
     * inside the handler, then return %TRUE. Returning %FALSE, or not handling
     * the signal at all, refuses the popup — which is the default, since
     * nothing at this level can decide where a new window belongs.
     *
     * Returns: %TRUE if the popup was accepted
     */
    signals[CREATE_WEB_VIEW] =
        g_signal_new(
            "create-web-view",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_LAST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, create_web_view),
            g_signal_accumulator_true_handled, NULL,
            NULL,       /* default (generic) C marshaller */
            G_TYPE_BOOLEAN,
            1,
            G_TYPE_UINT64
        );

    /**
     * ServoGtkWebView::close:
     * @self: the #ServoGtkWebView
     *
     * Emitted when the page closes its own webview — `window.close()`, or the
     * page that opened a popup closing it. Take down whatever window is showing
     * @self and drop your reference to it.
     */
    signals[CLOSE] =
        g_signal_new(
            "close",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_LAST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, close),
            NULL, NULL, /* accumulator */
            NULL,       /* default (generic) C marshaller */
            G_TYPE_NONE,
            0
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

    g_signal_connect(self,
                     "notify::scale-factor",
                     G_CALLBACK(servo_gtk_web_view_on_scale_factor_changed),
                     NULL);

    /* Focus, drawing and input, which differ between the toolkit versions. */
    servo_gtk_web_view_init_toolkit(self);

    self->tick_id = gtk_widget_add_tick_callback(widget, servo_gtk_web_view_tick, NULL, NULL);
}

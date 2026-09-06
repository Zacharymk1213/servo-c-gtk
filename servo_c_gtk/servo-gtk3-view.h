#ifndef SERVO_GTK_WEB_VIEW_H
#define SERVO_GTK_WEB_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Opaque Servo webview handle, defined by the Rust FFI (libservoshell/servo-webview.h). */
typedef struct ServoWebViewHandle ServoWebViewHandle;

#define SERVO_GTK_TYPE_WEB_VIEW                (servo_gtk_web_view_get_type ())
#define SERVO_GTK_WEB_VIEW(obj)                (G_TYPE_CHECK_INSTANCE_CAST ((obj), SERVO_GTK_TYPE_WEB_VIEW, ServoGtkWebView))
#define SERVO_GTK_TYPE_WEB_VIEW_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST ((klass), SERVO_GTK_TYPE_WEB_VIEW, ServoGtkWebViewClass))
#define SERVO_GTK_IS_WEB_VIEW(obj)             (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SERVO_GTK_TYPE_WEB_VIEW))
#define SERVO_GTK_IS_WEB_VIEW_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE ((klass), SERVO_GTK_TYPE_WEB_VIEW))
#define SERVO_GTK_IS_WEB_VIEW_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), SERVO_GTK_TYPE_WEB_VIEW, ServoGtkWebViewClass))


/**
 * ServoGtkLoadEvent:
 * @SERVO_GTK_LOAD_STARTED: a new load has begun; its headers are not parsed yet
 * @SERVO_GTK_LOAD_COMMITTED: the &lt;head&gt; has been parsed and the document
 *   body is reachable from script
 * @SERVO_GTK_LOAD_FINISHED: the document and all its subresources have loaded
 *
 * The stage a load reported by #ServoGtkWebView::load-changed has reached.
 */
typedef enum {
    SERVO_GTK_LOAD_STARTED,
    SERVO_GTK_LOAD_COMMITTED,
    SERVO_GTK_LOAD_FINISHED
} ServoGtkLoadEvent;

#define SERVO_GTK_TYPE_LOAD_EVENT              (servo_gtk_load_event_get_type ())

GType servo_gtk_load_event_get_type (void) G_GNUC_CONST;

/**
 * ServoGtkScriptDialogType:
 * @SERVO_GTK_SCRIPT_DIALOG_ALERT: an `alert()`: a message and a single button
 * @SERVO_GTK_SCRIPT_DIALOG_CONFIRM: a `confirm()`: a message with accept and
 *   cancel buttons
 * @SERVO_GTK_SCRIPT_DIALOG_PROMPT: a `prompt()`: a message, a text entry
 *   prefilled with the page's default, and accept and cancel buttons
 *
 * The kind of dialog web content opened, as reported by
 * #ServoGtkWebView::script-dialog.
 */
typedef enum {
    SERVO_GTK_SCRIPT_DIALOG_ALERT,
    SERVO_GTK_SCRIPT_DIALOG_CONFIRM,
    SERVO_GTK_SCRIPT_DIALOG_PROMPT
} ServoGtkScriptDialogType;

#define SERVO_GTK_TYPE_SCRIPT_DIALOG_TYPE      (servo_gtk_script_dialog_type_get_type ())

GType servo_gtk_script_dialog_type_get_type (void) G_GNUC_CONST;

/**
 * ServoGtkPermissionFeature:
 * @SERVO_GTK_PERMISSION_GEOLOCATION: the Geolocation API
 * @SERVO_GTK_PERMISSION_NOTIFICATIONS: desktop notifications
 * @SERVO_GTK_PERMISSION_PUSH: the Push API
 * @SERVO_GTK_PERMISSION_MIDI: MIDI device access
 * @SERVO_GTK_PERMISSION_CAMERA: camera capture
 * @SERVO_GTK_PERMISSION_MICROPHONE: microphone capture
 * @SERVO_GTK_PERMISSION_SPEAKER: speaker selection
 * @SERVO_GTK_PERMISSION_DEVICE_INFO: enumerating media devices
 * @SERVO_GTK_PERMISSION_BACKGROUND_SYNC: the Background Sync API
 * @SERVO_GTK_PERMISSION_BLUETOOTH: Web Bluetooth
 * @SERVO_GTK_PERMISSION_PERSISTENT_STORAGE: persistent storage
 * @SERVO_GTK_PERMISSION_SCREEN_WAKE_LOCK: the Screen Wake Lock API
 *
 * The capability a page asked for in #ServoGtkWebView::permission-request.
 */
typedef enum {
    SERVO_GTK_PERMISSION_GEOLOCATION,
    SERVO_GTK_PERMISSION_NOTIFICATIONS,
    SERVO_GTK_PERMISSION_PUSH,
    SERVO_GTK_PERMISSION_MIDI,
    SERVO_GTK_PERMISSION_CAMERA,
    SERVO_GTK_PERMISSION_MICROPHONE,
    SERVO_GTK_PERMISSION_SPEAKER,
    SERVO_GTK_PERMISSION_DEVICE_INFO,
    SERVO_GTK_PERMISSION_BACKGROUND_SYNC,
    SERVO_GTK_PERMISSION_BLUETOOTH,
    SERVO_GTK_PERMISSION_PERSISTENT_STORAGE,
    SERVO_GTK_PERMISSION_SCREEN_WAKE_LOCK
} ServoGtkPermissionFeature;

#define SERVO_GTK_TYPE_PERMISSION_FEATURE      (servo_gtk_permission_feature_get_type ())

GType servo_gtk_permission_feature_get_type (void) G_GNUC_CONST;

typedef struct _ServoGtkWebView              ServoGtkWebView;
typedef struct _ServoGtkWebViewPrivate       ServoGtkWebViewPrivate;
typedef struct _ServoGtkWebViewClass         ServoGtkWebViewClass;

GType servo_gtk_web_view_get_type (void) G_GNUC_CONST;

struct _ServoGtkWebView
{
    GtkDrawingArea web_view;

    gchar *uri;

    /* Servo FFI state. */
    ServoWebViewHandle *servo;
    GdkPixbuf          *frame;
    guint               tick_id;

    /*< private >*/
    ServoGtkWebViewPrivate *priv;
};

struct _ServoGtkWebViewClass {
    GtkDrawingAreaClass parent_class;

    /* Signals */
    void (*uri_changed) (ServoGtkWebView *web_view,
                         const gchar     *uri);
    void (*load_changed) (ServoGtkWebView   *web_view,
                          ServoGtkLoadEvent  load_event);
    gboolean (*script_dialog) (ServoGtkWebView          *web_view,
                               ServoGtkScriptDialogType  dialog_type,
                               const gchar              *message,
                               const gchar              *default_value,
                               guint64                   request_id);
    void (*script_dialog_cancelled) (ServoGtkWebView *web_view,
                                     guint64          request_id);
    gboolean (*run_file_chooser) (ServoGtkWebView    *web_view,
                                  const gchar *const *filter_patterns,
                                  gboolean            allow_multiple,
                                  guint64             request_id);
    gboolean (*authenticate) (ServoGtkWebView *web_view,
                              const gchar     *uri,
                              gboolean         for_proxy,
                              guint64          request_id);
    gboolean (*permission_request) (ServoGtkWebView           *web_view,
                                    ServoGtkPermissionFeature  feature,
                                    guint64                    request_id);
    gboolean (*context_menu) (ServoGtkWebView    *web_view,
                              gint                x,
                              gint                y,
                              const gchar *const *labels,
                              guint64             request_id);

    /*
     * Padding for future expansion. The original four slots were consumed by
     * the signals above; these replenish them, so the class struct grows once
     * here rather than on the next signal added.
     */
    void (*_gtk_reserved1) (void);
    void (*_gtk_reserved2) (void);
    void (*_gtk_reserved3) (void);
    void (*_gtk_reserved4) (void);
};

/**
 * servo_gtk_web_view_new:
 *
 * Creates a new Servo GTK web view widget.
 *
 * Returns: (transfer full): a newly-created #ServoGtkWebView
 */
ServoGtkWebView *servo_gtk_web_view_new(void);

/**
 * servo_gtk_web_view_load_uri:
 * @self: a #ServoGtkWebView
 * @uri: URI to load
 *
 * Loads the given URI.
 */
void servo_gtk_web_view_load_uri(ServoGtkWebView *self, const gchar *uri);

/**
 * servo_gtk_web_view_get_uri:
 * @self: a #ServoGtkWebView
 *
 * Gets the currently loaded URI.
 *
 * Returns: (nullable): the current URI
 */
const gchar *servo_gtk_web_view_get_uri(ServoGtkWebView *self);

/**
 * servo_gtk_web_view_get_title:
 * @self: a #ServoGtkWebView
 *
 * Gets the title of the loaded page.
 *
 * Returns: (nullable): the page title, or %NULL if the page has no title. The
 *   string is owned by @self and is valid until the title changes again.
 */
const gchar *servo_gtk_web_view_get_title(ServoGtkWebView *self);

/**
 * servo_gtk_web_view_is_loading:
 * @self: a #ServoGtkWebView
 *
 * Whether a load is currently in progress, i.e. #ServoGtkWebView::load-changed
 * has reported %SERVO_GTK_LOAD_STARTED without a matching
 * %SERVO_GTK_LOAD_FINISHED yet.
 *
 * Returns: %TRUE while a load is in progress
 */
gboolean servo_gtk_web_view_is_loading(ServoGtkWebView *self);

/**
 * servo_gtk_web_view_can_go_back:
 * @self: a #ServoGtkWebView
 *
 * Whether there is a previous entry in the session history, i.e. whether
 * servo_gtk_web_view_go_back() would do anything.
 *
 * Returns: %TRUE if the web view can go back
 */
gboolean servo_gtk_web_view_can_go_back(ServoGtkWebView *self);

/**
 * servo_gtk_web_view_can_go_forward:
 * @self: a #ServoGtkWebView
 *
 * Whether there is a following entry in the session history, i.e. whether
 * servo_gtk_web_view_go_forward() would do anything.
 *
 * Returns: %TRUE if the web view can go forward
 */
gboolean servo_gtk_web_view_can_go_forward(ServoGtkWebView *self);

/**
 * servo_gtk_web_view_set_zoom_level:
 * @self: a #ServoGtkWebView
 * @zoom_level: the zoom level, where 1.0 is unzoomed
 *
 * Sets the page zoom level, as a browser's Ctrl+/Ctrl- would: the page's
 * devicePixelRatio changes and the page re-lays out, rather than the rendered
 * result simply being magnified. The value is clamped to [0.1, 10.0].
 */
void servo_gtk_web_view_set_zoom_level(ServoGtkWebView *self, gdouble zoom_level);

/**
 * servo_gtk_web_view_get_zoom_level:
 * @self: a #ServoGtkWebView
 *
 * Gets the page zoom level set by servo_gtk_web_view_set_zoom_level().
 *
 * Returns: the current zoom level, where 1.0 is unzoomed
 */
gdouble servo_gtk_web_view_get_zoom_level(ServoGtkWebView *self);

/**
 * servo_gtk_web_view_respond_to_dialog:
 * @self: a #ServoGtkWebView
 * @request_id: the id from the #ServoGtkWebView::script-dialog emission
 * @accepted: %TRUE if the user accepted the dialog, %FALSE if they cancelled
 * @text: (nullable): for a %SERVO_GTK_SCRIPT_DIALOG_PROMPT, the text the user
 *   entered; %NULL keeps the page's default. Ignored for other dialog types.
 *
 * Answers a dialog reported by #ServoGtkWebView::script-dialog. Only needed by
 * a handler that returned %TRUE to present its own dialog — the built-in one
 * answers on its own.
 *
 * The page's script is blocked until a dialog is answered, so every handled
 * dialog must eventually be answered. Answering a @request_id that has already
 * been answered, or that Servo withdrew (see
 * #ServoGtkWebView::script-dialog-cancelled), does nothing.
 */
void servo_gtk_web_view_respond_to_dialog(ServoGtkWebView *self,
                                          guint64          request_id,
                                          gboolean         accepted,
                                          const gchar     *text);

/**
 * servo_gtk_web_view_respond_to_file_chooser:
 * @self: a #ServoGtkWebView
 * @request_id: the id from the #ServoGtkWebView::run-file-chooser emission
 * @paths: (array zero-terminated=1) (nullable): the chosen file paths, or
 *   %NULL if the chooser was dismissed with no selection
 *
 * Answers a file chooser reported by #ServoGtkWebView::run-file-chooser. Only
 * needed by a handler that returned %TRUE to present its own chooser — the
 * built-in one answers on its own.
 *
 * The page's script is blocked until the chooser is answered. Answering a
 * @request_id that has already been answered, or that Servo withdrew, does
 * nothing.
 */
void servo_gtk_web_view_respond_to_file_chooser(ServoGtkWebView    *self,
                                                guint64             request_id,
                                                const gchar *const *paths);

/**
 * servo_gtk_web_view_respond_to_authentication:
 * @self: a #ServoGtkWebView
 * @request_id: the id from the #ServoGtkWebView::authenticate emission
 * @username: (nullable): the username to authenticate with
 * @password: (nullable): the password to authenticate with
 *
 * Answers an authentication challenge reported by
 * #ServoGtkWebView::authenticate. Passing %NULL for either @username or
 * @password declines to supply credentials and the load fails as
 * unauthenticated.
 *
 * Only needed by a handler that returned %TRUE to prompt for credentials
 * itself — the built-in prompt answers on its own.
 */
void servo_gtk_web_view_respond_to_authentication(ServoGtkWebView *self,
                                                  guint64          request_id,
                                                  const gchar     *username,
                                                  const gchar     *password);

/**
 * servo_gtk_web_view_respond_to_permission_request:
 * @self: a #ServoGtkWebView
 * @request_id: the id from the #ServoGtkWebView::permission-request emission
 * @allowed: %TRUE to grant the capability, %FALSE to refuse it
 *
 * Answers a permission request reported by
 * #ServoGtkWebView::permission-request. A request that is never answered is
 * denied.
 */
void servo_gtk_web_view_respond_to_permission_request(ServoGtkWebView *self,
                                                      guint64          request_id,
                                                      gboolean         allowed);

/**
 * SERVO_GTK_CONTEXT_MENU_NO_SELECTION:
 *
 * Passed to servo_gtk_web_view_respond_to_context_menu() to dismiss a context
 * menu without choosing anything.
 */
#define SERVO_GTK_CONTEXT_MENU_NO_SELECTION ((gsize) -1)

/**
 * servo_gtk_web_view_respond_to_context_menu:
 * @self: a #ServoGtkWebView
 * @request_id: the id from the #ServoGtkWebView::context-menu emission
 * @item_index: the index of the chosen item, or
 *   %SERVO_GTK_CONTEXT_MENU_NO_SELECTION to dismiss
 *
 * Answers a context menu reported by #ServoGtkWebView::context-menu. An
 * out-of-range index, or the index of a separator, dismisses the menu.
 *
 * Only needed by a handler that returned %TRUE to present its own menu — the
 * built-in one answers on its own.
 */
void servo_gtk_web_view_respond_to_context_menu(ServoGtkWebView *self,
                                                guint64          request_id,
                                                gsize            item_index);

/**
 * servo_gtk_web_view_reload:
 * @self: a #ServoGtkWebView
 *
 * Reloads the current page. Does nothing if the web view has not been
 * realized yet (Servo is created on first allocation).
 */
void servo_gtk_web_view_reload(ServoGtkWebView *self);

/**
 * servo_gtk_web_view_go_back:
 * @self: a #ServoGtkWebView
 *
 * Navigates one entry back in the session history. Does nothing if there is
 * no previous entry, or if the web view has not been realized yet.
 */
void servo_gtk_web_view_go_back(ServoGtkWebView *self);

/**
 * servo_gtk_web_view_go_forward:
 * @self: a #ServoGtkWebView
 *
 * Navigates one entry forward in the session history. Does nothing if there
 * is no next entry, or if the web view has not been realized yet.
 */
void servo_gtk_web_view_go_forward(ServoGtkWebView *self);

/**
 * ServoGtkScriptResultCallback:
 * @web_view: the #ServoGtkWebView the script ran in
 * @result_json: (nullable): the script's return value serialized as a JSON
 *   string, or %NULL if evaluation failed
 * @error: (nullable): a human-readable error message, or %NULL on success
 * @user_data: the user data passed to servo_gtk_web_view_evaluate_script()
 *
 * Invoked exactly once when an asynchronous script evaluation finishes.
 * Exactly one of @result_json and @error is non-%NULL. Both strings are only
 * valid for the duration of the call.
 */
typedef void (*ServoGtkScriptResultCallback) (
    ServoGtkWebView *web_view,
    const gchar     *result_json,
    const gchar     *error,
    gpointer         user_data
);

/**
 * servo_gtk_web_view_evaluate_script:
 * @self: a #ServoGtkWebView
 * @script: the JavaScript source to evaluate
 * @callback: (scope async) (closure user_data): callback invoked once with
 *   the result
 * @user_data: user data passed to @callback
 *
 * Asynchronously evaluates @script in the web view's top-level browsing
 * context. When evaluation finishes @callback is invoked exactly once, later,
 * from the GTK main loop.
 */
void servo_gtk_web_view_evaluate_script(
    ServoGtkWebView                  *self,
    const gchar                      *script,
    ServoGtkScriptResultCallback      callback,
    gpointer                          user_data
);

G_END_DECLS

#endif /* SERVO_GTK_WEB_VIEW_H */

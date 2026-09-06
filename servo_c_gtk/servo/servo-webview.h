/*
 * C ABI for libservoshell — a thin wrapper around Servo's WebView.
 *
 * All functions must be called from a single thread (the GTK main thread):
 * the underlying Servo objects are not thread-safe.
 *
 * Typical use from a GtkDrawingArea:
 *   1. servo_webview_new()
 *   2. servo_webview_set_frame_ready_callback() -> copy RGBA into a texture
 *   3. drive servo_webview_spin() from a GtkWidget tick callback
 *   4. forward input via servo_webview_pointer_* / servo_webview_scroll
 *   5. servo_webview_free() on dispose
 */
#ifndef SERVO_WEBVIEW_H
#define SERVO_WEBVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a single Servo webview. */
typedef struct ServoWebViewHandle ServoWebViewHandle;

/*
 * Invoked once per rendered frame with a tightly-packed RGBA8 buffer
 * (stride == width * 4). The pointer is valid only for the duration of the
 * call — copy the pixels before returning.
 */
typedef void (*ServoFrameReadyCallback)(const uint8_t *rgba,
                                        uint32_t       width,
                                        uint32_t       height,
                                        void          *user_data);

/*
 * Invoked when the page requests a different cursor. `name` is a
 * NUL-terminated CSS cursor name (e.g. "pointer"), valid only for the call.
 */
typedef void (*ServoCursorChangedCallback)(const char *name,
                                           void       *user_data);

/*
 * Invoked when the webview navigates to a new URL (link, redirect, history
 * traversal, or an embedder-issued load). `url` is a NUL-terminated UTF-8
 * string valid only for the duration of the call — copy it before returning.
 */
typedef void (*ServoUrlChangedCallback)(const char *url,
                                        void       *user_data);

/*
 * Invoked when the page title changes. `title` is a NUL-terminated UTF-8
 * string valid only for the duration of the call, or NULL when the page has no
 * title (a fresh navigation clears it).
 */
typedef void (*ServoTitleChangedCallback)(const char *title,
                                          void       *user_data);

/* How far the current load has progressed. */
typedef enum {
    /* The load has started; the headers have not been parsed yet. */
    SERVO_LOAD_STATUS_STARTED = 0,
    /* <head> has been parsed; the document body is now reachable from script. */
    SERVO_LOAD_STATUS_HEAD_PARSED = 1,
    /* The document and all its subresources have loaded. */
    SERVO_LOAD_STATUS_COMPLETE = 2
} ServoLoadStatus;

/* Invoked when the load progresses. `status` is a ServoLoadStatus value. */
typedef void (*ServoLoadStatusChangedCallback)(uint32_t status,
                                               void    *user_data);

/*
 * Invoked when the session history changes — on navigation and on history
 * traversal — with the new availability of the back and forward entries.
 */
typedef void (*ServoHistoryChangedCallback)(bool  can_go_back,
                                            bool  can_go_forward,
                                            void *user_data);

/* Which of the script-initiated dialogs web content opened. */
typedef enum {
    SERVO_DIALOG_ALERT = 0,
    SERVO_DIALOG_CONFIRM = 1,
    SERVO_DIALOG_PROMPT = 2
} ServoDialogType;

/*
 * Invoked when web content opens a simple dialog — alert(), confirm() or
 * prompt(). The dialog is NOT answered when this returns: show your own UI and
 * call servo_webview_dialog_respond() with `request_id` once the user answers.
 * The page's script stays blocked until you do.
 *
 * `message` is page-controlled text, valid only for the duration of the call,
 * so a dialog showing it must not be mistakable for browser UI.
 * `default_value` is a prompt's initial text and is NULL for the other kinds.
 */
typedef void (*ServoDialogCallback)(uint64_t    request_id,
                                    uint32_t    dialog_type,
                                    const char *message,
                                    const char *default_value,
                                    void       *user_data);

/*
 * Invoked when web content activates an <input type=file>. The picker is NOT
 * answered when this returns: show a file chooser and call
 * servo_webview_file_picker_respond() with `request_id` once the user answers.
 *
 * `filter_patterns` holds `filter_pattern_count` bare filename extensions with
 * no leading dot (e.g. "png"), valid only for the duration of the call; an
 * empty array means any file is acceptable.
 */
typedef void (*ServoFilePickerCallback)(uint64_t           request_id,
                                        const char *const *filter_patterns,
                                        size_t             filter_pattern_count,
                                        bool               allow_multiple,
                                        void              *user_data);

/* What a context-menu item does when chosen. */
typedef enum {
    SERVO_CONTEXT_MENU_ACTION_GO_BACK = 0,
    SERVO_CONTEXT_MENU_ACTION_GO_FORWARD = 1,
    SERVO_CONTEXT_MENU_ACTION_RELOAD = 2,
    SERVO_CONTEXT_MENU_ACTION_COPY_LINK = 3,
    SERVO_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WEBVIEW = 4,
    SERVO_CONTEXT_MENU_ACTION_COPY_IMAGE_LINK = 5,
    SERVO_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_VIEW = 6,
    SERVO_CONTEXT_MENU_ACTION_CUT = 7,
    SERVO_CONTEXT_MENU_ACTION_COPY = 8,
    SERVO_CONTEXT_MENU_ACTION_PASTE = 9,
    SERVO_CONTEXT_MENU_ACTION_SELECT_ALL = 10
} ServoContextMenuAction;

/* One entry of a context menu. */
typedef struct {
    /* The item's text, or NULL when this entry is a separator. */
    const char *label;
    /* A ServoContextMenuAction; meaningless for a separator. */
    uint32_t    action;
    /* Whether the item can be chosen. */
    bool        enabled;
} ServoContextMenuItem;

/* Passed to servo_webview_context_menu_respond() to dismiss with no selection. */
#define SERVO_CONTEXT_MENU_NO_SELECTION ((size_t) -1)

/*
 * Invoked when the user asks for a context menu on web content. The menu is NOT
 * answered when this returns: show your own menu and call
 * servo_webview_context_menu_respond() with `request_id` and the index of the
 * chosen item.
 *
 * `items` holds `item_count` entries valid only for the duration of the call.
 * `x` and `y` are the top-left of the element the menu was opened on, in device
 * pixels.
 */
typedef void (*ServoContextMenuCallback)(uint64_t                    request_id,
                                         const ServoContextMenuItem *items,
                                         size_t                      item_count,
                                         int32_t                     x,
                                         int32_t                     y,
                                         void                       *user_data);

/*
 * Invoked when web content asks to open a new webview — window.open(), or a
 * link with target="_blank". Accept it by calling servo_webview_create_popup()
 * with `request_id` from inside this callback; returning without accepting
 * refuses the popup.
 */
typedef void (*ServoCreateWebViewCallback)(uint64_t request_id,
                                           void    *user_data);

/*
 * Invoked when a webview closes itself — window.close(), or the page that
 * opened a popup closing it. Take down whatever window is showing this webview
 * and free its handle.
 */
typedef void (*ServoClosedCallback)(void *user_data);

/*
 * Invoked when a server or proxy issues an HTTP authentication challenge. The
 * request is NOT answered when this returns: prompt for credentials and call
 * servo_webview_authentication_respond() with `request_id`.
 *
 * `url` is the URL that triggered the challenge, valid only for the duration of
 * the call. `for_proxy` distinguishes a proxy challenge from an origin one; the
 * two must not be confused, since credentials for one are not credentials for
 * the other.
 */
typedef void (*ServoAuthenticationCallback)(uint64_t    request_id,
                                            const char *url,
                                            bool        for_proxy,
                                            void       *user_data);

/* A permission-gated capability a page has asked for. */
typedef enum {
    SERVO_PERMISSION_GEOLOCATION = 0,
    SERVO_PERMISSION_NOTIFICATIONS = 1,
    SERVO_PERMISSION_PUSH = 2,
    SERVO_PERMISSION_MIDI = 3,
    SERVO_PERMISSION_CAMERA = 4,
    SERVO_PERMISSION_MICROPHONE = 5,
    SERVO_PERMISSION_SPEAKER = 6,
    SERVO_PERMISSION_DEVICE_INFO = 7,
    SERVO_PERMISSION_BACKGROUND_SYNC = 8,
    SERVO_PERMISSION_BLUETOOTH = 9,
    SERVO_PERMISSION_PERSISTENT_STORAGE = 10,
    SERVO_PERMISSION_SCREEN_WAKE_LOCK = 11
} ServoPermissionFeature;

/*
 * Invoked when a page asks for a permission-gated capability. The request is
 * NOT answered when this returns; call servo_webview_permission_respond() with
 * `request_id`. Never answering denies the request.
 */
typedef void (*ServoPermissionCallback)(uint64_t request_id,
                                        uint32_t feature,
                                        void    *user_data);

/*
 * Invoked when Servo withdraws a request that has not been answered — the page
 * navigated away, or the element left the document. Take down whatever UI is
 * showing for `request_id`; responding to it afterwards does nothing.
 */
typedef void (*ServoRequestCancelledCallback)(uint64_t request_id,
                                              void    *user_data);

/*
 * Create a webview with an initial surface of width x height device pixels.
 * `initial_uri` is the URL to load on creation, or NULL to start on
 * about:blank. The initial URL MUST be supplied here rather than via a
 * separate servo_webview_load_uri() call immediately after creation: Servo
 * creates the browsing context together with this URL, and a load issued
 * before that context exists is silently dropped. Returns NULL on failure;
 * free with servo_webview_free().
 */
ServoWebViewHandle *servo_webview_new(uint32_t    width,
                                      uint32_t    height,
                                      const char *initial_uri);

/* Destroy a handle. NULL is a no-op. */
void servo_webview_free(ServoWebViewHandle *webview);

/* Register/clear callbacks (pass NULL to clear). */
void servo_webview_set_frame_ready_callback(ServoWebViewHandle     *webview,
                                            ServoFrameReadyCallback callback,
                                            void                   *user_data);
void servo_webview_set_cursor_changed_callback(ServoWebViewHandle        *webview,
                                               ServoCursorChangedCallback callback,
                                               void                      *user_data);
void servo_webview_set_url_changed_callback(ServoWebViewHandle     *webview,
                                            ServoUrlChangedCallback callback,
                                            void                   *user_data);
void servo_webview_set_title_changed_callback(ServoWebViewHandle       *webview,
                                              ServoTitleChangedCallback callback,
                                              void                     *user_data);
void servo_webview_set_load_status_changed_callback(
    ServoWebViewHandle            *webview,
    ServoLoadStatusChangedCallback callback,
    void                          *user_data);
void servo_webview_set_history_changed_callback(ServoWebViewHandle         *webview,
                                                ServoHistoryChangedCallback callback,
                                                void                       *user_data);
void servo_webview_set_dialog_callback(ServoWebViewHandle *webview,
                                       ServoDialogCallback callback,
                                       void               *user_data);
void servo_webview_set_request_cancelled_callback(
    ServoWebViewHandle           *webview,
    ServoRequestCancelledCallback callback,
    void                         *user_data);

/*
 * Answer a dialog reported through a ServoDialogCallback. `accepted` is whether
 * the user pressed the affirmative button (ignored for alerts, which have only
 * one). `text` is a prompt's entered text and is ignored for the other kinds;
 * NULL keeps the default that came with the dialog.
 *
 * An unknown `request_id` — already answered, or withdrawn by Servo — is
 * ignored, so a host racing a withdrawal against a click need not track which
 * happened first.
 */
void servo_webview_dialog_respond(ServoWebViewHandle *webview,
                                  uint64_t            request_id,
                                  bool                accepted,
                                  const char         *text);

void servo_webview_set_file_picker_callback(ServoWebViewHandle     *webview,
                                            ServoFilePickerCallback callback,
                                            void                   *user_data);

/*
 * Answer a file picker reported through a ServoFilePickerCallback. `paths` is
 * an array of `path_count` file paths the user chose; a NULL `paths` or a
 * `path_count` of 0 means the picker was dismissed with no selection. An
 * unknown `request_id` is ignored.
 */
void servo_webview_file_picker_respond(ServoWebViewHandle *webview,
                                       uint64_t            request_id,
                                       const char *const  *paths,
                                       size_t              path_count);

void servo_webview_set_authentication_callback(ServoWebViewHandle         *webview,
                                               ServoAuthenticationCallback callback,
                                               void                       *user_data);
void servo_webview_set_permission_callback(ServoWebViewHandle     *webview,
                                           ServoPermissionCallback callback,
                                           void                   *user_data);

/*
 * Answer an authentication challenge reported through a
 * ServoAuthenticationCallback. Supply both `username` and `password` to
 * authenticate; a NULL for either declines to supply credentials and the load
 * fails as unauthenticated. An unknown `request_id` is ignored.
 */
void servo_webview_authentication_respond(ServoWebViewHandle *webview,
                                          uint64_t            request_id,
                                          const char         *username,
                                          const char         *password);

/*
 * Answer a permission request reported through a ServoPermissionCallback. A
 * request that is never answered is denied. An unknown `request_id` is ignored.
 */
void servo_webview_permission_respond(ServoWebViewHandle *webview,
                                      uint64_t            request_id,
                                      bool                allowed);

void servo_webview_set_context_menu_callback(ServoWebViewHandle      *webview,
                                             ServoContextMenuCallback callback,
                                             void                    *user_data);

/*
 * Answer a context menu reported through a ServoContextMenuCallback.
 * `item_index` indexes the `items` array that came with the menu;
 * SERVO_CONTEXT_MENU_NO_SELECTION, an out-of-range index, or the index of a
 * separator all dismiss the menu with no selection. An unknown `request_id` is
 * ignored.
 */
void servo_webview_context_menu_respond(ServoWebViewHandle *webview,
                                        uint64_t            request_id,
                                        size_t              item_index);

void servo_webview_set_create_webview_callback(ServoWebViewHandle        *webview,
                                               ServoCreateWebViewCallback callback,
                                               void                      *user_data);
void servo_webview_set_closed_callback(ServoWebViewHandle *webview,
                                       ServoClosedCallback callback,
                                       void               *user_data);

/*
 * Accept a popup request reported through a ServoCreateWebViewCallback and build
 * its webview, with a surface of width x height device pixels. Must be called
 * from inside that callback: the request is refused as soon as it returns.
 *
 * The popup joins the parent's engine, so both handles must be driven from the
 * same thread; spinning either one advances both. The returned handle is owned
 * by the caller and must be freed with servo_webview_free(), and starts with no
 * callbacks registered — register them before spinning or the first frame is
 * lost. Returns NULL for an unknown `request_id` or if the surface could not be
 * created, in which case the popup is refused.
 */
ServoWebViewHandle *servo_webview_create_popup(ServoWebViewHandle *webview,
                                               uint64_t            request_id,
                                               uint32_t            width,
                                               uint32_t            height);

/* Navigation. */
void servo_webview_load_uri(ServoWebViewHandle *webview, const char *uri);
void servo_webview_reload(ServoWebViewHandle *webview);
void servo_webview_go_back(ServoWebViewHandle *webview);
void servo_webview_go_forward(ServoWebViewHandle *webview);

/* Whether the matching history entry exists to traverse to. */
bool servo_webview_can_go_back(ServoWebViewHandle *webview);
bool servo_webview_can_go_forward(ServoWebViewHandle *webview);

/* Surface size in device pixels. */
void servo_webview_resize(ServoWebViewHandle *webview,
                          uint32_t            width,
                          uint32_t            height);

/*
 * Device pixels per logical (device-independent) pixel, e.g. 2.0 on a doubled
 * display. The surface size above is always in device pixels; this is what
 * tells the page how large a CSS pixel is, so window.devicePixelRatio, media
 * queries and layout come out right rather than the page being laid out at
 * half size and upscaled. Non-finite or non-positive values are ignored.
 */
void servo_webview_set_hidpi_scale_factor(ServoWebViewHandle *webview,
                                          float               scale);

/*
 * Page zoom: 1.0 is unzoomed, 2.0 double size. This is the zoom a browser's
 * Ctrl+/Ctrl- applies — it changes the page's devicePixelRatio and makes it
 * re-lay out, rather than magnifying the rendered result. Servo clamps the
 * value to the inclusive range [0.1, 10.0]; non-finite values are ignored.
 */
void  servo_webview_set_zoom_level(ServoWebViewHandle *webview, float zoom);
float servo_webview_get_zoom_level(ServoWebViewHandle *webview);

/* Input. `button`: 1 = left, 2 = middle, 3 = right (GDK numbering). */
void servo_webview_pointer_move(ServoWebViewHandle *webview, double x, double y);
void servo_webview_pointer_button(ServoWebViewHandle *webview,
                                  uint32_t            button,
                                  bool                pressed,
                                  double              x,
                                  double              y);
void servo_webview_scroll(ServoWebViewHandle *webview, double dx, double dy);

/* The stage a touch point is at. */
typedef enum {
    SERVO_TOUCH_DOWN = 0,
    SERVO_TOUCH_MOVE = 1,
    SERVO_TOUCH_UP = 2,
    SERVO_TOUCH_CANCEL = 3
} ServoTouchPhase;

/*
 * Report a touch point at (x, y) in device pixels. `phase` is a
 * ServoTouchPhase.
 *
 * `touch_id` identifies one finger across its whole gesture: the same id must
 * be used for the DOWN, every MOVE, and the UP or CANCEL that ends it, and two
 * fingers on screen at once must have different ids. An unrecognised phase is
 * treated as CANCEL rather than dropped, so a touch the host has stopped
 * tracking cannot leave the page believing a finger is still down.
 */
void servo_webview_touch(ServoWebViewHandle *webview,
                         uint32_t            phase,
                         int32_t             touch_id,
                         double              x,
                         double              y);

/*
 * Named keys understood by servo_webview_key(). SERVO_KEY_CHARACTER means the
 * key produced text — pass its Unicode codepoint in the `unicode` argument. All
 * other values identify a non-printable key and ignore `unicode`. These values
 * are part of the ABI; keep them in sync with the `servo_key` module in
 * servo-webview.rs.
 */
typedef enum {
    SERVO_KEY_CHARACTER = 0,
    SERVO_KEY_UNIDENTIFIED = 1,
    SERVO_KEY_ENTER = 2,
    SERVO_KEY_TAB = 3,
    SERVO_KEY_BACKSPACE = 4,
    SERVO_KEY_DELETE = 5,
    SERVO_KEY_ESCAPE = 6,
    SERVO_KEY_ARROW_LEFT = 7,
    SERVO_KEY_ARROW_RIGHT = 8,
    SERVO_KEY_ARROW_UP = 9,
    SERVO_KEY_ARROW_DOWN = 10,
    SERVO_KEY_HOME = 11,
    SERVO_KEY_END = 12,
    SERVO_KEY_PAGE_UP = 13,
    SERVO_KEY_PAGE_DOWN = 14,
    SERVO_KEY_INSERT = 15,
    SERVO_KEY_F1 = 16,
    SERVO_KEY_F2 = 17,
    SERVO_KEY_F3 = 18,
    SERVO_KEY_F4 = 19,
    SERVO_KEY_F5 = 20,
    SERVO_KEY_F6 = 21,
    SERVO_KEY_F7 = 22,
    SERVO_KEY_F8 = 23,
    SERVO_KEY_F9 = 24,
    SERVO_KEY_F10 = 25,
    SERVO_KEY_F11 = 26,
    SERVO_KEY_F12 = 27
} ServoKey;

/* Modifier bitmask for servo_webview_key(). */
typedef enum {
    SERVO_MODIFIER_NONE    = 0,
    SERVO_MODIFIER_SHIFT   = 1 << 0,
    SERVO_MODIFIER_CONTROL = 1 << 1,
    SERVO_MODIFIER_ALT     = 1 << 2,
    SERVO_MODIFIER_META    = 1 << 3
} ServoModifier;

/*
 * Report a key press (`pressed`) or release. `key` is a ServoKey; when it is
 * SERVO_KEY_CHARACTER, `unicode` carries the typed character's Unicode
 * codepoint (0 otherwise). `modifiers` is a bitmask of ServoModifier flags.
 */
void servo_webview_key(ServoWebViewHandle *webview,
                       uint32_t            key,
                       uint32_t            unicode,
                       uint32_t            modifiers,
                       bool                pressed);

/*
 * Pump Servo's event loop once. Call regularly from a GTK tick/timeout source;
 * the frame-ready callback fires synchronously from inside this call.
 */
void servo_webview_spin(ServoWebViewHandle *webview);

/*
 * Current URL as a newly-allocated UTF-8 string, or NULL. Free with
 * servo_string_free().
 */
char *servo_webview_get_uri(ServoWebViewHandle *webview);

/*
 * Current page title as a newly-allocated UTF-8 string, or NULL if the page has
 * no title. Free with servo_string_free().
 */
char *servo_webview_get_title(ServoWebViewHandle *webview);

/*
 * How far the current load has progressed, as a ServoLoadStatus value. An
 * invalid handle reports SERVO_LOAD_STATUS_COMPLETE (nothing is loading).
 */
uint32_t servo_webview_get_load_status(ServoWebViewHandle *webview);

/* Free a string returned by this library. NULL is a no-op. */
void servo_string_free(char *string);


typedef void (*ServoScriptResultCallback)(const char *result_json,
                                          const char *error,
                                          void       *user_data);

void servo_webview_evaluate_script(ServoWebViewHandle        *webview,
                                   const char                *script,
                                   ServoScriptResultCallback  callback,
                                   void                      *user_data);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_WEBVIEW_H */

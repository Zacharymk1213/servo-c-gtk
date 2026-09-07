/*
 * Internal declarations shared by the GTK3 and GTK4 builds of ServoGtkWebView.
 *
 * Not installed and not part of the public API. The widget is built from three
 * sources per GTK major: this header, servo-gtk-view-common.c (everything that
 * does not depend on the toolkit version) and servo-gtk<N>-view.c (the parts
 * that do). SERVO_GTK_MAJOR selects which public header and which
 * compatibility macros apply.
 */
#ifndef SERVO_GTK_VIEW_PRIVATE_H
#define SERVO_GTK_VIEW_PRIVATE_H

#ifndef SERVO_GTK_MAJOR
#  error "SERVO_GTK_MAJOR must be defined as 3 or 4"
#endif

#if SERVO_GTK_MAJOR == 3
#  include "servo-gtk3-view.h"
#elif SERVO_GTK_MAJOR == 4
#  include "servo-gtk4-view.h"
#else
#  error "SERVO_GTK_MAJOR must be 3 or 4"
#endif

#include "servo-webview.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ *
 * Toolkit compatibility
 *
 * The handful of spellings that differ between GTK3 and GTK4 for operations
 * that are otherwise identical. Anything that differs in substance rather than
 * in name lives in the per-toolkit source instead.
 * ------------------------------------------------------------------ */

#if SERVO_GTK_MAJOR == 3
#  define SERVO_GTK_ALT_MASK              GDK_MOD1_MASK
#  define SERVO_GTK_WIDGET_WIDTH(widget)  gtk_widget_get_allocated_width(widget)
#  define SERVO_GTK_WIDGET_HEIGHT(widget) gtk_widget_get_allocated_height(widget)
#  define SERVO_GTK_DESTROY_WINDOW(w)     gtk_widget_destroy(w)
#  define SERVO_GTK_ENTRY_TEXT(entry)     gtk_entry_get_text(GTK_ENTRY(entry))
#  define SERVO_GTK_IM_SET_CLIENT(im, widget) \
    gtk_im_context_set_client_window(im, gtk_widget_get_window(widget))
#else
#  define SERVO_GTK_ALT_MASK              GDK_ALT_MASK
#  define SERVO_GTK_WIDGET_WIDTH(widget)  gtk_widget_get_width(widget)
#  define SERVO_GTK_WIDGET_HEIGHT(widget) gtk_widget_get_height(widget)
#  define SERVO_GTK_DESTROY_WINDOW(w)     gtk_window_destroy(GTK_WINDOW(w))
#  define SERVO_GTK_ENTRY_TEXT(entry)     gtk_editable_get_text(GTK_EDITABLE(entry))
#  define SERVO_GTK_IM_SET_CLIENT(im, widget) gtk_im_context_set_client_widget(im, widget)
#endif

/* ------------------------------------------------------------------ *
 * Instance state
 * ------------------------------------------------------------------ */

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
     * Dialog windows the built-in handlers put up, keyed by request id, so a
     * dialog Servo withdraws can be taken back down. Values are unowned: each
     * window removes itself from here when it is destroyed.
     */
    GHashTable *dialogs;
    /*
     * The items of the context menu whose ::context-menu emission is in flight.
     * The emission is synchronous and the array belongs to the FFI callback for
     * exactly its duration, so only the class closure reads this, and only from
     * inside that emission.
     */
    const ServoContextMenuItem *menu_items;
    gsize                       menu_item_count;
    /*
     * Live touch points: GdkEventSequence* -> touch id. GTK identifies a finger
     * by an opaque sequence pointer that is only valid while the touch lasts,
     * while Servo wants a small integer that is stable for the whole gesture
     * and distinct between fingers, so the mapping is kept here.
     */
    GHashTable *touch_sequences;
    gint        next_touch_id;
    /*
     * The input method the widget feeds keys through, so dead keys, compose
     * sequences and CJK conversion work. Owned by the widget.
     */
    GtkIMContext *im_context;
    /*
     * Script-message channels the embedder has registered, as a set of names.
     * A console message carrying the bridge marker is only turned into a script
     * message when its channel is in here, so a page cannot invent channels.
     */
    GHashTable   *script_message_handlers;
    /*
     * A document and user scripts handed over before the widget was allocated,
     * replayed once the Servo webview exists. An application sets these up
     * immediately after construction, which is well before first allocation.
     */
    gchar        *pending_html;
    GPtrArray    *pending_user_scripts;
    /*
     * Whether a composition is open. Only a commit that arrives during one is
     * forwarded as text: outside a composition the key event already carries
     * the character, and sending both would insert it twice.
     */
    gboolean      composing;
};

/* ------------------------------------------------------------------ *
 * Closures shared by the built-in dialogs
 *
 * The windows differ between the toolkits but what they carry does not, so the
 * bookkeeping that answers each request exactly once is written once.
 * ------------------------------------------------------------------ */

typedef struct {
    ServoGtkWebView *web_view;    /* holds a reference */
    GtkWidget       *entry;       /* unowned; NULL unless this is a prompt */
    guint64          request_id;
    gboolean         responded;
} ScriptDialogClosure;

typedef struct {
    ServoGtkWebView *web_view;    /* holds a reference */
    GtkWidget       *username;    /* unowned */
    GtkWidget       *password;    /* unowned */
    guint64          request_id;
    gboolean         responded;
} AuthClosure;

typedef struct {
    ServoGtkWebView *web_view;    /* holds a reference */
    guint64          request_id;
    gboolean         responded;
} ContextMenuClosure;

typedef struct {
    ServoGtkWebView *web_view;    /* holds a reference */
    guint64          request_id;
} FileChooserClosure;

/* Attached to each built-in window under these keys. */
#define SERVO_GTK_SCRIPT_DIALOG_DATA "servo-gtk-script-dialog"
#define SERVO_GTK_AUTH_DATA          "servo-gtk-auth"
#define SERVO_GTK_CONTEXT_MENU_DATA  "servo-gtk-context-menu"
#define SERVO_GTK_ITEM_INDEX_DATA    "servo-gtk-item-index"

/* ------------------------------------------------------------------ *
 * Implemented in servo-gtk-view-common.c, used by the per-toolkit source
 * ------------------------------------------------------------------ */

G_GNUC_INTERNAL void servo_gtk_web_view_sync_surface(ServoGtkWebView *self,
                                                     int              width,
                                                     int              height);

G_GNUC_INTERNAL gboolean servo_gtk_web_view_key(ServoGtkWebView *self,
                                                guint            keyval,
                                                GdkModifierType  state,
                                                gboolean         pressed);

G_GNUC_INTERNAL double servo_gtk_web_view_to_device(ServoGtkWebView *self,
                                                    double           value);

/*
 * Forward one touch point to Servo, resolving @sequence to a touch id that is
 * stable for the gesture. Coordinates are in widget (logical) units.
 */
/*
 * Create the input method and connect its signals. Called from the toolkit half
 * of init, which is also where the key path that feeds it is set up.
 */
G_GNUC_INTERNAL void servo_gtk_web_view_init_input_method(ServoGtkWebView *self);

G_GNUC_INTERNAL void servo_gtk_web_view_touch(ServoGtkWebView   *self,
                                              ServoTouchPhase    phase,
                                              GdkEventSequence  *sequence,
                                              gdouble            x,
                                              gdouble            y);

G_GNUC_INTERNAL void script_dialog_closure_free(gpointer data);
G_GNUC_INTERNAL void script_dialog_respond(GtkWidget *window, gboolean accepted);

G_GNUC_INTERNAL void auth_closure_free(gpointer data);
G_GNUC_INTERNAL void auth_respond(GtkWidget *window, gboolean accepted);

G_GNUC_INTERNAL void context_menu_closure_free(gpointer data);
G_GNUC_INTERNAL void context_menu_respond(GtkWidget *menu, gsize item_index);

G_GNUC_INTERNAL GtkFileFilter *servo_gtk_web_view_build_file_filter(
    const gchar *const *filter_patterns);

/*
 * ::destroy handlers for the built-in windows. Connected by the per-toolkit
 * source; they cancel the request if the window goes away unanswered.
 */
G_GNUC_INTERNAL void on_script_dialog_destroy(GtkWidget *window, gpointer user_data);
G_GNUC_INTERNAL void on_auth_destroy(GtkWidget *window, gpointer user_data);

/* G_DEFINE_TYPE lives in the common source, so the parent class comes from it. */
G_GNUC_INTERNAL GtkWidgetClass *servo_gtk_web_view_parent_widget_class(void);

/*
 * Register a built-in dialog window so that a withdrawal by Servo, or the
 * widget being disposed, takes it down.
 */
G_GNUC_INTERNAL void servo_gtk_web_view_track_dialog(ServoGtkWebView *self,
                                                     guint64          request_id,
                                                     GtkWidget       *window);

/* ------------------------------------------------------------------ *
 * Implemented per toolkit, used by servo-gtk-view-common.c
 * ------------------------------------------------------------------ */

/* Servo asked the embedder to change the pointer cursor. */
G_GNUC_INTERNAL void servo_gtk_web_view_on_cursor_changed(const char *name,
                                                          gpointer    user_data);

/* Take a dialog window down; its ::destroy cancels it if still unanswered. */
G_GNUC_INTERNAL void servo_gtk_web_view_destroy_dialog(GtkWidget *window);

/* Class closures for the request signals. */
G_GNUC_INTERNAL gboolean servo_gtk_web_view_default_script_dialog(
    ServoGtkWebView          *self,
    ServoGtkScriptDialogType  dialog_type,
    const gchar              *message,
    const gchar              *default_value,
    guint64                   request_id);

G_GNUC_INTERNAL gboolean servo_gtk_web_view_default_run_file_chooser(
    ServoGtkWebView    *self,
    const gchar *const *filter_patterns,
    gboolean            allow_multiple,
    guint64             request_id);

G_GNUC_INTERNAL gboolean servo_gtk_web_view_default_authenticate(ServoGtkWebView *self,
                                                                 const gchar     *uri,
                                                                 gboolean         for_proxy,
                                                                 guint64          request_id);

G_GNUC_INTERNAL gboolean servo_gtk_web_view_default_context_menu(
    ServoGtkWebView    *self,
    gint                x,
    gint                y,
    const gchar *const *labels,
    guint64             request_id);

/*
 * Toolkit halves of class_init and init: drawing and input are wired up
 * entirely differently between GTK3's widget vfuncs and GTK4's draw func and
 * event controllers.
 */
G_GNUC_INTERNAL void servo_gtk_web_view_class_init_toolkit(ServoGtkWebViewClass *klass);
G_GNUC_INTERNAL void servo_gtk_web_view_init_toolkit(ServoGtkWebView *self);

G_END_DECLS

#endif /* SERVO_GTK_VIEW_PRIVATE_H */

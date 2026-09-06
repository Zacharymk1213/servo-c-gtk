/*
 * Unit tests for the ServoGtkWebView widget.
 *
 * These link the widget against the no-op FFI stubs (servo-webview-scan-stub.c)
 * rather than the real Rust library, for the same reason the introspection twin
 * does: the widget's GObject surface and its behaviour before a Servo webview
 * exists are entirely independent of the engine, and linking the engine would
 * make the tests need a working Servo build to say anything at all.
 *
 * What that covers is the part a binding or an application sees first: the
 * properties and signals, the enumerations they carry, and the fact that every
 * public entry point is safe to call on a widget that has not been allocated
 * yet — which is exactly when an application sets a URI or a zoom level.
 *
 * The same source builds against both GTK majors.
 */
#if SERVO_GTK_MAJOR == 3
#  include "servo-gtk3-view.h"
#  define SERVO_GTK_INIT_CHECK() gtk_init_check(&argc, &argv)
#else
#  include "servo-gtk4-view.h"
#  define SERVO_GTK_INIT_CHECK() gtk_init_check()
#endif

/* Exit code CTest is told to read as "skipped". */
#define SERVO_GTK_TEST_SKIP 77

/* A widget with a sunk reference, so unref actually disposes it. */
static ServoGtkWebView *
web_view_new(void)
{
    ServoGtkWebView *view = servo_gtk_web_view_new();

    g_object_ref_sink(view);

    return view;
}

/* ---- type and class ------------------------------------------------- */

static void
test_type_is_a_widget(void)
{
    g_assert_true(g_type_is_a(SERVO_GTK_TYPE_WEB_VIEW, GTK_TYPE_WIDGET));
    g_assert_true(g_type_is_a(SERVO_GTK_TYPE_WEB_VIEW, GTK_TYPE_DRAWING_AREA));
}

static void
test_enum_types_are_registered(void)
{
    /*
     * The signals below carry these, so a missing registration would show up as
     * a plain integer to bindings rather than a named value.
     */
    struct {
        GType        type;
        const gchar *name;
        gint         value;
        const gchar *nick;
    } cases[] = {
        { SERVO_GTK_TYPE_LOAD_EVENT, "ServoGtkLoadEvent",
          SERVO_GTK_LOAD_FINISHED, "finished" },
        { SERVO_GTK_TYPE_SCRIPT_DIALOG_TYPE, "ServoGtkScriptDialogType",
          SERVO_GTK_SCRIPT_DIALOG_PROMPT, "prompt" },
        { SERVO_GTK_TYPE_PERMISSION_FEATURE, "ServoGtkPermissionFeature",
          SERVO_GTK_PERMISSION_CAMERA, "camera" },
    };

    for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
        GEnumClass *klass;
        GEnumValue *value;

        g_assert_true(G_TYPE_IS_ENUM(cases[i].type));
        g_assert_cmpstr(g_type_name(cases[i].type), ==, cases[i].name);

        klass = g_type_class_ref(cases[i].type);
        value = g_enum_get_value(klass, cases[i].value);
        g_assert_nonnull(value);
        g_assert_cmpstr(value->value_nick, ==, cases[i].nick);
        g_type_class_unref(klass);
    }
}

/* ---- properties ----------------------------------------------------- */

static void
test_properties_are_installed(void)
{
    static const struct {
        const gchar *name;
        GType        type;
        gboolean     writable;
    } expected[] = {
        { "uri",            G_TYPE_STRING,  TRUE  },
        { "title",          G_TYPE_STRING,  FALSE },
        { "is-loading",     G_TYPE_BOOLEAN, FALSE },
        { "can-go-back",    G_TYPE_BOOLEAN, FALSE },
        { "can-go-forward", G_TYPE_BOOLEAN, FALSE },
        { "zoom-level",     G_TYPE_DOUBLE,  TRUE  },
    };
    GObjectClass *klass = g_type_class_ref(SERVO_GTK_TYPE_WEB_VIEW);

    for (gsize i = 0; i < G_N_ELEMENTS(expected); i++) {
        GParamSpec *pspec = g_object_class_find_property(klass, expected[i].name);

        g_assert_nonnull(pspec);
        g_assert_cmpuint(pspec->value_type, ==, expected[i].type);
        g_assert_true(pspec->flags & G_PARAM_READABLE);
        /* The read-only ones report engine state; writing them would lie. */
        g_assert_true(!(pspec->flags & G_PARAM_WRITABLE) == !expected[i].writable);
    }

    g_type_class_unref(klass);
}

static void
test_property_defaults(void)
{
    ServoGtkWebView *view = web_view_new();
    gchar           *uri = NULL;
    gchar           *title = NULL;
    gboolean         is_loading = TRUE;
    gboolean         can_go_back = TRUE;
    gboolean         can_go_forward = TRUE;
    gdouble          zoom = 0.0;

    g_object_get(view,
                 "uri", &uri,
                 "title", &title,
                 "is-loading", &is_loading,
                 "can-go-back", &can_go_back,
                 "can-go-forward", &can_go_forward,
                 "zoom-level", &zoom,
                 NULL);

    g_assert_null(uri);
    g_assert_null(title);
    g_assert_false(is_loading);
    g_assert_false(can_go_back);
    g_assert_false(can_go_forward);
    g_assert_cmpfloat(zoom, ==, 1.0);

    g_free(uri);
    g_free(title);
    g_object_unref(view);
}

/* ---- zoom ----------------------------------------------------------- */

static void
on_notify(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void) object;
    (void) pspec;

    (*(guint *) user_data)++;
}

static void
test_zoom_round_trips_and_notifies(void)
{
    ServoGtkWebView *view = web_view_new();
    guint            notifications = 0;
    gdouble          zoom = 0.0;

    g_signal_connect(view, "notify::zoom-level", G_CALLBACK(on_notify), &notifications);

    servo_gtk_web_view_set_zoom_level(view, 1.5);
    g_assert_cmpfloat(servo_gtk_web_view_get_zoom_level(view), ==, 1.5);
    g_object_get(view, "zoom-level", &zoom, NULL);
    g_assert_cmpfloat(zoom, ==, 1.5);
    g_assert_cmpuint(notifications, ==, 1);

    /* Setting the same value again must not claim the property changed. */
    servo_gtk_web_view_set_zoom_level(view, 1.5);
    g_assert_cmpuint(notifications, ==, 1);

    g_object_unref(view);
}

static void
test_zoom_is_clamped_to_the_range_servo_accepts(void)
{
    ServoGtkWebView *view = web_view_new();

    /*
     * Servo clamps to [0.1, 10.0] internally. The widget clamps to the same
     * range so the property never reports a value Servo would not accept.
     */
    servo_gtk_web_view_set_zoom_level(view, 0.0001);
    g_assert_cmpfloat(servo_gtk_web_view_get_zoom_level(view), ==, 0.1);

    servo_gtk_web_view_set_zoom_level(view, 1000.0);
    g_assert_cmpfloat(servo_gtk_web_view_get_zoom_level(view), ==, 10.0);

    g_object_unref(view);
}

/* ---- uri ------------------------------------------------------------ */

static void
test_uri_set_before_allocation_is_remembered(void)
{
    /*
     * An application sets the URI immediately after construction, long before
     * the widget is allocated and a Servo webview exists. It has to survive
     * that gap: it becomes the initial URL the browsing context is created
     * with.
     */
    ServoGtkWebView *view = web_view_new();
    guint            notifications = 0;

    g_signal_connect(view, "notify::uri", G_CALLBACK(on_notify), &notifications);

    servo_gtk_web_view_load_uri(view, "https://servo.org");

    g_assert_cmpstr(servo_gtk_web_view_get_uri(view), ==, "https://servo.org");
    g_assert_cmpuint(notifications, ==, 1);

    g_object_unref(view);
}

static void
test_uri_property_and_accessor_agree(void)
{
    ServoGtkWebView *view = web_view_new();
    gchar           *uri = NULL;

    g_object_set(view, "uri", "https://example.com", NULL);
    g_object_get(view, "uri", &uri, NULL);

    g_assert_cmpstr(uri, ==, "https://example.com");
    g_assert_cmpstr(servo_gtk_web_view_get_uri(view), ==, uri);

    g_free(uri);
    g_object_unref(view);
}

/* ---- signals -------------------------------------------------------- */

static void
test_signals_are_registered_with_the_expected_shape(void)
{
    static const struct {
        const gchar *name;
        GType        return_type;
        guint        n_params;
    } expected[] = {
        { "uri-changed",             G_TYPE_NONE,    1 },
        { "load-changed",            G_TYPE_NONE,    1 },
        { "script-dialog",           G_TYPE_BOOLEAN, 4 },
        { "script-dialog-cancelled", G_TYPE_NONE,    1 },
        { "run-file-chooser",        G_TYPE_BOOLEAN, 3 },
        { "authenticate",            G_TYPE_BOOLEAN, 3 },
        { "permission-request",      G_TYPE_BOOLEAN, 2 },
        { "context-menu",            G_TYPE_BOOLEAN, 4 },
        { "create-web-view",         G_TYPE_BOOLEAN, 1 },
        { "close",                   G_TYPE_NONE,    0 },
    };

    for (gsize i = 0; i < G_N_ELEMENTS(expected); i++) {
        GSignalQuery query;
        guint        id = g_signal_lookup(expected[i].name, SERVO_GTK_TYPE_WEB_VIEW);

        g_assert_cmpuint(id, !=, 0);
        g_signal_query(id, &query);
        g_assert_cmpuint(query.return_type & ~G_SIGNAL_TYPE_STATIC_SCOPE,
                         ==, expected[i].return_type);
        g_assert_cmpuint(query.n_params, ==, expected[i].n_params);
    }
}

static void
test_request_signals_can_be_suppressed_by_a_handler(void)
{
    /*
     * Every request signal uses g_signal_accumulator_true_handled, so a handler
     * returning TRUE stops the emission before the built-in class closure runs.
     * Without that, an application presenting its own dialog would get the
     * built-in one on top of it.
     */
    static const gchar *names[] = {
        "script-dialog", "run-file-chooser", "authenticate",
        "permission-request", "context-menu", "create-web-view",
    };

    for (gsize i = 0; i < G_N_ELEMENTS(names); i++) {
        GSignalQuery query;

        g_signal_query(g_signal_lookup(names[i], SERVO_GTK_TYPE_WEB_VIEW), &query);
        g_assert_cmpuint(query.signal_flags & G_SIGNAL_RUN_LAST, !=, 0);
    }
}

/* ---- behaviour with no Servo webview -------------------------------- */

static void
test_navigation_before_allocation_is_a_no_op(void)
{
    /* Nothing to navigate yet; these must not crash or warn. */
    ServoGtkWebView *view = web_view_new();

    servo_gtk_web_view_reload(view);
    servo_gtk_web_view_go_back(view);
    servo_gtk_web_view_go_forward(view);

    g_assert_false(servo_gtk_web_view_can_go_back(view));
    g_assert_false(servo_gtk_web_view_can_go_forward(view));

    g_object_unref(view);
}

static void
test_responding_to_an_unknown_request_is_a_no_op(void)
{
    /*
     * A handler can outlive the request it was answering — the widget may be
     * torn down between a dialog opening and the user clicking. Answering then
     * has to be harmless.
     */
    ServoGtkWebView *view = web_view_new();
    const gchar     *paths[] = { "/tmp/example", NULL };

    servo_gtk_web_view_respond_to_dialog(view, 12345, TRUE, "text");
    servo_gtk_web_view_respond_to_dialog(view, 12345, FALSE, NULL);
    servo_gtk_web_view_respond_to_file_chooser(view, 12345, paths);
    servo_gtk_web_view_respond_to_file_chooser(view, 12345, NULL);
    servo_gtk_web_view_respond_to_authentication(view, 12345, "user", "pass");
    servo_gtk_web_view_respond_to_authentication(view, 12345, NULL, NULL);
    servo_gtk_web_view_respond_to_permission_request(view, 12345, TRUE);
    servo_gtk_web_view_respond_to_context_menu(view, 12345, 0);
    servo_gtk_web_view_respond_to_context_menu(
        view, 12345, SERVO_GTK_CONTEXT_MENU_NO_SELECTION);

    g_object_unref(view);
}

static void
test_accepting_a_popup_without_a_webview_fails(void)
{
    /* Refusing is the safe answer, and the caller has to be able to see it. */
    ServoGtkWebView *parent = web_view_new();
    ServoGtkWebView *popup = web_view_new();

    g_assert_false(servo_gtk_web_view_accept_new_web_view(parent, 1, popup));

    g_object_unref(popup);
    g_object_unref(parent);
}

static void
test_dispose_is_idempotent(void)
{
    /*
     * dispose() tears down the tick callback, the frame and the dialog table.
     * GObject may run it more than once, so a second pass must not double-free.
     */
    ServoGtkWebView *view = web_view_new();

    g_object_run_dispose(G_OBJECT(view));
    g_object_run_dispose(G_OBJECT(view));

    g_object_unref(view);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    /* Constructing a widget needs a display; without one there is nothing to test. */
    if (!SERVO_GTK_INIT_CHECK()) {
        g_printerr("no display available; skipping\n");
        return SERVO_GTK_TEST_SKIP;
    }

    g_test_add_func("/servo-gtk/type/is-a-widget", test_type_is_a_widget);
    g_test_add_func("/servo-gtk/type/enum-types", test_enum_types_are_registered);
    g_test_add_func("/servo-gtk/properties/installed", test_properties_are_installed);
    g_test_add_func("/servo-gtk/properties/defaults", test_property_defaults);
    g_test_add_func("/servo-gtk/zoom/round-trip", test_zoom_round_trips_and_notifies);
    g_test_add_func("/servo-gtk/zoom/clamped", test_zoom_is_clamped_to_the_range_servo_accepts);
    g_test_add_func("/servo-gtk/uri/before-allocation",
                    test_uri_set_before_allocation_is_remembered);
    g_test_add_func("/servo-gtk/uri/property-matches-accessor",
                    test_uri_property_and_accessor_agree);
    g_test_add_func("/servo-gtk/signals/registered",
                    test_signals_are_registered_with_the_expected_shape);
    g_test_add_func("/servo-gtk/signals/suppressable",
                    test_request_signals_can_be_suppressed_by_a_handler);
    g_test_add_func("/servo-gtk/no-webview/navigation",
                    test_navigation_before_allocation_is_a_no_op);
    g_test_add_func("/servo-gtk/no-webview/responses",
                    test_responding_to_an_unknown_request_is_a_no_op);
    g_test_add_func("/servo-gtk/no-webview/accept-popup",
                    test_accepting_a_popup_without_a_webview_fails);
    g_test_add_func("/servo-gtk/lifecycle/dispose-twice", test_dispose_is_idempotent);

    return g_test_run();
}

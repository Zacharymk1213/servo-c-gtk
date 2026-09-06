//
// Created by mkj on 23.06.2026.
//
#include <gtk/gtk.h>

#include "servo-gtk3-view.h"

/* Navigate the web view to the URL typed in the entry (Enter pressed). */
static void
on_url_entry_activate(GtkEntry *entry, gpointer user_data)
{
    ServoGtkWebView *web_view = SERVO_GTK_WEB_VIEW(user_data);
    const gchar     *text = gtk_entry_get_text(entry);

    if (text == NULL || *text == '\0') {
        return;
    }

    /*
     * Servo drops URLs it can't parse, so a bare host like "example.com" would
     * silently do nothing. If no scheme was typed, assume https://.
     */
    gchar *scheme = g_uri_parse_scheme(text);
    if (scheme == NULL) {
        gchar *uri = g_strconcat("https://", text, NULL);
        servo_gtk_web_view_load_uri(web_view, uri);
        g_free(uri);
    } else {
        g_free(scheme);
        servo_gtk_web_view_load_uri(web_view, text);
    }
}

/* Result of an evaluate_script call: log the returned JSON or the error. */
static void
on_script_result(ServoGtkWebView *web_view,
                 const gchar     *result_json,
                 const gchar     *error,
                 gpointer         user_data)
{
    (void) web_view;
    (void) user_data;

    if (error != NULL) {
        g_printerr("Script error: %s\n", error);
    } else {
        g_print("Script result: %s\n", result_json != NULL ? result_json : "(null)");
    }
}

/*
 * A color was picked: inject a small script that sets `color` on every <h1> in
 * the page. The script returns the number of elements it touched, which is
 * delivered as JSON to on_script_result().
 */
static void
on_color_set(GtkColorButton *button, gpointer user_data)
{
    ServoGtkWebView *web_view = SERVO_GTK_WEB_VIEW(user_data);
    GdkRGBA          rgba;

    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);

    /* gdk_rgba_to_string() yields a CSS-valid "rgb(...)"/"rgba(...)" literal
     * with no single quotes, so it embeds safely in the string below. */
    gchar *color = gdk_rgba_to_string(&rgba);
    gchar *script = g_strdup_printf(
        "(function () {"
        "  var hs = document.querySelectorAll('h1');"
        "  for (var i = 0; i < hs.length; i++) { hs[i].style.color = '%s'; }"
        "  return hs.length;"
        "})();",
        color);

    servo_gtk_web_view_evaluate_script(web_view, script, on_script_result, NULL);

    g_free(script);
    g_free(color);
}

/* The web view navigated to a new URL: print it and reflect it in the entry. */
static void
on_web_view_uri_changed(ServoGtkWebView *web_view, const gchar *uri, gpointer user_data)
{
    GtkEntry *entry = GTK_ENTRY(user_data);

    (void) web_view;

    g_print("URL changed: %s\n", uri != NULL ? uri : "(null)");

    if (uri != NULL) {
        gtk_entry_set_text(entry, uri);
    }
}

/* Toolbar buttons: forward to the web view's session-history entry points. */
static void
on_back_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    servo_gtk_web_view_go_back(SERVO_GTK_WEB_VIEW(user_data));
}

static void
on_forward_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    servo_gtk_web_view_go_forward(SERVO_GTK_WEB_VIEW(user_data));
}

static void
on_reload_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    servo_gtk_web_view_reload(SERVO_GTK_WEB_VIEW(user_data));
}

/* The page title changed: mirror it in the window title. */
static void
on_web_view_title_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ServoGtkWebView *web_view = SERVO_GTK_WEB_VIEW(object);
    GtkWindow       *window = GTK_WINDOW(user_data);
    const gchar     *title = servo_gtk_web_view_get_title(web_view);

    (void) pspec;

    gtk_window_set_title(window, title != NULL ? title : "Servo GTK Demo");
}

/* Report how far the current load has got. */
static void
on_web_view_load_changed(ServoGtkWebView   *web_view,
                         ServoGtkLoadEvent  load_event,
                         gpointer           user_data)
{
    (void) web_view;
    (void) user_data;

    switch (load_event) {
    case SERVO_GTK_LOAD_STARTED:   g_print("Load started\n"); break;
    case SERVO_GTK_LOAD_COMMITTED: g_print("Load committed\n"); break;
    case SERVO_GTK_LOAD_FINISHED:  g_print("Load finished\n"); break;
    default: break;
    }
}

/* Step the page zoom by a multiplicative factor, or reset it when factor is 0. */
static void
zoom_by(ServoGtkWebView *web_view, gdouble factor)
{
    gdouble zoom = factor == 0.0
        ? 1.0
        : servo_gtk_web_view_get_zoom_level(web_view) * factor;

    servo_gtk_web_view_set_zoom_level(web_view, zoom);
    g_print("Zoom: %.0f%%\n", servo_gtk_web_view_get_zoom_level(web_view) * 100.0);
}

static void
on_zoom_out_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    zoom_by(SERVO_GTK_WEB_VIEW(user_data), 1.0 / 1.2);
}

static void
on_zoom_in_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    zoom_by(SERVO_GTK_WEB_VIEW(user_data), 1.2);
}

static void
on_zoom_reset_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    zoom_by(SERVO_GTK_WEB_VIEW(user_data), 0.0);
}

/*
 * Exercise the built-in script dialogs: prompt() blocks the page's script until
 * the dialog is answered, and the entered text comes back as the script result.
 */
static void
on_dialog_test_clicked(GtkButton *button, gpointer user_data)
{
    ServoGtkWebView *web_view = SERVO_GTK_WEB_VIEW(user_data);

    (void) button;

    servo_gtk_web_view_evaluate_script(
        web_view,
        "prompt('What is your name?', 'world');",
        on_script_result,
        NULL);
}

/* A popup closed itself: take its window down. */
static void
on_popup_close(ServoGtkWebView *web_view, gpointer user_data)
{
    (void) web_view;

    gtk_widget_destroy(GTK_WIDGET(user_data));
}

/*
 * The page called window.open(). Put the new web view in its own window and
 * accept the request from inside the handler, which is the only point at which
 * the request is still open.
 */
static gboolean
on_create_web_view(ServoGtkWebView *web_view, guint64 request_id, gpointer user_data)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *popup = GTK_WIDGET(servo_gtk_web_view_new());

    gtk_window_set_title(GTK_WINDOW(window), "Servo GTK Demo - Popup");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(user_data));
    gtk_container_add(GTK_CONTAINER(window), popup);

    if (!servo_gtk_web_view_accept_new_web_view(web_view, request_id,
                                                SERVO_GTK_WEB_VIEW(popup))) {
        gtk_widget_destroy(window);
        return FALSE;
    }

    g_signal_connect(popup, "close", G_CALLBACK(on_popup_close), window);
    gtk_widget_show_all(window);

    return TRUE;
}

static void
activate(GtkApplication *app, gpointer user_data)
{
    GtkWidget  *window;
    GtkWidget  *box;
    GtkWidget  *label;
    GtkWidget  *url_bar;
    GtkWidget  *back_button;
    GtkWidget  *forward_button;
    GtkWidget  *reload_button;
    GtkWidget  *zoom_out_button;
    GtkWidget  *zoom_in_button;
    GtkWidget  *zoom_reset_button;
    GtkWidget  *dialog_button;
    GtkWidget  *url_entry;
    GtkWidget  *color_button;
    GtkWidget  *web_view;
    const char *initial_uri = "https://servo.org";

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Servo GTK Demo");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), box);

    label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_markup(
        GTK_LABEL(label),
        "<b>Servo GTK Demo</b>\n"
        "This demo uses libservoshell and ServoGtkWebView."
    );
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 12);
    gtk_widget_set_margin_bottom(label, 12);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    web_view = GTK_WIDGET(servo_gtk_web_view_new());
    gtk_widget_set_hexpand(web_view, TRUE);
    gtk_widget_set_vexpand(web_view, TRUE);

    /* URL bar: type an address and press Enter to navigate; a color button on
     * the same row recolors every <h1> in the page. */
    url_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(url_bar, 12);
    gtk_widget_set_margin_end(url_bar, 12);
    gtk_widget_set_margin_bottom(url_bar, 12);

    back_button = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(back_button, "Go back");
    g_signal_connect(back_button, "clicked", G_CALLBACK(on_back_clicked), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), back_button, FALSE, FALSE, 0);

    forward_button = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(forward_button, "Go forward");
    g_signal_connect(forward_button, "clicked", G_CALLBACK(on_forward_clicked), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), forward_button, FALSE, FALSE, 0);

    reload_button = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(reload_button, "Reload the current page");
    g_signal_connect(reload_button, "clicked", G_CALLBACK(on_reload_clicked), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), reload_button, FALSE, FALSE, 0);

    url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url_entry), "Enter URL and press Enter");
    gtk_entry_set_text(GTK_ENTRY(url_entry), initial_uri);
    g_signal_connect(url_entry, "activate", G_CALLBACK(on_url_entry_activate), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), url_entry, TRUE, TRUE, 0);

    zoom_out_button = gtk_button_new_from_icon_name("zoom-out-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(zoom_out_button, "Zoom out");
    g_signal_connect(zoom_out_button, "clicked", G_CALLBACK(on_zoom_out_clicked), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), zoom_out_button, FALSE, FALSE, 0);

    zoom_reset_button = gtk_button_new_from_icon_name("zoom-original-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(zoom_reset_button, "Reset zoom");
    g_signal_connect(zoom_reset_button, "clicked", G_CALLBACK(on_zoom_reset_clicked), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), zoom_reset_button, FALSE, FALSE, 0);

    zoom_in_button = gtk_button_new_from_icon_name("zoom-in-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(zoom_in_button, "Zoom in");
    g_signal_connect(zoom_in_button, "clicked", G_CALLBACK(on_zoom_in_clicked), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), zoom_in_button, FALSE, FALSE, 0);

    dialog_button = gtk_button_new_from_icon_name("dialog-question-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(dialog_button, "Run a prompt() in the page");
    g_signal_connect(dialog_button, "clicked", G_CALLBACK(on_dialog_test_clicked), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), dialog_button, FALSE, FALSE, 0);

    color_button = gtk_color_button_new();
    gtk_widget_set_tooltip_text(color_button, "Set the color of all <h1> headings");
    g_signal_connect(color_button, "color-set", G_CALLBACK(on_color_set), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), color_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), url_bar, FALSE, FALSE, 0);

    /* Print and reflect URL changes reported by Servo (navigation, redirects). */
    g_signal_connect(web_view, "uri-changed", G_CALLBACK(on_web_view_uri_changed), url_entry);

    /*
     * Keep the history buttons in step with the session history, mirror the
     * page title in the window title, and trace load progress.
     */
    g_object_bind_property(web_view, "can-go-back", back_button, "sensitive",
                           G_BINDING_SYNC_CREATE);
    g_object_bind_property(web_view, "can-go-forward", forward_button, "sensitive",
                           G_BINDING_SYNC_CREATE);
    g_signal_connect(web_view, "notify::title",
                     G_CALLBACK(on_web_view_title_changed), window);
    g_signal_connect(web_view, "load-changed",
                     G_CALLBACK(on_web_view_load_changed), NULL);
    g_signal_connect(web_view, "create-web-view",
                     G_CALLBACK(on_create_web_view), window);

    gtk_box_pack_start(GTK_BOX(box), web_view, TRUE, TRUE, 0);

    servo_gtk_web_view_load_uri(SERVO_GTK_WEB_VIEW(web_view), initial_uri);

    gtk_widget_show_all(window);
}

int
main(int argc, char **argv)
{
    GtkApplication *app;
    int status;

    app = gtk_application_new(
        "org.example.ServoGtkDemo",
        G_APPLICATION_DEFAULT_FLAGS
    );

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);

    return status;
}
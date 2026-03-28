#ifndef UI_H
#define UI_H

#include <gtk/gtk.h>
#include "test.h"

typedef struct {
    /* window */
    GtkWidget *window;

    /* device section */
    GtkWidget *device_dropdown;
    GtkWidget *device_refresh_btn;
    GtkWidget *actual_size_label;

    /* capacity */
    GtkWidget *claimed_combo;   /* preset dropdown: 8/16/32/.../1TB/Custom */
    GtkWidget *claimed_spin;    /* shown only when Custom selected */
    GtkWidget *claimed_unit_combo;

    /* progress */
    GtkWidget *write_bar;
    GtkWidget *write_label;
    GtkWidget *verify_bar;
    GtkWidget *verify_label;

    /* sector map */
    GtkWidget *sector_map;

    /* result */
    GtkWidget *verdict_label;

    /* buttons */
    GtkWidget *start_btn;
    GtkWidget *cancel_btn;
    GtkWidget *save_btn;

    /* runtime */
    TestSession *session;
    GThread     *test_thread;
    char       **device_paths;   /* parallel array to dropdown items */
    int          device_count;
} AppData;

void app_activate(GApplication *app, gpointer user_data);

#endif /* UI_H */

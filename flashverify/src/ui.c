#include "ui.h"
#include "device.h"
#include "test.h"
#include "report.h"
#include "rawio.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ════════════════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ════════════════════════════════════════════════════════════════════════════ */
static void populate_device_dropdown(AppData *app);
static gpointer test_thread_func(gpointer data);

/* ════════════════════════════════════════════════════════════════════════════
 *  Idle data
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct { AppData *app; } IdleData;

/* ════════════════════════════════════════════════════════════════════════════
 *  Sector map drawing
 * ════════════════════════════════════════════════════════════════════════════ */
static void draw_sector_map(GtkDrawingArea *area, cairo_t *cr,
                            int width, int height, gpointer data) {
    (void)area;
    AppData *app = data;
    TestSession *s = app->session;
    if (!s || !s->chunks || s->chunk_count == 0) {
        cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
        cairo_paint(cr);
        return;
    }

    int cols = 40;
    int rows = (int)((s->chunk_count + (size_t)cols - 1) / (size_t)cols);
    double cw = (double)width / cols;
    double ch = (rows > 0) ? (double)height / rows : 8.0;
    if (ch < 6.0) ch = 6.0;

    for (size_t i = 0; i < s->chunk_count; i++) {
        int col = (int)(i % (size_t)cols);
        int row = (int)(i / (size_t)cols);
        double x = col * cw + 1;
        double y = row * ch + 1;

        switch (s->chunks[i].status) {
            case CHUNK_PENDING:    cairo_set_source_rgb(cr, 0.82, 0.82, 0.78); break;
            case CHUNK_WRITTEN:    cairo_set_source_rgb(cr, 0.28, 0.56, 0.76); break;
            case CHUNK_OK:         cairo_set_source_rgb(cr, 0.14, 0.63, 0.14); break;
            case CHUNK_MISMATCH:   cairo_set_source_rgb(cr, 0.80, 0.18, 0.18); break;
            case CHUNK_UNREADABLE: cairo_set_source_rgb(cr, 0.60, 0.35, 0.05); break;
        }
        cairo_rectangle(cr, x, y, cw - 2, ch - 2);
        cairo_fill(cr);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Idle callbacks (main thread)
 * ════════════════════════════════════════════════════════════════════════════ */
static gboolean idle_update_progress(gpointer data) {
    IdleData *d = data;
    AppData  *app = d->app;
    TestSession *s = app->session;
    g_free(d);
    if (!s) return G_SOURCE_REMOVE;

    size_t written = 0;
    for (size_t i = 0; i < s->chunk_count; i++)
        if (s->chunks[i].status != CHUNK_PENDING) written++;

    double wf = (s->chunk_count > 0) ? (double)written / s->chunk_count : 0.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->write_bar), wf);

    char lbl[128];
    snprintf(lbl, sizeof(lbl), "%zu / %zu chunks  —  %.1f MB/s",
             written, s->chunk_count, s->avg_write_mbps);
    gtk_label_set_text(GTK_LABEL(app->write_label), lbl);

    size_t verified = s->ok_count + s->fail_count;
    double vf = (s->chunk_count > 0) ? (double)verified / s->chunk_count : 0.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->verify_bar), vf);

    snprintf(lbl, sizeof(lbl), "%zu / %zu chunks  —  %.1f MB/s",
             verified, s->chunk_count, s->avg_read_mbps);
    gtk_label_set_text(GTK_LABEL(app->verify_label), lbl);

    gtk_widget_queue_draw(app->sector_map);
    return G_SOURCE_REMOVE;
}

static gboolean idle_test_done(gpointer data) {
    IdleData *d = data;
    AppData  *app = d->app;
    TestSession *s = app->session;
    g_free(d);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->write_bar),  1.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->verify_bar), 1.0);
    gtk_widget_queue_draw(app->sector_map);
    gtk_widget_set_visible(app->verdict_label, TRUE);

    if (s->state == STATE_CANCELLED) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "<span size='x-large' weight='bold' color='gray'>CANCELLED</span>\n"
            "<span size='small'>ผ่าน %zu / %zu ชิ้นส่วน  |  ผิดพลาด %zu  |"
            "  เขียน %.1f MB/s  |  อ่าน %.1f MB/s</span>",
            s->ok_count, s->chunk_count, s->fail_count,
            s->avg_write_mbps, s->avg_read_mbps);
        gtk_label_set_markup(GTK_LABEL(app->verdict_label), msg);
    } else if (s->state == STATE_ERROR) {
        char msg[320];
        snprintf(msg, sizeof(msg),
            "<span size='x-large' weight='bold' color='red'>ERROR\n%s</span>",
            s->error_msg);
        gtk_label_set_markup(GTK_LABEL(app->verdict_label), msg);
    } else {
        Verdict v = verdict(s);
        const char *color = (v == VERDICT_GENUINE)  ? "green"  :
                            (v == VERDICT_WARNING)   ? "orange" : "red";
        char msg[256];
        snprintf(msg, sizeof(msg),
            "<span size='xx-large' weight='bold' color='%s'>%s</span>\n"
            "<span size='small'>ผ่าน %zu / %zu ชิ้นส่วน  |  ผิดพลาด %zu  |"
            "  เขียน %.1f MB/s  |  อ่าน %.1f MB/s</span>",
            color, verdict_str(v),
            s->ok_count, s->chunk_count, s->fail_count,
            s->avg_write_mbps, s->avg_read_mbps);
        gtk_label_set_markup(GTK_LABEL(app->verdict_label), msg);
    }

    gtk_widget_set_sensitive(app->start_btn, TRUE);
    gtk_widget_set_visible(app->cancel_btn,  FALSE);
    /* show Save Report if done OR cancelled (partial data still useful) */
    gtk_widget_set_visible(app->save_btn,
        s->state == STATE_DONE || s->state == STATE_CANCELLED);
    return G_SOURCE_REMOVE;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Progress callback (worker thread → idle)
 * ════════════════════════════════════════════════════════════════════════════ */
static void on_progress(TestSession *s, void *user_data) {
    (void)s;
    IdleData *d = g_new(IdleData, 1);
    d->app = (AppData *)user_data;
    g_idle_add(idle_update_progress, d);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Worker thread
 * ════════════════════════════════════════════════════════════════════════════ */
static gpointer test_thread_func(gpointer data) {
    AppData     *app = data;
    TestSession *s   = app->session;

    RawDevice dev = rawio_open_rw(s->device_path);
    if (!rawio_valid(dev)) {
        snprintf(s->error_msg, sizeof(s->error_msg),
                 "เปิดอุปกรณ์เพื่อเขียนไม่ได้ (กรุณารันในฐานะ Administrator)");
        s->state = STATE_ERROR;
        goto done;
    }
    if (dev.lock_failed) {
        snprintf(s->error_msg, sizeof(s->error_msg),
                 "ล็อก Drive ไม่ได้ — ปิด File Explorer และโปรแกรมอื่น\n"
                 "ที่ใช้งาน Drive นี้อยู่ แล้วลองใหม่อีกครั้ง");
        rawio_close(dev);
        s->state = STATE_ERROR;
        goto done;
    }
    write_pass(s, dev);
    rawio_close(dev);

    if (s->state == STATE_CANCELLED || s->state == STATE_ERROR) goto done;

    dev = rawio_open_ro(s->device_path);
    if (!rawio_valid(dev)) {
        snprintf(s->error_msg, sizeof(s->error_msg),
                 "เปิดอุปกรณ์เพื่ออ่านไม่ได้");
        s->state = STATE_ERROR;
        goto done;
    }
    verify_pass(s, dev);
    rawio_close(dev);

    if (s->state != STATE_CANCELLED)
        s->state = STATE_DONE;

done: {
        IdleData *d = g_new(IdleData, 1);
        d->app = app;
        g_idle_add(idle_test_done, d);
    }
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Warning dialog response — start test
 * ════════════════════════════════════════════════════════════════════════════ */
static void on_warn_response(GtkDialog *dlg, int resp, gpointer user_data) {
    AppData *app = user_data;
    guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dlg), "dev-idx"));
    gtk_window_destroy(GTK_WINDOW(dlg));
    if (resp != GTK_RESPONSE_OK) return;

    /* free old session */
    if (app->session) { session_free(app->session); g_free(app->session); }
    app->session = g_new0(TestSession, 1);
    TestSession *s = app->session;

    strncpy(s->device_path, app->device_paths[idx], sizeof(s->device_path) - 1);

    /* claimed size — read from preset combo or custom spin */
    {
        static const double presets_gb[] = { 8,16,32,64,128,256,512,1,0 }; /* 0=custom,last=1TB */
        static const int    is_tb[]      = { 0, 0, 0, 0,  0,  0,  0,0,1 };
        int cidx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->claimed_combo));
        if (cidx < 0) cidx = 3; /* default 64 GB */
        double claimed_val;
        double multiplier;
        if (cidx == 7) {
            /* Custom */
            claimed_val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->claimed_spin));
            int unit_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->claimed_unit_combo));
            multiplier = (unit_idx == 1)
                         ? (1000.0*1000.0*1000.0*1000.0)
                         : (1000.0*1000.0*1000.0);
        } else {
            claimed_val = presets_gb[cidx];
            multiplier  = is_tb[cidx]
                          ? (1000.0*1000.0*1000.0*1000.0)
                          : (1000.0*1000.0*1000.0);
        }
        s->claimed_bytes = (uint64_t)(claimed_val * multiplier);
    }

    /* detect actual size */
    if (detect_device(s->device_path, &s->actual_bytes) != 0) {
        GtkWidget *e = gtk_message_dialog_new(
            GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "อ่านอุปกรณ์ไม่ได้\nกรุณารัน FlashVerify ในฐานะ Administrator");
        gtk_window_present(GTK_WINDOW(e));
        g_signal_connect_swapped(e, "response", G_CALLBACK(gtk_window_destroy), e);
        g_free(app->session); app->session = NULL;
        return;
    }

    if (session_init(s) != 0) {
        g_free(app->session); app->session = NULL;
        return;
    }

    s->on_progress   = on_progress;
    s->callback_data = app;

    /* reset UI */
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->write_bar),  0.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->verify_bar), 0.0);
    gtk_label_set_text(GTK_LABEL(app->write_label),  "0 / 0 ชิ้นส่วน  —  0.0 MB/s");
    gtk_label_set_text(GTK_LABEL(app->verify_label), "0 / 0 ชิ้นส่วน  —  0.0 MB/s");
    gtk_widget_set_visible(app->verdict_label, FALSE);
    gtk_widget_set_visible(app->save_btn,      FALSE);
    gtk_widget_set_sensitive(app->start_btn,   FALSE);
    gtk_widget_set_visible(app->cancel_btn,    TRUE);
    gtk_widget_queue_draw(app->sector_map);

    app->test_thread = g_thread_new("flashverify", test_thread_func, app);
    g_thread_unref(app->test_thread);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Save report — file dialog response
 * ════════════════════════════════════════════════════════════════════════════ */
static void on_save_response(GObject *src, GAsyncResult *res, gpointer user_data) {
    AppData *app = user_data;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file) return;

    char *pdf_path = g_file_get_path(file);
    g_object_unref(file);
    if (!pdf_path) return;

    /* JSON temp file */
    char json_path[512];
    snprintf(json_path, sizeof(json_path), "%s\\fv_session.json",
             g_get_tmp_dir() ? g_get_tmp_dir() : "C:\\Temp");
    dump_session_json(app->session, json_path);

    /* locate python/ dir relative to the exe */
    char script_dir[MAX_PATH];
#ifdef _WIN32
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    /* exe_path = "...\builddir\flashverify.exe"
       python/  = "...\python"  (sibling of builddir)        */
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *last_sep = '\0';                    /* strip exe name  */
    snprintf(script_dir, sizeof(script_dir), "%s\\..\\python", exe_path);
#else
    snprintf(script_dir, sizeof(script_dir), "../python");
#endif

    int ret = generate_pdf_report(json_path, pdf_path, script_dir);
    if (ret == 0) {
#ifdef _WIN32
        char cmd[MAX_PATH + 32];
        snprintf(cmd, sizeof(cmd), "explorer /select,\"%s\"", pdf_path);
        system(cmd);
#else
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "xdg-open \"%s\"", pdf_path);
        system(cmd);
#endif
    } else {
        GtkWidget *e = gtk_message_dialog_new(
            GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Failed to generate PDF.\n\n"
            "Make sure Python and reportlab are installed:\n"
            "  pip install reportlab\n\n"
            "Script dir: %s", script_dir);
        gtk_window_present(GTK_WINDOW(e));
        g_signal_connect_swapped(e, "response", G_CALLBACK(gtk_window_destroy), e);
    }
    g_free(pdf_path);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Device dropdown
 * ════════════════════════════════════════════════════════════════════════════ */
static void populate_device_dropdown(AppData *app) {
    if (app->device_paths) {
        free_device_list(app->device_paths, app->device_count);
        app->device_paths = NULL;
        app->device_count = 0;
    }

    GtkStringList *model = gtk_string_list_new(NULL);
    int count = 0;
    char **paths = list_removable_devices(&count);
    app->device_paths = paths;
    app->device_count = count;

    if (count == 0) {
        gtk_string_list_append(model, "(ไม่พบอุปกรณ์USB ที่เชื่อมต่ออยู่  )");
    } else {
        for (int i = 0; i < count; i++) {
            uint64_t bytes = 0;
            detect_device(paths[i], &bytes);
            double gb = (double)bytes / (1000.0*1000.0*1000.0);

#ifdef _WIN32
            char letters[16] = {0};
            char label[128]  = {0};
            get_drive_letters(paths[i], letters, sizeof(letters),
                              label, sizeof(label));

            char item[256];
            if (letters[0]) {
                if (label[0])
                    snprintf(item, sizeof(item), "%s:  %s  (%.1f GB)",
                             letters, label, gb);
                else
                    snprintf(item, sizeof(item), "%s:  (%.1f GB)",
                             letters, gb);
            } else {
                /* no drive letter mounted — show raw path */
                snprintf(item, sizeof(item), "%s  (%.1f GB)",
                         paths[i], gb);
            }
#else
            char item[128];
            snprintf(item, sizeof(item), "%s  (%.1f GB)", paths[i], gb);
#endif
            gtk_string_list_append(model, item);
        }
    }

    gtk_drop_down_set_model(GTK_DROP_DOWN(app->device_dropdown), G_LIST_MODEL(model));
    g_object_unref(model);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Button callbacks
 * ════════════════════════════════════════════════════════════════════════════ */
static void on_refresh_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    populate_device_dropdown(user_data);
    gtk_label_set_text(GTK_LABEL(((AppData *)user_data)->actual_size_label), "—");
}

static void on_device_selected(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppData *app = user_data;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (app->device_count == 0 || (int)idx >= app->device_count) return;

    uint64_t bytes = 0;
    if (detect_device(app->device_paths[idx], &bytes) == 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%.2f GB  (%.2f GiB)",
                 (double)bytes / (1000.0*1000.0*1000.0),
                 (double)bytes / (1024.0*1024.0*1024.0));
        gtk_label_set_text(GTK_LABEL(app->actual_size_label), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(app->actual_size_label), "อ่านไม่ได้");
    }
}

static void on_start_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = user_data;

    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->device_dropdown));
    if (app->device_count == 0 || (int)idx >= app->device_count) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app->window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "กรุณาเลือกอุปกรณ์ก่อน");
        gtk_window_present(GTK_WINDOW(dlg));
        g_signal_connect_swapped(dlg, "response", G_CALLBACK(gtk_window_destroy), dlg);
        return;
    }

    if (is_system_drive(app->device_paths[idx])) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app->window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "ไม่อนุญาต: ดูเหมือนเป็น Drive ของระบบ\n"
            "FlashVerify ไม่ทดสอบ Drive ที่ติดตั้ง Windows");
        gtk_window_present(GTK_WINDOW(dlg));
        g_signal_connect_swapped(dlg, "response", G_CALLBACK(gtk_window_destroy), dlg);
        return;
    }

    GtkWidget *warn = gtk_message_dialog_new(GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL,
        "คำเตือน: ข้อมูลทั้งหมดใน Drive ที่เลือกจะถูกลบ\n\n"
        "กรุณาตรวจสอบให้แน่ใจว่าเลือก Drive ถูกต้อง\nดำเนินการต่อ?");

    g_object_set_data(G_OBJECT(warn), "dev-idx", GUINT_TO_POINTER(idx));
    g_signal_connect(warn, "response", G_CALLBACK(on_warn_response), app);
    gtk_window_present(GTK_WINDOW(warn));
}

static void on_claimed_combo_changed(GtkComboBox *combo, gpointer user_data) {
    AppData *app = user_data;
    int cidx = gtk_combo_box_get_active(combo);
    gboolean custom = (cidx == 7);
    gtk_widget_set_visible(app->claimed_spin,       custom);
    gtk_widget_set_visible(app->claimed_unit_combo, custom);
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = user_data;
    if (app->session) app->session->cancel_flag = 1;
    gtk_widget_set_sensitive(app->cancel_btn, FALSE);
}

static void on_save_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = user_data;
    if (!app->session) return;

    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_initial_name(dlg, "flashverify_report.pdf");
    gtk_file_dialog_save(dlg, GTK_WINDOW(app->window), NULL,
                         on_save_response, app);
    g_object_unref(dlg);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Build UI
 * ════════════════════════════════════════════════════════════════════════════ */
static GtkWidget *section_label(const char *text) {
    GtkWidget *lbl = gtk_label_new(NULL);
    char buf[128];
    snprintf(buf, sizeof(buf), "<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(lbl), buf);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    return lbl;
}

void app_activate(GApplication *gapp, gpointer user_data) {
    (void)user_data;
    AppData *app = g_new0(AppData, 1);

    app->window = gtk_application_window_new(GTK_APPLICATION(gapp));
    gtk_window_set_title(GTK_WINDOW(app->window), "Blackcat FlashVerify");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 700, 820);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_window_set_child(GTK_WINDOW(app->window), scroll);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(vbox,  16);
    gtk_widget_set_margin_end(vbox,    16);
    gtk_widget_set_margin_top(vbox,    16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), vbox);

    /* header */
    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr),
        "<span size='xx-large' weight='bold'>FlashVerify</span>\n"
        "<span size='small' color='gray'>USB Flash Drive ทดสอบ USB แท้ ปลอม ดี เสีย </span>");
    gtk_label_set_justify(GTK_LABEL(hdr), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(vbox), hdr);
    gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* device row */
    gtk_box_append(GTK_BOX(vbox), section_label("อุปกรณ์"));
    GtkWidget *dev_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->device_dropdown    = gtk_drop_down_new(NULL, NULL);
    app->device_refresh_btn = gtk_button_new_with_label("รีเฟรช");
    gtk_widget_set_hexpand(app->device_dropdown, TRUE);
    gtk_box_append(GTK_BOX(dev_row), app->device_dropdown);
    gtk_box_append(GTK_BOX(dev_row), app->device_refresh_btn);
    gtk_box_append(GTK_BOX(vbox), dev_row);

    GtkWidget *sz_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(sz_row), gtk_label_new("ขนาดจริง:"));
    app->actual_size_label = gtk_label_new("—");
    gtk_widget_set_hexpand(app->actual_size_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(app->actual_size_label), 0.0f);
    gtk_box_append(GTK_BOX(sz_row), app->actual_size_label);
    gtk_box_append(GTK_BOX(vbox), sz_row);

    gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* claimed capacity */
    gtk_box_append(GTK_BOX(vbox), section_label("ความจุที่ระบุ"));
    GtkWidget *cap_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    app->claimed_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_combo), "8 GB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_combo), "16 GB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_combo), "32 GB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_combo), "64 GB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_combo), "128 GB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_combo), "256 GB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_combo), "512 GB");
   gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_combo), "1 TB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_combo), "กำหนดเอง...");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->claimed_combo), 3); /* default 64 GB */

    app->claimed_spin       = gtk_spin_button_new_with_range(1, 9999, 1);
    app->claimed_unit_combo = gtk_combo_box_text_new();
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->claimed_spin), 64);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_unit_combo), "GB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->claimed_unit_combo), "TB");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->claimed_unit_combo), 0);
    gtk_widget_set_visible(app->claimed_spin,       FALSE);
    gtk_widget_set_visible(app->claimed_unit_combo, FALSE);

    gtk_box_append(GTK_BOX(cap_row), gtk_label_new("ความจุตามกล่อง:"));
    gtk_box_append(GTK_BOX(cap_row), app->claimed_combo);
    gtk_box_append(GTK_BOX(cap_row), app->claimed_spin);
    gtk_box_append(GTK_BOX(cap_row), app->claimed_unit_combo);
    gtk_box_append(GTK_BOX(vbox), cap_row);
    gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* write progress */
    gtk_box_append(GTK_BOX(vbox), section_label("ขั้นตอนการเขียนข้อมูล"));
    app->write_bar   = gtk_progress_bar_new();
    app->write_label = gtk_label_new("0 / 0 ชิ้นส่วน  —  0.0 MB/s");
    gtk_label_set_xalign(GTK_LABEL(app->write_label), 0.0f);
    gtk_box_append(GTK_BOX(vbox), app->write_bar);
    gtk_box_append(GTK_BOX(vbox), app->write_label);

    /* verify progress */
    gtk_box_append(GTK_BOX(vbox), section_label("ขั้นตอนการตรวจสอบข้อมูล"));
    app->verify_bar   = gtk_progress_bar_new();
    app->verify_label = gtk_label_new("0 / 0 ชิ้นส่วน  —  0.0 MB/s");
    gtk_label_set_xalign(GTK_LABEL(app->verify_label), 0.0f);
    gtk_box_append(GTK_BOX(vbox), app->verify_bar);
    gtk_box_append(GTK_BOX(vbox), app->verify_label);

    /* sector map */
    gtk_box_append(GTK_BOX(vbox), section_label("แผนที่ชิ้นส่วน"));
    app->sector_map = gtk_drawing_area_new();
    gtk_widget_set_size_request(app->sector_map, -1, 120);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->sector_map),
                                   draw_sector_map, app, NULL);
    gtk_box_append(GTK_BOX(vbox), app->sector_map);

    GtkWidget *legend = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(legend),
        "<span color='#D0D0C8'>■</span> ยังไม่ได้เขียน  "
        "<span color='#4790C2'>■</span> ส่วนที่เขียนแล้ว  "
        "<span color='#24A124'>■</span> OK  "
        "<span color='#CC2E2E'>■</span> ผิดพลาด  "
        "<span color='#995808'>■</span> อ่านไม่ได้");
    gtk_label_set_xalign(GTK_LABEL(legend), 0.0f);
    gtk_box_append(GTK_BOX(vbox), legend);
    gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* verdict */
    app->verdict_label = gtk_label_new(NULL);
    gtk_label_set_use_markup(GTK_LABEL(app->verdict_label), TRUE);
    gtk_label_set_justify(GTK_LABEL(app->verdict_label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_visible(app->verdict_label, FALSE);
    gtk_box_append(GTK_BOX(vbox), app->verdict_label);

    /* buttons */
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
    app->start_btn  = gtk_button_new_with_label("เริ่มการทดสอบ");
    app->cancel_btn = gtk_button_new_with_label("ยกเลิก");
    app->save_btn   = gtk_button_new_with_label("บันทึกรายงาน");
    gtk_widget_add_css_class(app->start_btn,  "suggested-action");
    gtk_widget_add_css_class(app->cancel_btn, "destructive-action");
    gtk_widget_set_visible(app->cancel_btn, FALSE);
    gtk_widget_set_visible(app->save_btn,   FALSE);
    gtk_box_append(GTK_BOX(btn_row), app->start_btn);
    gtk_box_append(GTK_BOX(btn_row), app->cancel_btn);
    gtk_box_append(GTK_BOX(btn_row), app->save_btn);
    gtk_box_append(GTK_BOX(vbox), btn_row);

    /* signals */
    g_signal_connect(app->claimed_combo, "changed",
                     G_CALLBACK(on_claimed_combo_changed), app);
    g_signal_connect(app->device_refresh_btn, "clicked",
                     G_CALLBACK(on_refresh_clicked), app);
    g_signal_connect(app->device_dropdown, "notify::selected",
                     G_CALLBACK(on_device_selected), app);
    g_signal_connect(app->start_btn,  "clicked", G_CALLBACK(on_start_clicked),  app);
    g_signal_connect(app->cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), app);
    g_signal_connect(app->save_btn,   "clicked", G_CALLBACK(on_save_clicked),   app);

    populate_device_dropdown(app);
    gtk_window_present(GTK_WINDOW(app->window));
}

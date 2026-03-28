#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "test.h"
#include "report.h"
#include "rawio.h"

#ifdef WITH_GTK4
#include <gtk/gtk.h>
#include "ui.h"
#endif

/* ════════════════════════════════════════════════════════════════════════════
 *  CLI mode (no GTK4, or --cli flag)
 * ════════════════════════════════════════════════════════════════════════════ */

static void cli_progress(TestSession *s, void *user_data) {
    (void)user_data;
    const char *phase = (s->state == STATE_WRITING) ? "WRITE" : "VERIFY";
    size_t done = 0;
    if (s->state == STATE_WRITING) {
        for (size_t i = 0; i < s->chunk_count; i++)
            if (s->chunks[i].status != CHUNK_PENDING) done++;
    } else {
        done = s->ok_count + s->fail_count;
    }
    double pct = (s->chunk_count > 0)
                 ? (double)done / (double)s->chunk_count * 100.0 : 0.0;
    printf("\r[%s] %zu / %zu  (%.1f%%)  fail=%zu   ",
           phase, done, s->chunk_count, pct, s->fail_count);
    fflush(stdout);
}

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s                        — launch GUI\n", prog);
    printf("  %s list                   — list removable devices\n", prog);
    printf("  %s <device> [claimed_gb]  — run CLI test\n\n", prog);
#ifdef _WIN32
    printf("  device: e.g.  \\\\.\\PhysicalDrive1\n");
#else
    printf("  device: e.g.  /dev/sdb\n");
#endif
}

/* normalize "\.PhysicalDriveN" → "\\.\PhysicalDriveN" (bash eats one backslash) */
static const char *fix_device_path(const char *path, char *buf, size_t bufsz) {
#ifdef _WIN32
    /* bash under MSYS2 converts \\.\ to \.\ — restore it */
    if (path[0] == '\\' && path[1] == '.' && path[2] == '\\') {
        snprintf(buf, bufsz, "\\\\.\\%s", path + 3);
        return buf;
    }
#endif
    return path;
}

static int cli_run(int argc, char *argv[]) {
    printf("FlashVerify 1.0.0\n\n");

    if (strcmp(argv[1], "list") == 0) {
        int count = 0;
        char **devs = list_removable_devices(&count);
        if (count == 0) {
            printf("No removable devices found.\n");
        } else {
            for (int i = 0; i < count; i++) {
                uint64_t bytes = 0;
                detect_device(devs[i], &bytes);
                printf("  [%d] %-30s  %.2f GB\n", i, devs[i],
                       (double)bytes / (1024.0 * 1024.0 * 1024.0));
            }
        }
        free_device_list(devs, count);
        return 0;
    }

    char path_buf[512];
    const char *device_path = fix_device_path(argv[1], path_buf, sizeof(path_buf));

    TestSession s;
    memset(&s, 0, sizeof(s));
    strncpy(s.device_path, device_path, sizeof(s.device_path) - 1);
    s.on_progress = cli_progress;

    printf("Detecting: %s\n", device_path);
    if (detect_device(device_path, &s.actual_bytes) != 0) {
        fprintf(stderr, "ERROR: cannot read device '%s'\n", device_path);
        return 1;
    }
    printf("Actual : %.2f GB (decimal)  =  %.2f GiB\n",
           (double)s.actual_bytes / (1000.0 * 1000.0 * 1000.0),
           (double)s.actual_bytes / (1024.0 * 1024.0 * 1024.0));

    /* claimed size — user enters marketing GB (decimal 1GB=1,000,000,000) */
    s.claimed_bytes = (argc >= 3)
        ? (uint64_t)(atof(argv[2]) * 1000.0 * 1000.0 * 1000.0)
        : s.actual_bytes;
    printf("Claimed: %.2f GB (decimal)  =  %.2f GiB\n\n",
           (double)s.claimed_bytes / (1000.0 * 1000.0 * 1000.0),
           (double)s.claimed_bytes / (1024.0 * 1024.0 * 1024.0));

    if (session_init(&s) != 0) {
        fprintf(stderr, "ERROR: session_init failed\n");
        return 1;
    }
    printf("Chunks : %zu × %.0f MB\n\n",
           s.chunk_count, (double)s.chunk_size / (1024.0 * 1024.0));

    /* write */
    printf("WRITE pass...\n");
    RawDevice dev = rawio_open_rw(device_path);
    if (!rawio_valid(dev)) {
        fprintf(stderr, "ERROR: cannot open device for writing\n");
        session_free(&s);
        return 1;
    }
    if (dev.lock_failed) {
        fprintf(stderr, "ERROR: cannot lock drive — close File Explorer\n"
                "and any program using this drive, then retry.\n");
        rawio_close(dev);
        session_free(&s);
        return 1;
    }
    write_pass(&s, dev);
    rawio_close(dev);
    printf("\n");

    if (s.state == STATE_ERROR) {
        fprintf(stderr, "ERROR: %s\n", s.error_msg);
        session_free(&s);
        return 1;
    }

    /* verify */
    printf("VERIFY pass...\n");
    dev = rawio_open_ro(device_path);
    if (!rawio_valid(dev)) {
        fprintf(stderr, "ERROR: cannot open device for reading\n");
        session_free(&s);
        return 1;
    }
    verify_pass(&s, dev);
    rawio_close(dev);
    printf("\n\n");

    s.state = STATE_DONE;
    Verdict v = verdict(&s);

    printf("══════════════════════════════════════\n");
    printf("  VERDICT : %s\n",    verdict_str(v));
    printf("  OK      : %zu / %zu\n", s.ok_count, s.chunk_count);
    printf("  Fail    : %zu\n",   s.fail_count);
    printf("  Write   : %.1f MB/s\n", s.avg_write_mbps);
    printf("  Read    : %.1f MB/s\n", s.avg_read_mbps);
    printf("══════════════════════════════════════\n\n");

#ifdef _WIN32
    const char *json = "fv_session.json";
#else
    const char *json = "/tmp/fv_session.json";
#endif
    if (dump_session_json(&s, json) == 0)
        printf("JSON: %s\n", json);

    session_free(&s);
    return (v == VERDICT_GENUINE) ? 0 : 2;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ════════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    /* CLI mode: explicit subcommand or --cli flag */
    if (argc >= 2 && (strcmp(argv[1], "list") == 0 ||
                      strcmp(argv[1], "--cli") == 0 ||
                      argv[1][0] == '/' ||
                      argv[1][0] == '\\')) {
        /* strip --cli prefix if present */
        if (strcmp(argv[1], "--cli") == 0) {
            argv++;
            argc--;
        }
        if (argc < 2) { print_usage(argv[0]); return 1; }
        return cli_run(argc, argv);
    }

#ifdef WITH_GTK4
    GtkApplication *app = gtk_application_new("th.flashverify.app",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(app_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
#else
    if (argc < 2) { print_usage(argv[0]); return 1; }
    return cli_run(argc, argv);
#endif
}

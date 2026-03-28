#include "report.h"
#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void write_escaped(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n",  f); break;
            case '\r': fputs("\\r",  f); break;
            case '\t': fputs("\\t",  f); break;
            default:   fputc(*s, f);     break;
        }
    }
    fputc('"', f);
}

int dump_session_json(TestSession *s, const char *out_path) {
    FILE *f = fopen(out_path, "w");
    if (!f) return -1;

    /* report id: FV-YYYY-MMDD-NNN */
    char report_id[32];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(report_id, sizeof(report_id), "FV-%04d-%02d%02d-001",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

    static const char *months[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    char date_str[32], time_str[16];
    snprintf(date_str, sizeof(date_str), "%02d %s %04d",
             tm->tm_mday, months[tm->tm_mon], tm->tm_year + 1900);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm);

    const char *verdict_label;
    if (s->state == STATE_CANCELLED) {
        verdict_label = "CANCELLED";
    } else {
        verdict_label = verdict_str(verdict(s));
    }

    double claimed_gb = (double)s->claimed_bytes / (1000.0 * 1000.0 * 1000.0);
    double actual_gb  = (double)s->actual_bytes  / (1000.0 * 1000.0 * 1000.0);

    fprintf(f, "{\n");
    fprintf(f, "  \"report_id\":        \"%s\",\n", report_id);
    fprintf(f, "  \"report_date\":      \"%s\",\n", date_str);
    fprintf(f, "  \"report_time\":      \"%s\",\n", time_str);
    fprintf(f, "  \"software_ver\":     \"FlashVerify 1.0.0\",\n");

    fprintf(f, "  \"device_path\":      "); write_escaped(f, s->device_path);   fprintf(f, ",\n");
    fprintf(f, "  \"device_name\":      "); write_escaped(f, s->device_name);   fprintf(f, ",\n");
    fprintf(f, "  \"device_serial\":    "); write_escaped(f, s->device_serial); fprintf(f, ",\n");
    fprintf(f, "  \"vendor_id\":        "); write_escaped(f, s->vendor_id);     fprintf(f, ",\n");

    fprintf(f, "  \"claimed_bytes\":    %llu,\n", (unsigned long long)s->claimed_bytes);
    fprintf(f, "  \"actual_bytes\":     %llu,\n", (unsigned long long)s->actual_bytes);
    fprintf(f, "  \"claimed_gb\":       %.2f,\n", claimed_gb);
    fprintf(f, "  \"actual_gb\":        %.2f,\n", actual_gb);

    fprintf(f, "  \"verdict\":          \"%s\",\n", verdict_label);
    fprintf(f, "  \"chunk_size_mb\":    %llu,\n", (unsigned long long)(s->chunk_size / (1024*1024)));
    fprintf(f, "  \"total_chunks\":     %zu,\n", s->chunk_count);
    fprintf(f, "  \"ok_chunks\":        %zu,\n", s->ok_count);
    fprintf(f, "  \"fail_chunks\":      %zu,\n", s->fail_count);

    fprintf(f, "  \"write_speed_mbps\": %.1f,\n", s->avg_write_mbps);
    fprintf(f, "  \"read_speed_mbps\":  %.1f,\n", s->avg_read_mbps);
    fprintf(f, "  \"hash_algo\":        \"SHA-256\",\n");

    fprintf(f, "  \"order_id\":         "); write_escaped(f, s->order_id);       fprintf(f, ",\n");
    fprintf(f, "  \"shop_name\":        "); write_escaped(f, s->shop_name);      fprintf(f, ",\n");
    fprintf(f, "  \"platform\":         "); write_escaped(f, s->platform);       fprintf(f, ",\n");
    fprintf(f, "  \"purchase_price\":   "); write_escaped(f, s->purchase_price); fprintf(f, ",\n");

    /* fail chunk samples — up to 10 */
    fprintf(f, "  \"fail_chunk_samples\": [\n");
    int sample_count = 0;
    for (size_t i = 0; i < s->chunk_count && sample_count < 10; i++) {
        ChunkRecord *cr = &s->chunks[i];
        if (cr->status != CHUNK_MISMATCH && cr->status != CHUNK_UNREADABLE)
            continue;

        char hex[65];
        sha256_to_hex(cr->sha256, hex);

        if (sample_count > 0) fprintf(f, ",\n");
        fprintf(f, "    { \"index\": %zu, \"offset\": %llu, \"expected\": \"%.12s...\" }",
                i, (unsigned long long)cr->offset, hex);
        sample_count++;
    }
    if (sample_count > 0) fprintf(f, "\n");
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    fclose(f);
    return 0;
}

int generate_pdf_report(const char *json_path, const char *pdf_path, const char *script_dir) {
#ifdef _WIN32
#include <windows.h>
    /* Resolve script_dir (may contain "..") to absolute path */
    char abs_script_dir[MAX_PATH];
    GetFullPathNameA(script_dir, sizeof(abs_script_dir), abs_script_dir, NULL);

    /* try venv/bin/python.exe (MSYS2 style) then venv/Scripts/python.exe (native Windows) */
    char python[MAX_PATH];
    snprintf(python, sizeof(python), "%s\\venv\\bin\\python.exe", abs_script_dir);
    if (GetFileAttributesA(python) == INVALID_FILE_ATTRIBUTES) {
        snprintf(python, sizeof(python), "%s\\venv\\Scripts\\python.exe", abs_script_dir);
    }
    if (GetFileAttributesA(python) == INVALID_FILE_ATTRIBUTES) {
        /* fallback: MSYS2 UCRT64 system python */
        snprintf(python, sizeof(python), "C:\\msys64\\ucrt64\\bin\\python.exe");
    }

    char script[MAX_PATH];
    snprintf(script, sizeof(script), "%s\\generate_report.py", abs_script_dir);

    /* Build command line: "python.exe" "script.py" --input "..." --output "..." */
    char cmdline[4096];
    snprintf(cmdline, sizeof(cmdline),
             "\"%s\" \"%s\" --input \"%s\" --output \"%s\"",
             python, script, json_path, pdf_path);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    fprintf(stderr, "[report] python: %s\n", python);
    fprintf(stderr, "[report] cmdline: %s\n", cmdline);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[report] CreateProcess failed: %lu\n", GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
#else
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "python3 \"%s/generate_report.py\" --input \"%s\" --output \"%s\"",
             script_dir, json_path, pdf_path);
    return system(cmd);
#endif
}

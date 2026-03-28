#ifndef REPORT_H
#define REPORT_H

#include "test.h"

int dump_session_json(TestSession *s, const char *out_path);
int generate_pdf_report(const char *json_path, const char *pdf_path, const char *script_dir);

#endif /* REPORT_H */

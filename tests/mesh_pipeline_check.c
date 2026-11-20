// Inject cache-key results into the reporting guard used by mesh_prepared_check (vk_exec_draw.c).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int lines;
static char last[256];
static void rt_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(last, sizeof last, fmt, args);
    va_end(args);
    lines++;
}
#include "../runtime/mesh_pipeline_check.h"

int main(void) {
    mesh_pipeline_check_start(1);
    if (lines != 1 || !strstr(last, "check: on")) return 5;
    mesh_pipeline_check_start(0);
    if (lines != 2 || !strstr(last, "check: off")) return 6;
    lines = 0;
    int original = 7;
    const int *info = &original;
    mesh_pipeline_check_report(1);
    if (lines || info != &original || *info != 7) return 1;
    mesh_pipeline_check_report(0);
    if (lines != 1 || !strstr(last, "pipeline differs from draw")) return 2;
    mesh_pipeline_check_report(-1);
    if (lines != 2 || !strstr(last, "entry not found")) return 3;
    for (int i = 0; i < 10; i++) mesh_pipeline_check_report(0);
    if (lines != 8 || info != &original || *info != 7) return 4;
    puts("mesh pipeline check: passed");
    return 0;
}

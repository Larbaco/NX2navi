// Report a mesh draw's cache-key check without changing the draw. -1 means the cache entry was not found.
#pragma once
#include <stdint.h>

// The start line: the check runs on the mesh emulation's draws only (vk_exec_draw.c mesh_prepared_check), and a run
// without a report line says nothing otherwise.
static inline void mesh_pipeline_check_start(int emulated) {
    rt_log("vk_exec: mesh emulation pipeline key check: %s", emulated ? "on (the first 8 mesh draws whose pipeline "
           "differs from the draw, or has no cache entry, are logged)" : "off (mesh draws are not emulated)");
}

static inline void mesh_pipeline_check_report(int match) {
    static uint32_t logged;
    if (match <= 0 && __atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 8)
        rt_log("vk_exec: mesh emulation pipeline key check: %s", match ? "entry not found" : "pipeline differs from draw");
}

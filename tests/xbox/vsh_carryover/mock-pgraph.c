#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "mock-pgraph.h"

void pgraph_check_within_begin_end_block(PGRAPHState *pg)
{
    (void)pg;
}

/* vertex.c's NV2A workload counter hook (gpu_counters.c is not linked). */
void nv2a_gc_immediate_vertex(PGRAPHState *pg)
{
    (void)pg;
}

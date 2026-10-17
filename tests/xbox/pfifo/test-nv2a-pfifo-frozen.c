/*
 * Unit tests for the NV2A pushbuffer words frozen at a ring restart
 * (hw/xbox/nv2a/pfifo_frozen.h): what the threaded pusher reads after the
 * guest rewrote the ring head before DMA_GET arrived there, as pbkit's
 * pb_reset() does once its 100-tick wait for DMA_GET times out.
 */

#include "qemu/osdep.h"
#include <glib.h>
#include "hw/xbox/nv2a/pfifo_frozen.h"

#define RING_BYTES 0x10000
#define JUMP_TO(off) ((uint32_t)(off) | 1)

static uint32_t ring[RING_BYTES / 4];

static void fill(uint32_t from, uint32_t to, uint32_t seed)
{
    for (uint32_t a = from; a < to; a += 4) {
        ring[a / 4] = seed ^ (a * 2654435761u);
    }
}

/*
 * The pusher's read of the word at get: RAM, then the frozen copy if there
 * is one. Returns the word, and the words it may take from there.
 */
static uint32_t pusher_read(PFIFOFrozen *f, uint32_t get, uint32_t put,
                            size_t *avail)
{
    uint32_t word = ring[get / 4];
    *avail = (uint32_t)(put - get) / 4;
    uint32_t *frozen = pfifo_frozen_lookup(f, get, avail);
    return frozen ? *frozen : word;
}

/*
 * pbkit: a frame at [0x100, 0x800) of which the pusher read up to 0x700, a
 * jump back to the head at the old PUT, PUT moved to the head, then the
 * next frame written over the whole range before the pusher moves.
 */
static void test_restart_survives_overwrite(void)
{
    PFIFOFrozen f = { 0 };
    const uint32_t get = 0x700, old_put = 0x800;

    fill(0, RING_BYTES, 0x1234);
    ring[old_put / 4] = JUMP_TO(0);
    uint32_t want[(old_put + 4 - get) / 4];
    memcpy(want, &ring[get / 4], sizeof(want));

    pfifo_frozen_capture(&f, (uint8_t *)ring, RING_BYTES - 1, get, old_put, 0);
    g_assert_true(qatomic_read(&f.valid));
    g_assert_cmphex(f.start, ==, get);
    g_assert_cmphex(f.end, ==, old_put + 4);

    fill(0, 0x1000, 0xdead);

    uint32_t new_put = 0x1000;
    for (uint32_t a = get; a <= old_put; a += 4) {
        size_t avail;
        g_assert_cmphex(pusher_read(&f, a, new_put, &avail), ==,
                        want[(a - get) / 4]);
        g_assert_cmpuint(avail, ==, (old_put + 4 - a) / 4);
    }

    /* The pusher runs the jump: the head holds the new frame. */
    pfifo_frozen_drop(&f);
    size_t avail;
    g_assert_cmphex(pusher_read(&f, 0, new_put, &avail), ==, ring[0]);
    g_assert_cmpuint(avail, ==, new_put / 4);
}

/*
 * The jump at the old PUT targets a word inside the frozen range: after the
 * jump the words there must come from RAM.
 */
static void test_jump_into_frozen_range_reads_ram(void)
{
    PFIFOFrozen f = { 0 };

    fill(0, RING_BYTES, 0x55);
    ring[0x40 / 4] = JUMP_TO(0x20);
    pfifo_frozen_capture(&f, (uint8_t *)ring, RING_BYTES - 1, 0x10, 0x40, 0);
    g_assert_true(qatomic_read(&f.valid));
    fill(0, 0x80, 0x66);

    size_t avail;
    g_assert_cmphex(pusher_read(&f, 0x40, 0x80, &avail), ==, JUMP_TO(0x20));
    pfifo_frozen_drop(&f);
    g_assert_cmphex(pusher_read(&f, 0x20, 0x80, &avail), ==, ring[0x20 / 4]);
}

/*
 * No restart, nothing unread behind the old PUT, or a PUT moved back but
 * not behind GET (a reset writing GET then PUT, a retraction): the pusher
 * won't run [get, old_put] to a jump, so nothing is frozen and the words
 * the guest writes there next come from RAM.
 */
static void test_no_capture(void)
{
    PFIFOFrozen f = { 0 };

    fill(0, RING_BYTES, 0x77);
    pfifo_frozen_capture(&f, (uint8_t *)ring, RING_BYTES - 1, 0x100, 0x200,
                         0x300);
    g_assert_false(qatomic_read(&f.valid));
    pfifo_frozen_capture(&f, (uint8_t *)ring, RING_BYTES - 1, 0x300, 0x200,
                         0);
    g_assert_false(qatomic_read(&f.valid));
    pfifo_frozen_capture(&f, (uint8_t *)ring, 0x1ff, 0x100, 0x200, 0);
    g_assert_false(qatomic_read(&f.valid));
    pfifo_frozen_capture(&f, (uint8_t *)ring, RING_BYTES - 1, 0x100, 0x200,
                         0x180);
    g_assert_false(qatomic_read(&f.valid));
    pfifo_frozen_capture(&f, (uint8_t *)ring, RING_BYTES - 1, 0x100, 0x200,
                         0x100);
    g_assert_false(qatomic_read(&f.valid));

    /* USER DMA_GET = 0, then DMA_PUT = 0, then new words and PUT = 0x40. */
    pfifo_frozen_capture(&f, (uint8_t *)ring, RING_BYTES - 1, 0, 0x200, 0);
    g_assert_false(qatomic_read(&f.valid));
    fill(0, 0x40, 0x88);
    size_t avail;
    g_assert_cmphex(pusher_read(&f, 0, 0x40, &avail), ==, ring[0]);
    g_assert_cmpuint(avail, ==, 0x40 / 4);
}

/*
 * A ring far behind (a D3D wrap) freezes CACHE1's worth from GET; past it
 * the pusher reads RAM again and the copy is dropped.
 */
static void test_capped(void)
{
    PFIFOFrozen f = { 0 };
    const uint32_t get = 0x1000, old_put = 0x8000;
    const uint32_t end = get + 4 * PFIFO_FROZEN_WORDS;

    fill(0, RING_BYTES, 0x99);
    uint32_t last = ring[end / 4 - 1];
    pfifo_frozen_capture(&f, (uint8_t *)ring, RING_BYTES - 1, get, old_put, 0);
    g_assert_cmphex(f.end, ==, end);
    ring[end / 4 - 1] ^= 0xffffffff;

    size_t avail;
    g_assert_cmphex(pusher_read(&f, end - 4, old_put, &avail), ==, last);
    g_assert_cmpuint(avail, ==, 1);
    g_assert_cmphex(pusher_read(&f, end, old_put, &avail), ==, ring[end / 4]);
    g_assert_false(qatomic_read(&f.valid));
}

/* A second restart while a frozen tail is pending keeps the first. */
static void test_second_restart_keeps_first(void)
{
    PFIFOFrozen f = { 0 };

    fill(0, RING_BYTES, 0xab);
    pfifo_frozen_capture(&f, (uint8_t *)ring, RING_BYTES - 1, 0x700, 0x800, 0);
    uint32_t w = f.words[0];
    pfifo_frozen_capture(&f, (uint8_t *)ring, RING_BYTES - 1, 0x10, 0x400, 0);
    g_assert_cmphex(f.start, ==, 0x700);
    g_assert_cmphex(f.words[0], ==, w);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nv2a/pfifo/frozen/restart-survives-overwrite",
                    test_restart_survives_overwrite);
    g_test_add_func("/nv2a/pfifo/frozen/jump-into-frozen-range",
                    test_jump_into_frozen_range_reads_ram);
    g_test_add_func("/nv2a/pfifo/frozen/no-capture", test_no_capture);
    g_test_add_func("/nv2a/pfifo/frozen/capped", test_capped);
    g_test_add_func("/nv2a/pfifo/frozen/second-restart",
                    test_second_restart_keeps_first);
    return g_test_run();
}

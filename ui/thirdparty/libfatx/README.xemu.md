# libfatx

FATX filesystem library from https://github.com/mborgerson/fatx (commit
fe41b2c), by Matt Borgerson, GPL v2 or later (see LICENSE.txt). `ext.c`
combines OpenBSD's basename (BSD licence, Todd C. Miller) with the GNU C
Library's memrchr and dirname (LGPL 2.1 or later); see its headers.
`scripts/gen-license.py` lists these notices (licenses/libfatx).

Local changes:
- optional `io_read` / `io_write` callbacks in `struct fatx_fs`
  (fatx.h, fatx_dev.c, fatx.c) so the library can work on xemu's hard disk
  image through QEMU's block layer instead of a host file. `ext.c`'s
  memrchr is renamed `ext_memrchr` so it can't clash with the C library's.
- `fatx_ts_to_time_t()` clears the `struct tm` before `mktime()`; its
  `tm_isdst` was uninitialized, so times could come out an hour apart from
  one call to the next.
- `fatx_read_dir()` and `fatx_dirent_to_attr()` copy at most 42 name bytes;
  a damaged entry's length byte overflowed the 43-byte name buffers.
- `fatx_open_dir()` compares whole path components ("Halo" matched
  "Halo 2"), and fails when a directory walk can't go on instead of going
  on from the wrong folder.
- `fatx_next_dir_entry()` stops a directory cluster chain that loops
  (Brent's cycle check, state in `struct fatx_dir`).
- `fatx_alloc_dir_entry()` no longer writes past the end of a full
  directory cluster (the end marker, or the new entry, landed in the next
  cluster on the disk), and frees the cluster it took if it can't attach
  it.
- `fatx_alloc_cluster()` keeps its search position per volume
  (`fatx_fs.alloc_hint`); a static shared by volumes of different sizes
  could loop forever on a full one.
- `fatx_create_dirent()` and `fatx_rename()` accept 42-character names (the
  FATX maximum; they checked `>=`).

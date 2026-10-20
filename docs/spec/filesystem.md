# Filesystem and .iwd search order

Behavior spec written from static analysis of the original single-player executable
(patch 1.3) with REA 6.7.0 + Ghidra 12.1.4.
Nothing below is copied code.

## Observed

- `.iwd` files are ordinary ZIP archives. Entries are stored (method 0) or deflated (method 8).
- The engine registers the cvars `fs_basepath`, `fs_homepath`, `fs_basegame` and `fs_game` (strings at 0x573878–0x5738a8).
- Pack discovery per directory happens in `FUN_0041ece0`:
  1. It lists `*.iwd` and keeps at most 1024 of them (more prints "Exceeded max number of iwd files").
  2. For every name starting with `localized_`, the first 10 characters are temporarily replaced with 10 spaces (placeholder bytes at 0x573b70). The list is then sorted with `qsort` and the prefix is restored. Since spaces sort before letters, **localized packs sort first**.
  3. Packs are added in sorted order, each one in front of the search list, so **the last pack in sort order is searched first**.
  4. A localized pack must be named `localized_<language>_…`, where `<language>` is one of 15 known language names (table at 0x5a4ba0, starting with `english`). Otherwise a warning is printed and the pack is skipped.
  5. When a localized pack is added, it goes after the run of non-localized packs at the head of the list.

## Inferred

- The `qsort` comparator (`LAB_0041ebc0`) was not decoded. We assume a case-insensitive name comparison.
- Loose files take priority over packs in the same directory, as in the id Tech 3 lineage. One message mentions a "File conflict (between non-iwd and iwd file)" for xmodelsurfs, but loose-file priority is not confirmed yet.
- File lookup is case-insensitive. The shipped packs mix `Sound/` and `sound/`, so the engine must treat them as the same folder.

## Resulting order for the retail Polish install

`iw_14 > iw_13 > … > iw_00 > localized_polish_iw10 > … > localized_polish_iw00`

## Implementation

`src/fs/vfs.cpp`, covered by `tests/test_vfs.cpp`.

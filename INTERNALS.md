
Small explanation of the internals of goxel code
================================================

Voxel data is stored in 16³ tiles (`tile_t`). Each tile holds a pointer to
shared `tile_data_t` (RGBA voxels + ref count). Tiles and volumes use copy-on-write
with reference counting, so copying is cheap and `tile_data_t` is duplicated only
when a tile is written.

Several tiles together form a volume (`volume_t`). Volumes also use copy-on-write
for their tile hash table (`tiles_ref`), so volume copies are basically free.
`volume->key` changes on every write and is used to invalidate caches.

An `image_t` contains several `layer_t`, each of which owns a `volume_t` plus
material, transform, and visibility attributes. The image keeps undo snapshots
via `image_history_push()` at stroke boundaries; COW on tiles keeps this
memory-efficient.

The basic function to paint or modify a volume is `volume_op()` in
`src/volume_utils.c`. It takes a `painter_t` (shape SDF, color, blend mode,
symmetry, noise) and an oriented bounding box. The lowest-level write is
`volume_set_at()` in `src/volume.c`, which triggers `volume_prepare_write()`
and `tile_prepare_write()` as needed.

Layer compositing and brush preview use `volume_merge()` (per-tile `tile_merge()`).

Rendering is deferred: `render_volume()` and related calls append items to
`renderer_t::items`. The actual OpenGL draw happens in `render_submit()`, which
iterates tiles, looks up or generates per-tile VBOs (`get_item_for_tile()`),
and issues draw calls. Vertex data is built on the CPU by
`volume_generate_vertices()` in `src/volume_to_vertices.c`.

Each frame (`src/main.c` → `loop_function`):

    poll input → goxel_iter() → goxel_render() → swap buffers

Voxel writes happen in `goxel_iter()` (before drawing). The 3D viewport is
drawn in `goxel_render_view()`, then ImGui is composited in `gui_render()`.

For a full walkthrough of the frame pipeline (brush drag, tile cache
invalidation, picking FBO, etc.), see `.cursor/docs/pipeline.md`.

Assets are stored directly in C code (`src/assets.inl`); `tools/create_assets`
generates that file. Use `assets_get()` to retrieve them.

The GUI uses Dear ImGui (`src/gui.cpp`), with custom widgets in
`src/imgui_user.inl`.

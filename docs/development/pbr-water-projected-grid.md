# PBR Water: the sea's projected grid

## Why

The bisection tessellator (pbr-water-bisection-tessellator.md) refines the engine's water meshes, but only the loaded
cells' meshes. Unified Water's far tiles are separate meshes whose edges meet the loaded cells' with T-junctions, so the
displacement had to fade out at the loaded cells' border (`LoadedGridFade`, ~140 m) and with the distance
(`DisplacementDistanceFade`, 230-470 m). That fade is the hard LOD edge: the sea goes flat a couple of cells out.

## Design

A projected grid (Johanson 2004, "Real-time water rendering: introducing the projected grid concept"): one mesh of
(columns + 1) x (rows + 1) vertices spread evenly over the screen and cast onto the sea's plane.

- **No border.** One mesh over the whole plane: no T-junctions, so no fade. The waves reach as far as the sea is seen.
- **Constant density in pixels.** Cells of `GridCellSize` px (default 8) near and far: no sub-pixel triangles at distance,
  no quad overdraw along them; the deferred surface shades each pixel once anyway.
- **Rows end where the sea does.** The rows span from the screen's bottom edge (or top, from below the surface) to where
  the plane's ring at `GridMaxDistance` (1,000,000 units) meets the screen: no vertices wasted above the horizon. Rays that
  miss the plane, or meet it farther than that, fold onto the ring.
- **Margins.** The grid reaches 15% (NDC) past the left, right and bottom edges, so waves pulled sideways or down never
  uncover the screen's edge.
- **No swimming.** The vertices slide over the sea as the camera turns, so each vertex's waves are filtered to the grid's
  spacing at that point (the cell's longer side on the plane, ProjectedGridCS): a vertex never carries a wave shorter
  than its cell, and the band-limited surface does not shimmer. The pixels carry the full detail (cascades per pixel).
- **Where the sea is.** The grid spans the whole plane, so its pixels are discarded outside the sea: a cell texture
  (`SeaLevels`, t28) holds each cell's still level from Unified Water's coverage (`WaterEnvironment::GetCellLevels`); the
  sea's level is the level most cells share. Lakes and rivers keep their own meshes (and the tessellator).

## Pipeline

- `ProjectedGrid::SetCells` (worldspace change): sea level, cells texture.
- `PBRWater::UpdateTessellation` (once a frame, after the tessellator): `ProjectedGrid::Build` lays the grid out and runs
  `ProjectedGridCS` with the PBR Water constants, minus the loaded-cell fade (Grid0 zeroed, distance fade moved to the
  grid's end). Vertices use the tessellator's `PBRWaterTess::Vertex` layout with camera-relative positions.
- `PBRWater::SetupDraw`: a main-view water draw whose mesh lies at the sea's level is culled (`Tess0.x = 2`, as the
  tessellator does); the first such draw of each pass (per technique) is marked, and `RestoreDraw` draws the grid in its
  state: the stencil pass records it (`DrawRecordedStencil`, texture coordinates recorded), the surface and point light
  passes shade the record (`DrawDeferredSurface`, mesh key `GridMeshStart`), the other passes draw it forward.
- `Water.hlsl` `GridWaterVertex` (`Tess0.x = 6`): brings the camera-relative vertex into the draw's model space (as the
  deferred surface's Resolve does) and runs the cached-vertex program; the pixel shader discards outside the sea.
- The batched far-tile stencil path skips sea tiles (the grid draws them).
- Texture coordinates (vanilla normal maps): the first sea mesh's affine map, turned into a plane over absolute xy
  (`GpuData::Proj2/3`), evaluated per vertex.

## Settings

`ProjectedGrid` (default on), `GridCellSize` (px, default 8). Feature version 0-28-0.

## Known limits / to verify in game

- Winding: drawn two-sided (the grid's screen winding flips with the side of the plane).
- One grid draw per technique per frame: a technique drawn in two batches in one frame draws the grid only in the first.
- The curl's mesh-spacing test (`ShoreCurlSpacing`) still models the tessellator's spacing, not the grid's.
- Sea cells are whole cells: a cell partly at the sea's level and partly a lake at another level is all sea or none.
- Camera within a unit of the surface: the plane is taken a unit away (never edge-on).
- Untested in game (written 2026-11-25 without compiling the DLL; shaders compiled offline before that).

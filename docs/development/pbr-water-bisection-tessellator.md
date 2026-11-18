# PBR Water: bisection tessellator (work in progress)

Replaces the compute tessellator's CPU quadtree of patches (`WaterTessellator`, `ComputeTessellation.cpp`) with
longest-edge bisection (LEB) on the GPU, after Dupuy's concurrent binary trees. Density then varies per triangle
instead of per patch. That removes the two artefacts the patch scheme cannot avoid:

- **Blocks.** A leaf takes its densest side's grid across its whole area (`leafLog2N` is the max over its sides), so
  density changes in leaf-sized steps.
- **Fans.** A leaf's coarser sides snap their border vertices (`Representative`), fanning slivers along every leaf border.

Conforming LEB has no hanging vertices, so it needs neither the snapping nor the edge trees (`Split1D`, `CollectEdge`).
Every split is a "diamond" split of two triangles sharing their longest edge.

## Status

Built and running in game (2026-11-18), and the only tessellator since: the patch tessellator (`WaterTessellator`,
`TessellationCS.hlsl`) is removed, and its mesh read-back lives on as `WaterMeshes`. Vertex shader mode 1 draws it. First run: every mesh after the first drew the first mesh's bisectors through its
own transform (doubled, exploded water), because `SV_VertexID` does not count a draw's `StartVertexLocation` (seen in
RenderDoc: vertex 0 of the second mesh's draw had `SV_VertexID` 0). The draws now start at vertex 0 and the vertex
shader adds the mesh's list offset, stored after the draw arguments (t108), for the mesh slot in b11.

Tuned the same day (`GridN` 8, capacity 16384; split by the legs, not the refinement edge, which made every triangle
twice too fine in area; settled bisectors skip the spacing test; once an iteration changes nothing the rest dispatch
zero groups, with `FixupCS` restoring the buffer parity; the loop issues last frame's iteration count plus two, read
back without waiting). Then made persistent (v2 below, without a CBT): the subdivision is kept from frame to frame, a
level of merges a frame, and AllocateCS is folded into ClassifyCS.

Same session at the bench pose against the patch tessellator: static camera, refinement 0.065 ms + vertices 0.07 ms
vs 0.10 ms in all, stencil pass +0.015 ms, total water GPU level; moving camera, refinement 0.07 ms, stencil +0.03 ms,
surface about +0.15 ms (the finer mesh behind a moving camera merges back a level a frame, under a hysteresis).

| Piece | State |
|---|---|
| `Bisection.hlsli`: bisector layout, child rules, the shared spacing rule | runs |
| `BisectionCS.hlsl`: init, classify, split (allocate + link), compact, vertices | runs |
| `BisectionTessellator` (C++ host): buffers, roots, cross-mesh twins, dispatch sequence, draws | runs |
| `WaterTessellator::TakeBaseTriangles` / `StillSource`: the meshes for the host | done |
| Water.hlsl VS mode 4 (`BisectionVertexIndex`) | runs (mesh offset from t108/b11) |
| `PBRWater` switch (`BisectionTessellation`, `DrawCachedMesh`), deferred surface on top | runs |
| Debug line: meshes, base triangles, shared root edges, CPU time | done |
| Persistent pool + merges (the actual CBT) | not started: v2 |
| Terrain culling on the GPU, measured-reach margins | not started (uses the analytic margins) |
| Geomorphing | not done: relies on the wave filter (see "Vertices, morph, popping") |

First things to check in game (Debug section and wireframe):
- the Debug line's shared root edges should be about 3 per base triangle in open water (cell rectangles share their
  sides and diagonals); far fewer means the edge matching failed and cell borders will crack;
- wireframe "only": no cracks along cell borders or diamond chains, density smooth;
- the D3D debug layer: `CopySubresourceRegion` out of the state buffer while it is bound as a UAV, and
  `DrawInstancedIndirect` vertex fetch past the engine's vertex buffer (the vertex shader ignores the inputs in mode 4;
  D3D11 returns zeros for out-of-range fetches).

## References

- J. Dupuy, "Concurrent Binary Trees (with application to longest edge bisection)", HPG 2020. MIT reference code:
  `jdupuy/libcbt`, `jdupuy/libleb`.
- A. Benyoub, J. Dupuy, "Concurrent Binary Trees for Large-Scale Game Components", HPG 2024. Pool of bisectors with
  explicit neighbour links, a CBT used as the pool's allocator, split/merge with conforming propagation, applied to
  an ocean.
- W. F. Mitchell, "Adaptive refinement for arbitrary finite-element spaces with hierarchical bases" (1991): newest
  vertex / longest-edge bisection conformity on arbitrary triangle meshes.

## v1: stateless refinement from the roots every frame

v1 rebuilds the bisection from the base triangles each frame, in a fixed number of split iterations. There is no
persistent state, so there are no merges, no hysteresis, nothing to resync on camera cuts, and nothing to reset when
cells load. Allocation is a bump counter, because nothing is ever freed within a frame.

v1 gives up incremental cost: each frame repays every level. That's about `iterations × bisectors` thread
invocations, around 2M for 64k bisectors, well under a millisecond. The CBT pays off in v2.

### Data

- **Root (base triangle).** One per triangle of every registered water mesh, after `MergeRectangle` (a cell's water is
  2 right triangles). Uploaded every frame (camera-relative positions change). Fields:
  - camera-relative world corners;
  - model-space corners;
  - UVs and colours (as `GpuBaseTriangle` today);
  - the mesh slot;
  - `uint3 neighbours`: the root across each edge, or `None`.
  - Corner order: edge 2 (corner 2 to corner 0) is the longest edge, the refinement edge. Corner 1 is the apex. Order
    stays counter-clockwise.
  - Neighbours: edge k runs corner k to corner k+1 (mod 3). They are matched across all meshes by quantized endpoints
    (`Quantize`, as the edge trees did). This is what keeps adjacent cells conforming.
- **Bisector.** A triangle of the current subdivision, 48 bytes. Fields:
  - `root`;
  - `depth`;
  - corners as barycentric weights of root corners 1 and 2 (`float2 b[3]`; dyadic, exact to depth ~23);
  - `uint3 n`, the neighbour across each edge (same edge numbering as the root);
  - `flags`: split this iteration, split requested by a neighbour, culled.
- Child B's slot is written at allocation, in its own array.

### Split rule (Bisection.hlsli)

Parent `(c0, c1, c2)`, refinement edge `c2-c0`, midpoint `m`:

| Child | Slot | Corners | `n[0]` | `n[1]` | `n[2]` (refinement edge) |
|---|---|---|---|---|---|
| A | parent's own slot | `(c1, m, c0)` | B | twin's child B | `resolve(parent.n[0])` |
| B | newly allocated | `(c2, m, c1)` | twin's child A | A | `resolve(parent.n[1])` |

Both children keep the parent's winding and have their own refinement edge on edge 2.

The twin of a diamond has corners `(d0, d1, d2)` with `d0 = c2` and `d2 = c0`. Its child A (its own slot) covers
`m-c2`; its child B covers `c0-m`.

`resolve(X)` is the neighbour after this iteration:
- X did not split: X.
- X split and its own edge to us was its edge 0: X's child A.
- Its edge 1: X's child B.
- Its edge 2 can't happen: X would be splitting with its twin across that edge, which is us.

### Iteration (BisectionCS.hlsl)

Per iteration, over all `count` bisectors, using `DispatchIndirect` sized from the counter:

1. **ClassifyCS.**
   - `want = edgeLength / GridN > spacing(centroid) || forced`, with frustum culling (current margins).
   - If wanting:
     - If the twin `n[2]` is `None`, or `bisector[twin].n[2] == self`, it's a diamond: set `SPLIT` on self and twin
       (InterlockedOr).
     - Otherwise the twin is coarser: set `FORCED` on the twin, which splits in a later iteration.
2. **Allocation (in ClassifyCS since v2; AllocateCS before).** The first of a diamond's bisectors to set `SPLIT` on the
   diamond's leader (lower index, or no twin) reserves 2 slots, or 1, with one atomic add, writes `childB` for both and
   sets `SPLIT` on the other. If the pool is full it clears the leader's flag: a diamond either splits whole or not at
   all, so conformity can't break.
3. **LinkCS.** Double-buffered: read `src`, write `dst`.
   - Split bisectors write both children (table above).
   - Unsplit bisectors copy themselves and re-point any neighbour that split (`resolve`).
   - No thread writes another bisector's entry.
4. Swap `src`/`dst`; the next iteration's `ClassifyCS` clears `SPLIT`.

Run `MaxIterations` (about 2 × the deepest bisection plus forced chains; 40 is safe). Unconverged chains only leave
some triangles coarser than asked, never a crack.

### Spacing: one rule on the GPU

`PBRWater::TessellationSpacing(positionWS)` in `Bisection.hlsli` is the world spacing the mesh should have at a point:
- the target size (`Tess0.y`, coarsened by `Draw0.w`) over the pixels per unit (`Tess0.w`) times the distance;
- twice that on open water, eased to once in the shore band;
- divided by the curl detail where breakers curl (`ShoreCurlSpacing`'s weights);
- times `TessellationGrazingScale`;
- never finer than `128 / Maximum Tessellation`.

The CPU no longer mirrors it, so the vertex filter, the curl's spacing and the split rule can't drift apart.

### Vertices, morph, popping

- Each bisector is drawn as a small uniform grid, `GridN = 8`: 45 vertices, 64 triangles. Neighbours share whole
  edges, so equal `GridN` on both sides matches point for point. There's no snapping, and no cracks as long as shared
  points come out bit-identical. Interpolate in canonical corner order, as `VertexCS` does today with
  `BaseTriangle.order`.
- **VertexCS (Bisection).** Per (listed bisector, grid vertex):
  - position, model position, UV and colour from the root by barycentrics;
  - waves by `EvaluateVertexWaves(world, TessellationSpacing(world), true)`;
  - the reach atomics as today.
- **No geomorph.** The waves are filtered to the continuous spacing at each vertex, not to the bisector's level. A
  split only adds samples of an already low-passed surface, so pops stay below a pixel. If they show, morph the
  grid's midpoints by `saturate(2 * (log2(edge / spacing) - level) + 1)` as `TessellationMorph` does.

### Drawing

- **CompactCS.** Counts bisectors per mesh slot. A one-thread pass computes offsets and writes per-mesh
  `DrawInstanced` arguments: `VertexCountPerInstance = 3·GridN²·count`, `InstanceCount = 1`,
  `StartVertexLocation = 0`, and each mesh's offset after all the arguments. `SV_VertexID` does not include the
  start vertex, so the vertex shader adds the offset itself: the mesh slot comes in a per-draw constant (b11), the
  offsets in the arguments buffer as a raw view (t108). A scatter pass writes each bisector's index into the per-mesh
  list.
- **Water.hlsl VS mode 1.** Replace `TessellatedVertices[vertexID]` with:
  - `list = vertexID / (3·GridN²)`, `t = (vertexID % (3·GridN²)) / 3`, `corner = vertexID % 3`;
  - `TriangleCorners(t, GridN)` gives the grid vertex;
  - read `Vertices[list·VG + VertexIndex(g)]`, where `VG = (GridN+1)(GridN+2)/2`.
- **RestoreDraw.** `DrawInstancedIndirect(args, slot·16)` instead of `DrawIndexed`. `Lookup` returns the mesh slot,
  and the deferred surface keys its meshes by slot (`DeferredMesh::startIndex` becomes the slot).

### Host (BisectionTessellator)

- Same interface as `WaterTessellator`: `Register` / `Lookup` / `Build` / `Draw` / `IsReady` / `BuiltFor` / `GetStats`.
  `PBRWater.cpp` can then switch with a setting.
- Mesh read-back (`GetMesh`, `MergeRectangle`) is shared with `WaterTessellator`. Move it into its own file when
  wiring in.
- Build per frame:
  - CPU: gather roots and match twins, an O(roots) hash map of quantized edges. Upload.
  - GPU: InitCS, then `MaxIterations × (Classify, Allocate, Link, Args)`, then Compact, then VertexCS.
- Getting the base triangles: `WaterTessellator`'s mesh cache (`GetMesh`, `MergeRectangle`, `MakeSource`) is private.
  Add a public `TakeBaseTriangles(std::vector<WorldTriangle>&, std::vector<const void*>& geometries)` that consumes
  `registered` as `Build` does, or move the cache into its own file.
- Root setup on the CPU:
  - rotate each triangle's corners so its longest edge is edge 2 (`s = k + 1` for longest old edge k);
  - make it counter-clockwise in xy (water draws use cull none). The twin child mapping in LinkCS assumes twins run
    their shared edge in opposite directions;
  - match twins through a hash map of sorted quantized endpoint pairs;
  - set `order` by `Less` on quantized absolute corners, as `GpuBaseTriangle::order`.
- Indirect arguments: `State` (u3) holds the dispatch arguments. Copy bytes 16-27 and 32-43 with
  `CopySubresourceRegion` into a separate `DRAWINDIRECT_ARGS` buffer before each `DispatchIndirect`, unless the debug
  layer accepts reading arguments from a buffer bound as a UAV.
- Clear `MeshCounts` and `Reach` (`ClearUnorderedAccessViewUint`) before CountCS and VertexCS. After the last
  iteration run ClassifyCS once more, so the culled flags cover the last children before CountCS and ScatterCS.
- Slot layout: u0 bisectors, u1 next (swap each iteration), u2 ChildB, u3 State, u4 MeshCounts, u5 DrawArgs, u6 List,
  u7 Vertices, u8 Reach; t0 roots; b0 `BisectionCB` (`uint4` counts, `float4` cull, camera-relative `ViewProj`).
- Capacity: 65536 bisectors (1M triangles, 983k vertices at 80 bytes).
- Budget: the pool capacity bounds the bisectors. Read back the counter asynchronously (like the reach), and coarsen
  `targetSize` (`detailScale`) when the pool fills.

## v2: persistent subdivision (implemented 2026-11-18, without a CBT)

The bisectors are kept between frames while the roots stay the same. Each frame:

- **Roots.** The meshes drawn this frame, plus those drawn within `RetainFrames` (120), sorted by geometry, so the
  roots keep their indices while the camera turns away from a mesh and back. A hash of the meshes and quantized corners
  decides: the same roots keep the subdivision; other roots (a cell loaded or dropped) start over with `InitCS` and the
  full `MaxIterations`.
- **FrameStartCS.** This frame's counters from last frame's count.
- **MergeDecideCS.** Each child A whose sibling B (across its edge 0) is a leaf of the same depth leads; of a diamond the
  child A with the lower slot. The parent's twin's children are A' (across B's edge 0) and B' (across A's edge 1), both
  leaves, or neither at the border. The diamond merges when neither parent would split with the view
  (`SplitRatio <= MergeHysteresis`, 0.8, or outside the frustum). The four children are marked `MergeA`/`MergeB`.
  Parents are rebuilt from the children: `(A.b2, A.b0, B.b0)`, the path shifted right, the depth one less (depth sits in
  the flags' upper bits, the path in the old depth field: one bit a level, 0 child A, 1 child B).
- **MergeLinkCS.** Writes each parent into its child A's slot (`n = (Merged(A.n.z), Merged(B.n.z), B.n.x)`), marks the B
  slots dead, and re-points every other link through `Merged` (a merged B's parent sits in its sibling A's slot).
- **CompactCountCS / CompactWriteCS / CompactArgsCS.** Pack the live bisectors to the front, links renumbered through
  the new slots (in `ChildB`). The free space is then at the end again, so splitting keeps its bump allocation.
- **Refinement.** As v1, starting from last frame's subdivision: one or two iterations change anything most frames. The
  allocation is folded into `ClassifyCS`: the first of a diamond's two bisectors to set the split flag on the leader
  (the lower slot) reserves both children's slots.
- **ScatterCS** clears the frame's marks (split, forced, settled, merge) for the next frame.

A CBT (Benyoub & Dupuy 2024) would replace the compaction with a free-slot bitfield; at 16k bisectors the compaction's
two passes are cheap enough.

## Validation plan

- Wireframe (`WireframeMode 2`) at the bench pose: no fans, smooth density gradients, no cracks at cell borders.
- Crack test: wireframe "overlay" against a bright background with exaggerated waves (`WaveHeight` up). Look along
  cell borders and diamond chains.
- Nsight (`nv.py`): stencil quads, VS and CS instructions and Tessellate time against `WaterTessellator`, same session.
- Stress: camera cuts (no state in v1), cell loads, an irregular mesh (`OptimisedWaterMesh`: arbitrary triangles, long
  forced chains), and pool exhaustion (shrink the pool).

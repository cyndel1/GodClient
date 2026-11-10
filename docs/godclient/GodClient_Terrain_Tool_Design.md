# In-Game Terrain Tool for the God Client — Design

**Goal:** sculpt and paint terrain from inside the God Client while flying around the world: see each change immediately, undo it, and save it to a `.trn` the client (and later the server) loads.
**Status:** Phase 0 done 2026-11-04. The unchanged God Client now builds from `E:\client-tools` (branch `godclient-terrain-tool`, uncommitted) and starts to its main window. Test copy: `E:\SWG_GodClient\swg\exe\win32_new\`. Login not yet tested.

**Phase 1 verified in-game 2026-11-04** (brush, undo/redo, height readout, Options window and clean exit all working; tested by the user). The panel opens by default on the right and is in Game → Terrain, Tools and Window menus. Launcher: `J:\SWG_Claude\God Client Terrain Editor (E).cmd`. Window → Terrain Editor: Raise / Lower / Flatten / Level brush with live preview, undo, redo, remove all.
New files in `SwgGodClient\src`: `TerrainEditData.cpp/.h` (edit engine), `TerrainEditWindow.cpp/.h` (panel). Changed: `MainFrame.cpp/.h` (dock + Window menu), `GameWidget.cpp` (mouse hooks, right-click menu off in brush mode), `SwgGodClient.vcxproj`.

Design changes found while building:
- The client builds terrain chunks on a **worker thread that reads the rules outside any lock**, and area rebuilds only work with that thread on. So a live layer is never edited: each preview update (every 250 ms while dragging) builds a whole new stroke layer and swaps it in, the same remove/add the engine uses for building flattening. Old layers are deleted only after terrain generation has settled for 3 s.
- Live updates rebuild only the newly painted part; when the stroke ends, its whole area is rebuilt once.
- No brush ring decal yet: the panel shows the ground height under the cursor instead.
- Edits sit at the top level of the terrain's rules (as run-time rules), not in a parent group.
- **For Phase 2 saving:** the engine's own run-time rules (building flattening) are in the same layer list and must be left out when writing the `.trn`.

**Flatten report (2026-11-04):** first report was "jagged and bumpy"; on retest Flatten worked. The stroke log (`terrain_edit_log.txt`) showed clean, evenly spaced stroke paths. Likely cause: Flatten takes its target height from where *each* stroke starts, so overlapping strokes on uneven ground land at different heights and leave terraces. Level (one fixed height) works for multi-stroke flattening. Possible improvement: make Flatten keep the first stroke's height for following strokes. Added a "Check last stroke" button and per-stroke logging to `terrain_edit_log.txt` for future diagnosis.

**Phase 4 plan (2026-11-05):** 4a Roughen, Terrace, Plant flora → 4b Road, River → 4c Shape fills, Water level → 4d Smooth. Planting *trees* needs editing the baked static-collidable flora map (non-legacy planets ignore collidable flora rules), so it is a separate step.

**Tree planting verified in-game 2026-11-05 (not committed):** Plant flora → `tree:` entries.
- **Trees on non-legacy planets live in the `.trn`'s baked static-collidable flora map:** one family per 16 m tile (1024 × 1024 on Tatooine), plus a baked height map. The tree spot inside a tile is seeded by the tile index (`createFlora`). The picker lists the families already used in the map (4 on Tatooine, e.g. `Oasis_Palmcollidables`).
- **A stroke** picks the empty tiles whose tree spot is inside the brush or shape, at Plant density (a fresh random pick per stroke, so strokes add up). Each tile gets the family and the ground height at its spot. The tool keeps unpacked copies of both maps and pushes them back through a new public `ProceduralTerrainAppearanceTemplate::setStaticCollidableFloraMaps`. The edit's layer is an empty marker and is not saved. Save writes the template's current maps, so planted trees reach the server's `.trn` too. Undo/Redo/Remove all restore the tiles.
- **Engine bug 1 (clientTerrain, `RTM2_rebuilding` completion):** rebuilt chunks never called `createFlora`. Deleting the old chunks cached their trees, so every terrain edit hid the baked trees in its area, and planted trees never appeared. Fix: `createFlora` for every rebuilt chunk, in a second pass after all old chunks are deleted. Deleting a larger old chunk removes trees that a smaller new chunk had already re-created.
- **Engine bug 2 (sharedTerrain `createFlora`):** a tree reused from the cache kept its old position. It now takes its height from the baked map.
- **Trees follow the ground:** after every height-changing edit, and after its Undo/Redo/Remove all, the trees in the area get the new ground height in the baked map (`TREES moved N` log line), and the area is rebuilt. This also fixes the old note that collidable flora heights go stale on edited ground.
- **Limits:** one tree per 16 m tile (no dense forests); tiles that already have a tree are kept; trees inside Clear flora areas stay hidden.
- **Save verified 2026-11-05:** 6 planted palms, raised 4 m, then saved (`tatooine.trn` 2:03 PM; backup `_terrain_backup\tatooine_20261105_140313.trn`). They were still there at the raised height after Revert and after a client restart.

**Phase 4d verified in-game 2026-11-05 (not committed):** Smooth (brush or any shape, Smooth strength %).
- **No smoothing rule exists**, and a new affector type would make the `.trn` unreadable for the server. So Smooth is baked when the drag ends:
  1. Sample the ground on the terrain's own pole grid (2 m on Tatooine; coarser above ~6000 poles).
  2. Blur it: two box passes; the width is half the brush radius, or a quarter of the shape's smaller side, between 2 poles and 32 m.
  3. Every pole that moves ≥2 cm gets a sublayer: a `BoundaryRectangle` one pole wide (no feather) plus `AffectorHeightConstant` replace to its baked height.
  - The engine prunes sublayers by extent per chunk, so a few hundred patches are cheap. Strokes add up.
- **Crash found and fixed:** `TerrainGenerator::generateChunk` sizes the fractal caches once, from the first chunk built (the client's chunk pole count). Sampling in larger 64-pole blocks wrote past them on ground with fractal layers (Roughen), and an engine `FATAL` (exception 0x4000001f) followed. Sampling now uses exactly `2 × tilesPerChunk + originOffset + upperPad` poles per block. Any other code calling `generateChunk` must keep to that size too.

**Phase 4c verified in-game 2026-11-05 (not committed):** Shape fills and Water level.
- **Shape picker** (Brush / Rectangle / Circle / Polygon): every tool except Road and River can fill a shape built when the drag ends. Rectangle = drag corner to corner; circle = drag from the centre (16–96 outline points); polygon = drag around the outline (evened to ≤120 points, closed, rejected under 4 m²). The layer uses a `BoundaryPolygon`.
- **Soft edge for shapes** = Soft edge % × half the shape's smaller side. It was first tied to the brush size, so small shapes were all edge and barely rose (user report: "rectangle and circle raise didn't work").
- **Clear flora in a shape:** a grid of clear-flora circles covering the polygon (≤1000, spacing ≥8 m, radius 0.75 × spacing). Saving blanks baked trees inside the polygon.
- **Water level:** the `BoundaryPolygon`'s local water table at the Flatten/Level/Water height (planet water shader, size 8, `TGWT_water`). With a River/Lake depth it also digs a basin (`AffectorHeightConstant` replace to height − depth, banks over the soft edge). With the Brush shape, the drag draws a polygon. If the water height is under all the ground in the shape (e.g. the box still at 0 m), it is lifted to the lowest ground + 1 m and the panel says so.
- **Engine (sharedTerrain):** public `ProceduralTerrainAppearanceTemplate::rebuildWaterTables ()`, called after run-time water is added or removed, so new rivers and lakes can be swum in (confirmed for both).
- **Panel feedback:** rejected shapes explain why (and log their drag points). Raise/Lower/Roughen/Terrace/Water fills show the ground in the middle before and after (`RESULT shape middle` log line).
- **Testing note:** one "still broken" report came from a client started outside `win32_new`. If the log has no new `STROKE BEGIN` lines, check which exe ran.

**Phase 4b verified in-game 2026-11-05 (not committed):** Road and River.
- Both are built when the stroke ends. The drawn path is resampled and rounded off; rivers use long sections (≥16 m, one rounding pass). Ground heights are then sampled along it, as SOE's Terrain Editor "bake" does. The samples are stored with the stroke so undo, redo and save rebuild it identically.
- **Bug found:** `HeightData::Segment::find` interpolates between a segment's first and last sample and walks off the end of a 1-sample segment, giving garbage heights (stretched canyon walls) or a crash. SOE's long segments never hit it. Every segment now gets ≥2 samples, and near-duplicate points are dropped.
- **Road:** `AffectorRoad` (texture = Paint texture, smoothed baked heights, clears flora) plus a sub-layer that resets the colour tint along the path.
- **River:** `AffectorRiver` carves the channel and paints banks/bed (its own water tables disabled: one flat tile per section left seams at every joint). Water is an `AffectorRibbon`: one continuous strip with heights at 30% of the depth below the baked bank. The water shader comes from the planet (Tatooine: `wter_dsrt_bright`).
- **Engine (clientTerrain):** `ClientLocalWaterManager::setAddGroup/removeGroup` (both water lists) and `ClientProceduralTerrainAppearance::addRunTimeWater/removeRunTimeWater`, so water can be added and removed while playing (the client only built water at planet load). Run-time ribbons were not yet in the template's swim/water-height list (built once at load); fixed in 4c.
- **Texture priority:** a family's priority = its index in the shader group, and higher priorities are drawn over lower ones where tiles meet. The Paint list now shows it. The log is now always written next to the exe.

**Phase 4a verified in-game 2026-11-05 (not committed):**
- **Roughen:** `AffectorHeightFractal` using one of the planet's existing fractal families (adding fractal families at run time isn't thread-safe), `TGO_add` with ScaleY = roughness, plus `AffectorHeightConstant` subtracting half of it so bumps centre on the old ground.
- **Terrace:** `AffectorHeightTerrace` (step height, flat fraction).
- **Plant flora:** `AffectorFloraStaticNonCollidableConstant` (FloraGroup families: bushes, rocks) or `AffectorFloraDynamicNearConstant` (RadialGroup families: grass), `TGO_add` with density override. Shown live thanks to the re-plant engine fix.
- **Panel:** now in a `QScrollView` with tools in two columns. The first 4a build packed too many rows into the dock, so the new controls couldn't be used. The log now records `STROKE BEGIN` and `STROKE REFUSED <reason>` lines.

**Phase 3 fully verified in-game 2026-11-05:** Paint and Clear flora visible; Undo/Redo, Save, Revert and a client restart all confirmed with paint and flora strokes (saved `tatooine.trn` holds the paint layer with its neutral tint and the four-affector clear-flora layer; the post-save stroke was correctly dropped by Revert).
- **Paint looked like it did nothing:** it did work. A radius-16 stroke at night was too small to notice. Proven with a "Check last stroke" probe that runs the generator at the stroke points, then rebuilds the on-screen chunk with the client's exact pole layout (64/64 poles painted).
- **Paint looked "mixed" / had dark areas:** the planet's colour layers (colour-ramp fractals) tint every texture. Paint strokes now also carry an `AffectorColorConstant` (replace, white = untinted), feathered with the stroke. Confirmed clean by the user.
- **Clear flora showed no change (engine bug):** after a terrain rebuild the radial flora managers (grass, bushes, small rocks) only re-snap existing plants to the new height (`markExtentDirty` → `isWithinDirtyRegion` is used only for height). A node is re-decided only when it leaves the camera circle. **Engine fix:** `ClientRadialFloraManager::markRegionReplant` (new); `RTM2_rebuilding` completion in `ClientProceduralTerrainAppearance_LevelOfDetail.cpp` calls it for the union of the rebuilt chunks, so flora there is re-planted from the new chunk data. Confirmed by the user. The tool also registers per-point "clear non-collidable flora" objects (`addClearNonCollidableFloraObject`).
- **"Check last stroke" crashed on paint/flora strokes:** `ProceduralTerrainAppearance::findStaticCollidableFlora` reads the chunk's collidable map, which client chunks don't keep. Removed from the probe.
- **Mouse freeze while holding the button:** not reproduced in later sessions; stall logging (`STALL` lines) and rebuild tracking (`REBUILD REQUEST` / `REBUILT after` / `NOT REBUILT` / `REBUILD WAITING` lines) stay in `terrain_edit_log.txt`.

**Phase 3 original build notes:** Paint and Clear flora tools.
- **Paint:** stroke layer with `AffectorShaderConstant` (family picked by name from the planet's `ShaderGroup`; the picker refills per planet). The family's own feather clamp gives crisp edges.
- **Clear flora:** stroke layer with the four flora affectors set to `removeAll` (static collidable, static non-collidable, dynamic near, dynamic far), which covers grass, bushes and legacy-mode trees. **On most planets trees and rocks come from the `.trn`'s baked static-collidable flora map** (`createFlora` reads `getStaticCollidableFloraFamily` per 16 m tile), not from terrain rules. So live, the tool also adds the engine's run-time "clear collidable flora" circles (`addClearCollidableFloraObject`, one per stroke point, opaque keys), and on save it blanks the cleared tiles in a rebuilt copy of the baked map (tree position per tile reproduced from `createFlora`'s seeded offsets; map rebuilt from an array because `setValue` can't store values below the map's minimum).
- Undo/redo/remove-all add and remove the clear circles with the stroke. "Check last stroke" now also works on paint and flora strokes (generator probe).

**Phase 2 verified in-game 2026-11-05:** Save, Save copy to folder… and Revert all confirmed working by the user.

**"Crash warning" on close was not a crash:** the engine's `SetupSharedFoundation::remove` shows an "N warnings logged" dialog at exit whenever any warning was logged; here that was one harmless data warning, "Could not find skill DEPRECATED_SKILLS in DataTable". The 2016 client does the same. Fixed for the rebuilt client with `[SharedFoundation] demoMode=true` in `exe\win32_new\user.cfg`, which only suppresses end-of-session reports. Windows also logged an "AppHangTransient" (not responding) event once: shutdown takes ~16 s. Sampling shows the main thread blocked in a Windows wait inside a DLL (graphics, audio or Qt teardown), not in God Client code. Not yet investigated further.

**Phase 2 build notes:** Save / Save copy to folder… / Revert to saved in the Terrain Editor panel.
- Every save writes **the planet's rules as on disk at the first save of the session** (a fresh `TerrainGenerator` loaded from the file and kept in memory) **+ clones of the session's current strokes**. So building-flattening run-time layers are never saved, re-saving never duplicates strokes, and Undo after a save is handled. The live terrain layers are never touched (no race with the worker thread).
- Save target: the loose file the planet came from (`TreeFile::getPathName`), else `<highest-priority loose folder>/terrain/<name>.trn`, which overrides the `.tre` copy. The first overwrite per session backs up to `<folder>/_terrain_backup/<name>_<date_time>.trn`. Writes go to `.tmp`, then replace.
- Uses the engine's own writer (`ProceduralTerrainAppearanceTemplate::write`, PTAT 0015). Fixes an engine bug on the way: `prepareWriterData` copies the non-collidable max distance into `collidableMaximumDistance`.
- The static collidable flora maps are kept as loaded (as SOE's Terrain Editor does). Collidable flora heights on edited ground may be off for server-side collision until re-baked.
- Revert = `GroundScene::reloadTerrain()` (re-reads the file from disk; the edit list restarts).

**Phase 1 fully verified 2026-11-05:** brush, sticky Flatten, and ground staying visible while editing (no scrolling needed) all confirmed in-game by the user.

**Fixes 2026-11-05 (from the second test round):**
- **Sticky Flatten:** the first Flatten stroke sets the height (shown in the Flatten/Level height box); later strokes reuse it. A right-click picks a new height and stays in Flatten; clicking Flatten again lets the next stroke pick one.
- **Edited ground vanished until zooming out:** the engine's `clearInvalidRegionList` discards all pending chunk requests and just-built chunks. The level-of-detail system only re-requests them when the view changes, which left holes. Rebuilds are now merged and requested only while terrain generation is idle, at least 400 ms apart.
- **Ground still vanished after the change above (real cause found):** when a rebuild finishes (`RTM2_rebuilding` → done in `ClientProceduralTerrainAppearance_LevelOfDetail.cpp`), the engine swaps the new chunks into the tree but never marks the level-of-detail selection dirty. Selection only re-runs when dirty or when the camera moves ≥1 m (position is rounded to whole metres), so the new chunks were never added to the world and stayed invisible until the camera moved. The old `setDirty(true)` there had been commented out. **Engine fix:** `m_levelOfDetail->setDirty (true);` when a rebuild completes (clientTerrain library; also benefits SwgClient).
- **Intermittent crash on exit (11:27 pm session, `CrashReportInformation::remove` → `delete[]`):** heap damage from earlier in the session. Likely cause: adding a layer can grow the engine's layer `ArrayList`, whose `resizeUp()` frees the old block while the terrain worker may still be walking it (and writing prune flags through the stale pointers). Live-preview swaps happened every 250 ms. Routine swaps and rebuild requests now happen only while the worker is idle (no pending requests for ≥150 ms). End of stroke, undo, redo and remove-all stay immediate (single operations, like the engine's building layers). Not yet confirmed: needs several sessions without an exit crash.

**Crash fixes after first in-game test (2026-11-04):**
- **Options window FATAL:** newer UI code expected the control `checkShowToolbarCommandCooldownTimer`, which the 2016-era UI data lacks. That lookup, plus `buttonEnterSpace` (space HUD) and `buttonIntro` (news window), is now optional (`SwgCuiOptUi.cpp`, `SwgCuiHudSpace.cpp`, `SwgCuiNews.cpp`). A scan of all ~2,000 required UI controls found no others missing (three more were already commented out in source).
- **Crash on exit (also in the original 2016 client):** `FileManifest::addStoredManifestEntry` and `addEntry` deleted entries they had just put into the manifest map. That left dangling pointers (heap corruption while playing) and a double free in `FileManifest::remove` at shutdown. Fixed in `sharedFile/FileManifest.cpp`. It came from upstream commit `4dc1ea1e5`. Verified: no regression on open/close; the logged-in exit case still needs a test.
- Shutdown takes ~20–25 s in both old and new clients (not a hang).

**Phase 1 test checklist:**
1. Log in and go to an open outdoor spot.
2. Window → Terrain Editor; tick "Terrain brush on". The status line shows the ground height under the cursor.
3. Raise: left-drag; ground rises along the stroke within about a second. Right-drag lowers.
4. Flatten: drag across a slope; it levels to the height under the first click.
5. Level: right-click to pick a height, then left-drag to level to it.
6. Undo / Redo / Remove all work; Alt + mouse still moves the camera; untick the brush and left-click selects objects again.
7. Fly away and back (chunks rebuild with the edits); change planet (the edit list empties without crashing).

**Phase 0 fixes (all in `SwgGodClient.vcxproj`, Release config):**
1. Character Set back to `MultiByte` (a local change had set it to `Unicode`).
2. Third-party library paths were 4 levels up instead of 6 (`src\game\external` → `src\external`).
3. Added `compile\win32\dpvs\Release`, and built the `dpvs` project in Release (only Debug existed).
4. `/SAFESEH:NO` (old Perforce, Qt `qtmain` and STLport libraries).
5. Added the 58 engine libraries (`archive` … `zlib`) and their `compile\win32\<lib>\Release` folders. The Release config never listed them.
6. `qtmain.lib` first and `lcdui.lib` last, because `lcdui.lib` contains its own `WinMain` that exits at once.

---

## 1. What we have (findings)

| Area | Finding | Matters because |
|---|---|---|
| God Client binary | Prebuilt 2016 `SwgGodClient_r.exe`. No terrain-editing tools, only **Game → Terrain → Reload / Purge**. | A true in-game tool means **rebuilding the God Client from source**. |
| God Client source | `E:\client-tools\src\game\client\application\SwgGodClient` (Qt 3.3.4 UI, 59 C++ files). | We add a Terrain panel here, alongside the Object Editor and Tree Browser. |
| Toolchain | VS2013 installed at `E:\Program Files (x86)\Microsoft Visual Studio 12.0`; MSBuild 12 + v120 present. 70 projects / 189 libraries already built in `E:\client-tools\src\compile\win32`; `SwgClient_d.exe` built 2026-05-05. | The engine libraries mostly build on this PC already. |
| Last God Client build | **Failed** 2026-05-14 with 5 errors in `FilesystemTree.cpp` and `RecentDirectory.cpp`: ANSI `char*` passed to Unicode Windows APIs. The project defines both `_MBCS` and `UNICODE`. | Likely fixed by setting the project's Character Set to Multi-Byte. More errors may appear after that. |
| Terrain engine | Terrain is **procedural**: a `.trn` is a stack of *layers*, each with *boundaries* (circle, rectangle, polygon, polyline + feather), *filters* and *affectors* (height constant/fractal/terrace, shader, flora, river, road, environment, passable…). | Edits are added as extra layers on top of the original rules. Non-destructive: original rules untouched. |
| Live editing hooks | `ProceduralTerrainAppearance::addLayer/removeLayer` change the running terrain. `invalidateRegion(rect)` rebuilds only the affected chunks. (The game uses this to flatten ground under player buildings.) | Instant preview of a brush stroke without reloading the planet. Undo = remove the layer. |
| Saving | `TerrainGenerator::save(Iff&)` plus `save()` on every layer, boundary and affector type. | Write `.trn` files using the engine's own code, so the format is exactly what the game reads. |
| Picking | `GodClientData::findIntersection_p(x, y, …)` returns the world point under the mouse. | Brush position comes straight from existing code. |
| Terrain files | 68 loose `.trn` in `data\sku.0\sys.client\compiled\game\terrain` (corellia, naboo, tatooine…, plus `simple.trn` and `godclient_test.trn`). | Saved files go into that loose folder, which overrides the originals. Test on `simple.trn` first. |
| Galaxies Terrain Editor | Your web editor (`index.html`, `js/trn.js`, `editor.js`) already opens and writes `.trn` (PTAT 0015) and adds edits as `editor_raise`/`editor_paint` layers. Its exports **haven't been tested in the client** yet. | Good for big offline jobs and new planets. The in-game tool handles touch-ups in place. |
| Not in our hands | The **server** (192.168.1.200) loads its own copy of each `.trn` for heights, collision and spawning. | Client edits are visual only until the `.trn` is copied to the server and it restarts. See §6. |

---

## 2. Options

### A. Native Terrain panel in a rebuilt God Client — recommended
A new dockable **Terrain** window and brush mode inside the God Client. Truly in-game: brush on the ground, see it change, undo, save.
- **Needs:** the God Client to build from source (Phase 0 gate).
- **Effort:** large. Phase 0 is the unknown; the tool itself is moderate because the engine already has the hard parts.

### B. Live link with your Galaxies Terrain Editor
Edit in the web editor, which gets an **"Export to God Client"** button. It writes the `.trn` straight into the God Client's loose terrain folder (with a backup). Then click **Game → Terrain → Reload Terrain** in the running client to see it.
- **Needs:** no rebuild. Small change to `editor.js`.
- **Limits:** not in-game; a full reload for each look; relies on the web editor's `.trn` writer, which is untested in the client.

### C. Both
A for touch-ups in place; B for big jobs and new planets. Both write ordinary `.trn` layers, so they can share files.

**Recommendation:** A, gated by Phase 0. If the God Client won't build within the time box, fall back to B, which is useful either way.

---

## 3. Option A design

### 3.1 How an edit is stored
Every brush stroke becomes **one terrain layer** inside a parent layer named **`GodClient Edits`**, added at the end of the `.trn` so it runs after the original rules:

```
GodClient Edits                 (parent layer, can be switched off to compare)
 ├─ edit_0001  Raise   BoundaryPolyline(stroke path, width = brush size, feather)  + AffectorHeightConstant(add, +2.0 m)
 ├─ edit_0002  Flatten BoundaryCircle(x, z, r, feather)                            + AffectorHeightConstant(replace, 41.5 m)
 └─ edit_0003  Paint   BoundaryPolyline(…)                                         + AffectorShaderConstant(shader family 7)
```

- A drag becomes a **polyline** boundary (the engine's road-style shape with width and soft edges), so a whole stroke is one layer, not hundreds of dabs.
- A single click becomes a **circle** boundary.
- The original planet rules are never modified. Deleting `GodClient Edits` restores the original terrain exactly.

### 3.2 Tools (v1)

| Tool | Click / drag does | Engine pieces |
|---|---|---|
| **Raise / Lower** | Add or subtract height along the stroke | AffectorHeightConstant (add) |
| **Flatten** | Set the ground to the height under the first click | AffectorHeightConstant (replace) |
| **Level to value** | Set a typed height | AffectorHeightConstant (replace) |
| **Paint** | Apply a ground shader family (picked from the planet's list) | AffectorShaderConstant |
| **Clear flora** | Remove trees, bushes and grass in the area | AffectorFloraStatic/Dynamic constant (remove) |
| **No-build / Passable** (v2) | Mark ground passable or not | AffectorPassable |
| **Water** (v2) | Add a lake at a height | boundary water flags |

Brush settings: **size** (radius, m), **feather** (soft edge, 0–1, with the engine's feather curves), **strength** (m), **height** (for Level).

### 3.3 Using it in the game window
- **Window → Terrain** opens the panel: tool buttons, brush sliders, edit list (with on/off and delete per edit), *Save*, *Save As*, *Revert*.
- A **Terrain mode** toggle on the panel (or <kbd>Ctrl</kbd>+<kbd>K</kbd>) switches the game view's left mouse button from selecting to brushing. Camera controls (<kbd>Alt</kbd> + mouse) keep working. Object tools are off while it's on.
- A ring decal under the cursor shows the brush size and feather.
- **While dragging:** a preview layer is updated and `invalidateRegion(stroke bounds)` rebuilds just that area. **On release:** the stroke is committed as `edit_NNNN`.
- **Undo/Redo** (<kbd>Ctrl</kbd>+<kbd>Z</kbd>/<kbd>Y</kbd>) remove or re-add the last edit layer. This joins the existing God Client undo history.

### 3.4 Saving
1. **Save** writes the whole generator with `TerrainGenerator::save()` to `data\sku.0\sys.client\compiled\game\terrain\<planet>.trn`, the loose folder that overrides the shipped file.
2. Before the first save of a session, the existing file is copied to `terrain\_backup\<planet>_<date>.trn`.
3. **Save As** picks a new name (e.g. `tatooine_test.trn`) for experiments.
4. **Export for server** copies the saved `.trn` (and any bitmaps) to a folder you choose, ready to drop into the server's data.
5. **Revert** reloads the file from disk (`GroundScene::reloadTerrain()`).

### 3.5 Code layout (new files in `SwgGodClient\src`)

| File | Job |
|---|---|
| `TerrainEditWindow.cpp/.h` + `BaseTerrainEditWindow.ui` | The dockable panel (Qt 3 Designer form, like `BaseObjectEditor.ui`) |
| `TerrainBrush.cpp/.h` | Brush settings, stroke capture, polyline simplification, preview ring |
| `TerrainEditData.cpp/.h` | Owns the `GodClient Edits` layer: add/remove edit layers, undo/redo, invalidate region, save/backup |
| `ActionsTerrain.cpp/.h` | Menu and toolbar actions (Window → Terrain, Terrain mode, Save, Undo hooks) |
| changes to `GameWidget.cpp` | Route mouse to the brush when Terrain mode is on |
| changes to `MainFrame.cpp` | Create the panel and add the menu entries |
| changes to `SwgGodClient.vcxproj` | Add the files; set Character Set = Multi-Byte |

Getting at the live generator:
`TerrainObject::getInstance()->getAppearance()` → `ProceduralTerrainAppearance` → its `TerrainGenerator` and `addLayer/removeLayer/invalidateRegion`. All are already in the libraries the God Client links.

---

## 4. Phases

| # | Phase | Done when | Risk |
|---|---|---|---|
| **0** | **Get the unchanged God Client building** (Character Set fix, then work through any further compile/link errors; skip libMozilla/Vivox as the README suggests). Time box: one session. | A new `SwgGodClient_r.exe` launches and logs in like the 2016 one. | **High:** unknown number of errors. Gate for everything else. |
| 1 | Panel + Raise/Lower/Flatten with live preview and undo, on `simple.trn` | Strokes appear instantly; undo removes them | Medium |
| 2 | Save / Save As / backup / Revert | Saved file reloads identically after restarting the client | Medium |
| 3 | Paint + Clear flora | Shader and flora edits show and save | Low |
| 4 | Water, passable, edit list polish | — | Low |
| 5 | Export for server + manual section | Edited planet works on the server after a restart | Depends on server access |

Phase 0 changes nothing for players: the rebuilt exe goes in a **separate folder** (e.g. `E:\SWG_GodClient\swg\exe\win32\SwgGodClient_terrain_r.exe`), and the 2016 exe stays as it is.

---

## 5. Option B design (fallback / companion)
- Add **Export to God Client** in the Galaxies Terrain Editor. It writes `<planet>.trn` (+ `.tga` bitmaps, shaders, textures) straight into `E:\SWG_GodClient\swg\data\sku.0\sys.client\compiled\game\…`, after backing up any file it replaces.
- In the God Client: **Game → Terrain → Reload Terrain**.
- First job either way: **test one export in the client.** The manual says exports haven't been tried in-game yet.

---

## 6. Risks and limits

| Risk | Mitigation |
|---|---|
| God Client won't build in reasonable time | Phase 0 time box; fall back to Option B |
| Rebuilt exe behaves differently from the 2016 one | Separate exe name and folder; 2016 exe untouched |
| **Server doesn't see client edits** (heights, collision, spawns) | Explain clearly in the UI and manual; "Export for server" step; restart server to apply |
| Many strokes → many layers → slower terrain generation | One layer per stroke (polyline); a later "merge edits" step could bake them into one bitmap height layer |
| Bad save corrupts a planet | Automatic backup before first save; loose override means deleting the file restores the shipped one |
| 32-bit memory | Already raised to 4 GB with the LAA flag; apply the same flag to the rebuilt exe |

---

## 7. Open questions for you
1. **Which option:** A (in-game, rebuild God Client), B (link your web editor), or C (both)?
2. Do you have access to the server's data folder on 192.168.1.200 to deploy edited `.trn` files?
3. Which planet do you want to edit first? (Testing starts on `simple.trn` either way.)
4. Which tools matter most to you: height, paint, flora, water?

---

## 8. Phase 6 — new terrain tools (planning, 2026-11-10)

### 8.1 What the engine gives us that the editor doesn't use yet

| Engine part | What it can do | Used now? |
|---|---|---|
| `FilterSlope`, `FilterHeight`, `FilterDirection` | Limit any layer to steep or flat ground, a height band, or slopes facing one way | No |
| `FilterShader` | Limit a layer to ground that already has a given texture | No |
| `FilterFractal` | Break a layer into natural patches | No |
| `AffectorColorConstant` | Tint the ground | Only to reset tint under Paint |
| `AffectorEnvironment` | Change the environment family (sky, ambience) in an area | No |
| `AffectorPassable` | Mark ground as blocked for walking | No |
| `AffectorExclude` | Stop lower layers from acting in an area | No |
| `ServerCommander::createObject` | Create real server objects at a position (used by Paste) | Not by the terrain panel |

There is **no height-bitmap affector** in this engine version. Anything that copies or imports exact ground shapes has to use the per-point method Smooth already uses: one fixed-height patch per terrain grid point.

### 8.2 Candidate tools

| # | Tool | How it works | Effort | Risk |
|---|---|---|---|---|
| 1 | **Smart paint** (slope / height / facing limits) | Add optional filters to Paint, Plant flora and Clear flora: "only slopes over 30°", "only between 50 m and 120 m", "only north-facing". Paint cliffs rocky in one stroke across a whole mountain | Small | Low: filters are standard layer parts and save in the `.trn` |
| 2 | **Copy / paste ground** | Pick a rectangle, copy its heights (and optionally its texture), then stamp it elsewhere, rotated, scaled in height, or mirrored. Built like Smooth (per-point patches) | Medium | Medium: a large copy makes many patches; cap the size (about 256 × 256 m at 2 m grid) |
| 3 | **Heightmap stamp** | Load a greyscale image (`.tga` / `.png`) and stamp it as a mountain, crater or mesa at a chosen size and height. Same mechanism as 2 | Medium | Medium, same caps as 2. Shares code with 2 |
| 4 | **Ramp** | Drag from A to B: a straight slope between the heights at the two ends, with soft sides. For paths up to buildings and between terraces | Small–medium | Low: uses the per-point bake |
| 5 | **Flatten under selection** | Select a building or object in the God Client, press a button: the ground under its footprint (plus a margin) is levelled to its base height with a soft rim | Small | Low: reuses Flatten with a rectangle or circle shape from the object's bounding box |
| 6 | **Object scatter / dense forest** | Choose an object template (a tree, rock or prop) and paint an area: real server objects are created at the chosen density, snapped to the ground, with random turn and size. Lifts the "one tree per 16 m tile" limit | Medium | Medium–high: these are **server objects** (saved in the server database, not in the `.trn`). Undo deletes them. Needs a sensible cap per stroke |
| 7 | **Tint brush** | Paint a colour tint on the ground (dry patches, burnt ground, darker forest floor) | Small | Low |
| 8 | **Environment / no-walk zones** | Paint an area that changes sky/ambience, or blocks walking | Small | Low, but the effect is subtle and needs a server restart to matter for walking |
| 9 | **Erosion** | Simulated water erosion that cuts gullies into slopes for a weathered look | Large | Medium: slow to compute, many patches |

### 8.3 Suggested order

1. **Smart paint**: the biggest visual win for the least work, and it makes Plant flora and Paint far more useful on real mountains.
2. **Flatten under selection** and **Ramp**: small, and they make placing buildings practical.
3. **Copy / paste ground** and **Heightmap stamp** together: they share the bake code.
4. **Object scatter**: needs care because it creates server objects; do it after the terrain-only tools.
5. Tint, environment, erosion: if wanted.

### 8.4 Shared design points

- Every new tool is one entry in the edit list, with Undo/Redo, Check last stroke and log lines like the existing tools.
- Filters (tool 1) are a new "Limits" box in the panel that applies to Paint, Plant, Clear flora and Tint. Off by default.
- Per-point tools (2, 3, 4) share a new `bakeHeights(area, function)` built from `bakeSmooth`, with the same chunk-size sampling that fixed the Smooth crash.
- Object scatter keeps a list of the object IDs it created for each stroke, so Undo, Remove all and Revert can delete them. It is not written to the `.trn`; the panel says so.

### 8.5 Open questions
1. Which tools to build first?
2. For copy/paste and heightmaps: is a 256 × 256 m limit per stamp enough?
3. For object scatter: is it fine that these objects live on the server (everyone sees them at once, no `.trn` save needed)?

**Decision (2026-11-10):** build all four groups, in this order:
- 6a: Smart paint
- 6b: Flatten under selection, then Ramp
- 6c: Copy/paste ground, then Heightmap stamp
- 6d: Object scatter / dense forest

Tint, environment and erosion are on hold.

### 8.6 Phase 6a — Smart paint: done (2026-11-10, verified in game)

- **Limits box** for Paint, Plant flora and Clear flora: slope range, height band, and "only where the ground is already" (an existing texture).
  - Paint, bushes, rocks and grass: engine filters on the stroke layer (`FilterHeight`, `FilterSlope`, `FilterShader`), saved in the `.trn`.
  - "Flatter than" is an inverted `FilterSlope` sublayer, because the filter leaves out exactly flat ground.
  - The texture filter sits on the innermost sublayer, with the affectors, because the engine only refreshes the texture map for a layer that has affectors.
  - Trees are checked once at planting. Clear flora with limits checks each baked tree and clears only the ones that pass, with small clear circles; the save does the same.
  - Facing direction was left out: `FilterDirection` can only reach half the compass.
  - Usability: ticking Height starts the band at the ground ±10 m. A stroke that changes nothing names the blocking limit. Painting a texture "only on" itself is refused. Each limited stroke logs per-spot results.
- **Lake fix:** water is always kept 0.3 m below the lowest point of the shoreline, so it can't stand above the bank.
- **Flora scatter, whole planet:** sets the `.trn`'s flora tile borders for bushes, rocks and grass (0% = planet's grid look, 100% = anywhere in the cell). It applies live by re-placing the flora managers' nodes, and is saved with the planet. Tatooine bushes and rocks: 4 m cells, 1.5 m border.
  - Engine additions: `ProceduralTerrainAppearanceTemplate::setFloraTileBorders`, `ClientProceduralTerrainAppearance::setFloraTileBorders`, `ClientRadialFloraManager::setTileBorder`.
- Panel: drop-down lists can shrink, plus a right-hand gap, so the spin-box arrows stay clear of the scroll bar.
- Manual, guide and both PDFs updated.

### 8.7 Phase 6b — Flatten under selection and Ramp: done (2026-11-10, verified in game)

- **Ramp** (tool `T_ramp`, stroke `SK_ramp`):
  - Only the drag's two ends are kept. When the drag ends, the ground on the pole grid inside the corridor (half-width = Brush size, soft edge at the sides) is moved toward the straight line between the ground heights at the two ends.
  - Stored and rebuilt exactly like Smooth (`createSmoothLayer` patches).
  - New shared helper `sampleGroundGrid` (chunk-size blocks), ready for 6c copy/paste and heightmaps.
- **Flatten under selected objects** (button):
  - For each selected object in the world cell, the footprint comes from its appearance `BoxExtent` (or 70% of its sphere), plus a 1 m margin and a rim of Brush size.
  - The footprint is turned into a rotated rectangle in world space and made into a Flatten polygon stroke at the object's base height, with soft edge = rim. One edit per object.
  - Strokes made in code use `m_strokeOutlineReady` (skip `buildOutline`) and `m_strokeNameOverride`.
  - Verified on a large Corellia-style player house (33 × 39 m footprint).
- Manual, guide and PDFs updated.

### 8.8 Phase 6c — Copy/paste ground and Heightmap stamp: done (2026-11-10, verified in game)

- **Copy ground** (`T_copy` / `SK_copy`):
  - The drag is a rectangle. On release, `sampleGroundGrid` reads it, and heights are stored relative to the average along the rectangle's edge.
  - It's stored in an in-memory clipboard only; no layer and no edit. Limit 512 m a side.
- **Stamp** (`T_stamp` / `SK_stamp`):
  - On release, the clipboard is baked at the click point, turned, mirrored and height-scaled.
  - Base = average ground along the stamp's outline; the edges fade over Soft edge. Stored like Smooth (pole patches), so Undo, Redo and Save work.
- **Heightmap images:**
  - The panel loads PNG, BMP or JPG through `QImage`, and TGA with its own reader (8/24/32-bit, raw or RLE).
  - Images are converted to grey 0..1, reduced to 1024 pixels a side at most, and put on the clipboard with Image size and Image height of white. The top of the image is north.
  - Samples: `J:\SWG_Claude\heightmaps\mountain.png` and `crater.png`.
- **Ground outlines** (`TerrainEditData::drawOverlay`, called from `GodClientData::draw`):
  - Drawn: the brush circle, shapes while dragging (Copy in yellow), the Ramp corridor, and the stamp footprint at the cursor.
  - Lines follow the ground via `getHeight` on the chunks already loaded.
- **Layout:** the left dock column starts at 220 px and the right (Terrain Editor, Filter) at 430 px, so the fixed 1024 × 768 game view fits.
- Manual, guide and PDFs updated.

### 8.9 Phase 6d — Object scatter: done (2026-11-10, verified in game)

- **Tool** `T_scatter` / `SK_scatter`:
  - The template comes from the Tree Browser's ServerTemplates selection, completed to `object/<name>.iff` as Ctrl+N does.
  - Spots: one per Object spacing cell at a random point, kept at Plant density and where the Limits pass, at most 250 per stroke, with y = ground and an optional random yaw.
  - The edit is an empty marker layer (never saved).
- **Creation:** `ServerCommander::createObject` sends `object createTranslateRotate`. The server doesn't return the ID, so `updateScatter` (every 0.5 s from `tick`) matches each placement to an object that appears within 0.4 m with the matching shared template.
  - It scans the `WOL_Tangible`, `WOL_TangibleNotTargetable` and `WOL_TangibleFlora` lists, and gives up after 2 minutes.
  - It warns once if nothing comes back within 15 s.
- **Undo / Remove all:** `object destroy <id>` through the new `ServerCommander::destroyObjectById`. Placements not seen yet are destroyed when they arrive. Redo re-creates them.
- **First test:** no objects appeared, because the short Tree Browser name (`static/flora/...`) was sent without `object/` and `.iff`. Fixed.
- **Verified:** 62- and 35-object strokes of Rori trees, all matched back in about 1 s; Undo destroyed 35, and Redo re-created and matched all 35.
- **Panel:** Object scatter box (object name, Object spacing, Random turn). The panel fits at 450 px; the first left column is 200 px.
- Manual, guide and PDFs updated.

### 8.10 Save destinations: done (2026-11-10, verified in game)

- Central storage `J:\SWG_Assets_storage` (`terrain\`, `heightmaps\` with the sample images).
- The panel's "Save copy to folder…" became a **Save destinations** box:
  - **My storage folder** (default `J:\SWG_Assets_storage`) with **Save to my folder**.
  - **Server data folder** with **Send to server**, which confirms first and checks the folder can be reached.
  - Both folders are remembered with `QSettings` (`SWG/GodClient/TerrainEditor`).
- `saveCopyTo` now keeps any file it replaces as `terrain\_terrain_backup\<name>_<date_time>.trn`, and logs `SAVE TO FOLDER`.
- Load heightmap image opens `<storage>\heightmaps` by default.

### 8.11 Joined water levels and Waterfall: done (2026-11-10, verified in game)

- **Problem:** a lake next to a river sat lower than the river. The shore clamp (0.3 m below the lowest outline ground) treated the river bed as shore.
- **`existingWaterHeight`:** uses the template's `getWaterHeight` (water tables and ribbon quads), counting only water at or above the ground.
  - **Lakes:** outline and inside points that are in water are left out of the shore. The lake takes the lowest such water level (if it's below the dry shore), then digs its basin from there.
  - **Rivers:** path points in existing water use that height for the ribbon. They're stored in `PathSamples::waterHeights`, so saves rebuild them the same way.
- **Waterfall** (`T_waterfall` / `SK_waterfall`):
  - Drag top to bottom; the ground is sampled every 0.5 m.
  - The water height is the highest ground within 3 m behind each point, plus 0.3 m. So the water overshoots the edge about 3 m and drops in front of the face, never climbing. The ends join existing water.
  - It's built as an `AffectorRibbon`, registered as run-time water so you can swim in it.
- **Waterfall looks:**
  - The engine draws ribbon water with the shader's own animation; the ribbon velocity isn't used. The desert water shader looked like it flowed upward.
  - Choices: `wter_dath_mistyfalls` (default), `wter_watefall_tile`, or the planet's water. The look is stored in the edit's `m_waterShader`, and a Flip flow option (the edit's `m_param`) reverses the point order.
  - The Theed falls switcher shaders were dropped: their `wter_thed_falls_static01`–`08` frames aren't in the client data, and there are no `.tre` archives.

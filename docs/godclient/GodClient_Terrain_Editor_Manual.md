# God Client Terrain Editor — User Manual

2026-11-05

The Terrain Editor lets you sculpt, paint, plant and flood the ground of a planet from inside the God Client. Each change shows within about a second, can be undone, and can be saved into the planet's `.trn` file.

## Getting started

1. Start the rebuilt client: `E:\SWG_GodClient\swg\exe\win32_new\SwgGodClient_r.exe`, or the launcher `J:\SWG_Claude\God Client Terrain Editor (E).cmd`. Older copies elsewhere (for example `exe\win32`) do not have the editor's latest fixes.
2. Log in and go to an open outdoor spot on a ground planet.
3. Open the panel: **Window → Terrain Editor**. It normally opens docked on the right.
4. Tick **Terrain brush on**. The status line shows the ground height under the cursor.

**Mouse controls with the brush on**

| Action | What it does |
| --- | --- |
| Left drag | Use the current tool |
| Right drag | The tool's alternate: Raise ↔ Lower |
| Right click (Flatten, Level, Water level) | Pick the ground height under the cursor into the height box |
| Alt + mouse | Moves the camera as usual |

Untick **Terrain brush on** to select objects with the left button again.

**Outlines on the ground.** With the brush on, the game view draws lines on the ground showing what the next stroke will cover:

| Colour | Shows |
| --- | --- |
| Green circle | The brush size, following the cursor |
| White outline | A rectangle, circle or polygon while you drag it (cyan for Water level) |
| Yellow | The Copy ground rectangle while you drag it; a small yellow cross before you start |
| Cyan rectangle | Where a Stamp will go, already turned and sized, at the cursor |
| Green corridor, yellow line | A Ramp's width and its centre line while you drag |

Editing needs multi-threaded terrain: `[ClientTerrain] terrainMultiThreaded=true`, which is the default. Without it, the status line says the brush is disabled.

## The panel

Pick a tool, pick a shape, set the boxes the tool uses, then drag in the game window. The panel scrolls if it doesn't fit.

| Control | Used by | Meaning |
| --- | --- | --- |
| Tool (18 buttons) | — | Raise, Lower, Flatten, Level, Paint texture, Clear flora, Roughen, Terrace, Plant flora, Road, River, Water level, Smooth, Ramp, Copy ground, Stamp, Object scatter, Waterfall |
| Flatten under selected objects | — (a button) | Levels the ground under the selected objects (see Flatten under selected objects) |
| Shape | All but Road, River, Ramp, Copy ground, Stamp, Waterfall | Brush, Rectangle, Circle or Polygon (see Shapes) |
| Waterfall box | Waterfall | Look (the water shader) and Flip flow (see Waterfalls) |
| Stamp box | Copy ground, Stamp | The clipboard, Load heightmap image…, Turn, Mirror, Height (%), Image size, Image height (see Copy ground and Stamp) |
| Brush size (m) | All | Brush radius; Road, River and Ramp are twice this wide. For Flatten under selected objects, the width of the slope around each object |
| Soft edge (%) | All | How far the effect fades out at the edge (0 = hard edge) |
| Raise/lower by (m) | Raise, Lower | Height added or removed per stroke |
| Flatten/Level/Water height (m) | Flatten, Level, Water level | Target height; right-click the ground to pick it |
| Paint texture | Paint, Road, River | Ground texture, listed as name (#id, priority n) |
| Roughen pattern | Roughen | One of the planet's noise patterns |
| Roughness (m of bumps) | Roughen | Size of the bumps |
| Terrace step (m) | Terrace | Height of each step |
| Terrace flat part (%) | Terrace | Share of each step that is flat |
| Plant | Plant flora | `flora:` bushes and rocks, `grass:` grass, `tree:` trees |
| Plant density (%) | Plant flora, Object scatter | How thickly to plant |
| Object scatter box | Object scatter | The selected object, Object spacing (m), Random turn (see Object scatter) |
| River/Lake depth (m) | River, Water level | Channel depth below the bank, or basin depth below the water |
| Smooth strength (%) | Smooth | How far the ground moves toward smooth per stroke |
| Flora scatter, whole planet (%) | — (a planet setting) | How far bushes, rocks and grass may stray from the middle of their grid cells (see Flora scatter) |
| Limits box | Paint, Plant flora, Clear flora, Object scatter | Slope, height and existing-texture limits (see Smart paint limits) |

Below the boxes are, in order:

- the status line. With Paint, Plant flora or Clear flora chosen, it also shows the slope and texture under the cursor, and whether that spot is inside the limits
- the edit list, newest last
- **Undo**, **Redo** and **Remove all**
- **Check last stroke**, with a message line under it
- **Save (this client)** and **Revert to saved**
- the **Save destinations** box: **Save to my folder** and **Send to server**

## Shapes

The Shape box decides how a drag becomes an area. Every tool except Road and River can use any shape.

| Shape | How to draw it | When it appears |
| --- | --- | --- |
| Brush | Drag; the tool follows the cursor at brush size | Live while dragging (Smooth, trees, Road, River: when you let go) |
| Rectangle | Drag from one corner to the opposite corner | When you let go |
| Circle | Drag outward from the centre | When you let go |
| Polygon | Drag all the way around the outline; it closes itself | When you let go |

- **Soft edge with shapes** is a share of the way from the outline to the middle. At 50%, a fill fades in over the outer half and is at full strength in the inner half, whatever its size. With the brush, soft edge is a share of the brush radius.
- **Shapes that build nothing:**
  - a shape smaller than about 1 m (a click without a drag)
  - a polygon that encloses no area (a line rather than a loop)

  The message line says why.
- After a height fill, the message line shows the ground in the middle before and after, for example `180.0 m → 185.0 m (+5.0 m)`.

## Height tools

| Tool | What it does | Settings | Tips |
| --- | --- | --- | --- |
| Raise | Adds height | Raise/lower by | Right drag lowers; strokes add up |
| Lower | Removes height | Raise/lower by | Right drag raises |
| Flatten | Sets the ground to one height | Height box | The first stroke picks the height under the click, and later strokes reuse it so overlaps join up flat. Click Flatten again for a new height, or right-click to pick one |
| Level | Sets the ground to the height box | Height box | Right-click picks the height under the cursor |
| Roughen | Adds natural bumps around the current ground | Roughen pattern, Roughness | Uses the planet's own noise patterns |
| Terrace | Turns slopes into steps | Terrace step, Terrace flat part | Works best on slopes |
| Smooth | Softens bumps | Smooth strength | Built when you let go; go over an area again to smooth it more |
| Ramp | A straight slope between two points | Brush size, Soft edge | Drag from one end to the other; built when you let go |

**How Smooth works:**

1. The ground is sampled on the terrain's 2 m grid and blurred. The blur width is half the brush radius, or a quarter of the smaller side for shapes, up to 32 m.
2. Every grid point that moves more than 2 cm gets a small fixed-height patch.
3. The message line reports how many points moved and by how much.

Very large areas use 4 m patches or coarser.

**Ramp:**

1. Choose **Ramp** and drag from one end of the ramp to the other. Only the start and end points count; the path in between doesn't matter.
2. When you let go, the ground between becomes a straight slope, from the ground height at the start to the ground height at the end.
3. The ramp is as wide as Brush size × 2, and fades into the surrounding ground over the soft edge at its sides.

- It cuts through bumps and fills dips along the way. It's built in small patches like Smooth, so Undo, Redo and Save work as usual.
- The message line gives the length, the two end heights and the slope in degrees, for example `24 m long from 190.2 m to 180.0 m (22.9 deg slope)`.
- To ramp up onto a mound or a building's terrace, start on the low ground and end on top, or the other way round.

**Flatten under selected objects** levels the ground under buildings and other objects:

1. Untick **Terrain brush on** and select one or more objects in the usual way (Shift-click to add more).
2. Set **Brush size** to the width you want for the slope back to the old ground, for example 8 m.
3. Press **Flatten under selected objects**.

- Each object's ground is levelled to its base height. The flat area covers its footprint, taken from its bounding box, plus 1 m, and it turns with the object's rotation.
- Outside that, the ground slopes back to the old ground over Brush size.
- Each object becomes its own entry in the edit list, for example `Flatten under shared_player_house_corellia_large_style_01.iff to 180.0 m (67 x 73 m, rim 16 m)`. Undo takes them back one at a time.
- Objects inside buildings are skipped.
- Buildings already keep their own floor flat, so the effect shows around them: bumps, slopes or raised ground near the walls are levelled.

Trees on the planet move with the ground after every height edit, and move back on Undo.

## Surface tools

**Paint texture** stamps a ground texture from the Paint texture list.

- It also resets the planet's colour tint inside the stroke, so the texture shows clean instead of blotchy.
- Each texture has a priority, shown in the list. Where two textures meet, the higher one is drawn over the lower. Narrow strokes look cleanest with a high-priority texture (lower in the list).
- A radius-16 stroke is small. Use a bigger brush to see it from a distance.

**Clear flora** removes trees, rocks, bushes and grass in the area. Cleared areas also stay clear when saved.

**Plant flora** adds plants from the Plant list at Plant density:

| Plant entry | What it adds | How it is stored |
| --- | --- | --- |
| `flora:` | Bushes, rocks and similar | A terrain rule |
| `grass:` | Grass and small plants | A terrain rule |
| `tree:` | Trees (the families already used on the planet) | The planet's baked tree map |

**Planting trees:**

- On Tatooine and most planets, trees come from a baked map with one slot per 16 m tile. You get at most one tree per tile: scattered trees, not dense forest.
- Trees appear when you let go. Only empty tiles get a tree; tiles that already have one keep it.
- Density is a fresh random pick per stroke, so going over an area again adds more until every tile is full.
- Each tree's height is set from the ground under it, and is updated whenever you edit the ground there.
- Trees planted inside an area you cleared with Clear flora stay hidden.

## Copy ground and Stamp

**Copy ground** copies the shape of a piece of ground, and **Stamp** puts it somewhere else. They share a **clipboard**, shown at the top of the **Stamp** box. The clipboard holds either copied ground or a heightmap image.

**Copying ground:**

1. Choose **Copy ground (rectangle)**.
2. Drag a rectangle from one corner to the opposite corner. A yellow box shows it while you drag. It can be 4 m to 512 m each way.
3. When you let go, the shape goes to the clipboard. The terrain doesn't change, and there's no entry in the edit list.

Heights are copied relative to the average ground along the rectangle's edge. To copy a hill, drag the box **around** it, with some flat ground on every side. A box drawn across the top of a hill copies only the bumps on top.

**Stamping:**

1. Choose **Stamp (click to place)**. A cyan outline shows where the stamp will go.
2. Set **Turn (deg)**, **Mirror** and **Height (%)** if you want to change it.
3. Click where the middle should go. You can drag to move it before letting go.
4. When you let go, the stamp is built. Its edge meets the average ground along its outline, and it fades in over **Soft edge**.

| Setting | What it does |
| --- | --- |
| Turn (deg) | Rotates the stamp |
| Mirror | Flips it left to right |
| Height (%) | 100 = as copied, 200 = twice as tall, 50 = half; −100 turns it upside down (a hill becomes a pit) |
| Image size (m) | Heightmap images only: how wide the stamp is |
| Image height of white (m) | Heightmap images only: how high white is (black = no change) |

Each stamp is one entry in the edit list. Undo, Redo and Save work as usual, and trees move with the new ground.

**Heightmap images:**

1. Press **Load heightmap image…** and open a greyscale PNG, BMP, JPG or TGA (8, 24 or 32-bit, plain or RLE). Colour images are turned to grey.
2. The clipboard now shows the image. Set **Image size** and **Image height of white**.
3. Stamp it like copied ground. The top of the image points north.

Images larger than 1024 pixels a side are reduced to fit. Two samples are in `J:\SWG_Claude\heightmaps`:

- `mountain.png`: a smooth round hill.
- `crater.png`: a raised ring. For a full crater, stamp `mountain.png` at **Height −100%** first, then stamp the ring over it.

Very large stamps use coarser patches (4 m or more), like Smooth.

## Object scatter

**Object scatter** paints real server objects, such as trees, rocks and props, at any density. There's no one-per-16 m limit as with baked trees.

1. Untick **Terrain brush on**. In the **Tree Browser**, open the **ServerTemplates** tab and pick an object. This is the same template **Ctrl+N** creates. The **Object scatter** box shows its file name.
2. Tick **Terrain brush on** again and choose **Object scatter**.
3. Set **Object spacing (m)** (default 8) and **Plant density (%)**, and tick or untick **Random turn**.
4. Paint with the brush, or fill a rectangle, circle or polygon. When you let go, the objects are created on the server, each standing on the ground.

- There's one candidate spot per spacing cell, at a random place inside it, so objects don't line up in rows. Plant density decides how many of the spots get an object.
- The **Limits** box applies, for example rocks only on slopes over 20°.
- A stroke makes at most 250 objects, to protect the server. Paint again for more.
- They're real server objects: everyone on the server sees them right away, they stay after a restart, and **Save** doesn't write them to the `.trn`.
- **Undo** destroys them and **Redo** creates them again; **Remove all** destroys them as well. The editor learns each object's ID when the server sends it back, usually within a second or two. Objects not back yet when you press Undo are destroyed as soon as they arrive.
- **Revert to saved** doesn't remove scatter objects; undo them first if you don't want them.
- **Check last stroke** shows how many objects were sent and how many the server has sent back.
- If the server sends back none of a stroke's objects within 15 seconds, the message line says so. Check that Ctrl+N works with that template and that the account has god rights.

## Smart paint limits

The **Limits** box makes Paint, Plant flora, Clear flora and Object scatter act only on some of the ground under the stroke. A stroke changes only the ground that passes every limit you've ticked.

| Limit | What it does | Example |
| --- | --- | --- |
| Slope between (deg) | Only ground whose slope is in the range; 0 = flat, 90 = a cliff | 30 to 90: rock on steep slopes only. 0 to 10: flat ground only |
| Height between (m) | Only ground between the two heights | A beach band, or bushes only on low ground |
| Only where the ground is already | Only ground that already has this texture | Paint Sand_light over gravel_small only, leaving the rock alone |

- **Only where the ground is already** is the texture to paint *over* or plant *on*, not the texture to paint (that's **Paint texture**). Leave it at **(any texture)** unless you want it. If it's set to the same texture as Paint texture, the stroke is refused, because it could change nothing.
- Ticking **Height between** sets the band to 10 m either side of the ground under the cursor. Change the two boxes as needed.
- Hover over the ground first: the status line shows its height, slope and texture, and says whether it's inside the limits.
- After a limited Paint or Plant stroke, the message line says how many checked spots passed. If none did, it names the limit that blocked the stroke and describes the ground there.
- Limited Paint, bushes, rocks and grass use the engine's own terrain filters, so the limits are saved in the `.trn` with the stroke.
- **Trees:** each tree's spot is checked once, when you plant it.
- **Clear flora** with limits is built when you let go. Each tree in the area is checked, and only the ones that pass are removed. Bushes and grass follow the limits too.
- The edit list shows the limits a stroke used, for example `[slope 30-90 deg]`.

There's no facing-direction limit (such as north-facing slopes only). The engine's direction filter only covers half the compass.

## Flora scatter

The engine places bushes, rocks and grass on a planet-wide grid: one plant at most in each cell, at a random spot inside it. The planet's `.trn` also sets a border that keeps plants away from the cell edges. With a wide border, every plant sits near the middle of its cell, so dense planting looks like rows. On Tatooine, bushes and rocks use 4 m cells with a 1.5 m border, which is about 25% scatter.

**Flora scatter, whole planet (%)** changes that border:

- **0%** keeps plants near the middle of their cells, which gives a grid look.
- **100%** lets a plant be anywhere in its cell, which looks natural. With one plant per cell at most, even 100% looks evenly spread rather than clumped.
- When a planet loads, the box shows that planet's current value.
- A change applies at once, and every bush, rock and grass patch on the planet is re-placed.
- It's saved with the planet when you click **Save**, and **Revert to saved** undoes it. It isn't an entry in the edit list.
- Trees aren't affected: they come from the baked tree map, at most one per 16 m tile.
- Bushes and grass only change how the planet looks in the client; the server doesn't use them.

## Paths and water

**Road** and **River** follow the path you drag and are built when you let go.

- Your path is evened out and its corners rounded first.
- The ground heights along it are recorded, so Undo, Redo and Save rebuild it exactly.
- Both are as wide as the brush size × 2 and use the Paint texture.

What each one does:

- **Road:** follows the ground along the path, smoothed and flat across its width, with the road texture and no flora.
- **River:** carves a channel River/Lake depth deep and paints the banks and bed. It fills the channel with one continuous strip of water, about 30% of the way down. The water uses the planet's own water look (Tatooine: `wter_dsrt_bright`), and you can swim in it.

**Water level** makes a lake or pond:

1. Choose Water level and a shape. With the Brush shape, your drag draws the outline.
2. Set River/Lake depth, for example 3 m. The ground inside is dug down to the water height minus the depth, with banks as wide as the soft edge.
3. Leave the height box at 0 to fill the lake to just below the shore, or type a lower height for a lake that sits lower in its basin.
4. Drag the shape. You can swim in the lake.

The water only exists inside the outline, so it's always kept **0.3 m below the lowest point of the shoreline**. Otherwise it would stand above the bank as a raised sheet.

- If the height box is under all the ground in the shape (for example still at 0 m), the lake is filled to just below the shore.
- If the height box is above the lowest point of the shore, the water is lowered to just below it.
- The message line says when either happens, and the height box shows the height used.
- If River/Lake depth is 0 and the ground inside isn't lower than the shore, the water is hidden under the ground. The message line says to set a depth.

While Water level is selected, the status line shows how far the water height is above the ground under the cursor.

**Joining water.** New water meets water that's already there at the same level:

- **A lake touching a river or another lake:** points on its outline or inside it that are already in water don't count as shore. The lake takes that water's level, and digs its basin down from there. The message line says "the lake joins the water already there".
- **A river running into a lake or another river:** path points that lie in existing water take that water's height, so the river meets it level. The message line says how many points joined.
- Only water at or above the ground counts, so a deep planet-wide water table under the land is ignored.

**Waterfalls**

1. Choose **Waterfall (drag top to bottom)**.
2. Drag from the top of the fall, a few metres back from the edge, to at least 4–5 m past the foot of the cliff. A cyan outline shows the width (Brush size × 2; about 4 gives an 8 m fall).
3. When you let go, a sheet of falling water is built. It carries on about 3 m past the cliff edge, then drops nearly straight down in front of the face, so it doesn't lie on or cut into the rock. On gentler slopes it steps down just above the ground. It never flows uphill.
4. If the top starts in a river, or the bottom ends in a lake or pool, that end joins the existing water's level.

The **Waterfall** box sets:

| Setting | What it does |
| --- | --- |
| Look | **Dathomir misty falls** (default), **Waterfall tile**, or **The planet's own water** |
| Flip flow | Tick it before drawing if the water seems to run upward with the chosen look |

- The look and the flip are saved with each waterfall, so Undo, Redo and Save keep them. You can swim in it.
- Good test: Raise a 40 × 40 m rectangle 15 m with Soft edge 10% for a cliff, put a Water level pool at its foot, then draw the waterfall from the top into the pool.
- The game's Theed waterfall shaders aren't offered: they need frame files (`wter_thed_falls_static01`–`08`) that this client data doesn't have.

## History and saving

Every stroke is one entry in the edit list.

- **Undo** takes back the newest entry.
- **Redo** puts it back.
- **Remove all** takes back every edit since the last load.

Changing planet or reverting empties the list.

| Button | What it does |
| --- | --- |
| Save (this client) | Writes the planet's `.trn` into this client's loose data folder (Tatooine: `swg\data\sku.0\sys.client\compiled\game\terrain\tatooine.trn`), so the edits show here after a restart. The first save of a session backs up the previous file to `_terrain_backup\<name>_<date_time>.trn` beside it |
| Revert to saved | Reloads the planet from disk; unsaved edits are lost |

**Save destinations** (a box below the Save buttons) sends the saved planet to two more places:

| Control | What it does |
| --- | --- |
| My storage folder, **…**, **Save to my folder** | Writes `<folder>\terrain\<name>.trn` into the developer's own folder. The default is the central `J:\SWG_Assets_storage`; type or browse to use your own |
| Server data folder, **…**, **Send to server** | After asking you to confirm, writes `<folder>\terrain\<name>.trn` into the server's data. Pick the server folder that holds `terrain\`, for example `…\sys.shared\compiled\game`. If the folder can't be reached (an offline share or unmapped drive), it stops and says so |

- Both folders are remembered between sessions.
- Neither overwrites without a copy: a file already there is kept as `terrain\_terrain_backup\<name>_<date_time>.trn` in that folder.
- Neither changes this client's own file; use **Save (this client)** for that.
- **Load heightmap image…** opens `<storage folder>\heightmaps` by default. The central storage has the two sample heightmaps there.

The central storage folder is laid out as:

- `J:\SWG_Assets_storage\terrain\` for saved planets, with `_terrain_backup\` for older versions.
- `J:\SWG_Assets_storage\heightmaps\` for heightmap images.

A save writes three things:

- the planet as it was on disk at the first save of the session
- this session's edits
- the current tree map

Saving again never duplicates edits, and buildings' own ground flattening is never written.

**Getting edits to the server:** use **Send to server** (or copy the saved `.trn` into the server's data yourself), then restart the server. It only sees edits after the restart. Planted trees and cleared trees are in the same file, so they become solid on the server too.

## Diagnostics

**Check last stroke** measures the newest edit and shows a summary under the button. For example:

- **Paint:** how many ground points took the texture.
- **Lake:** whether there is water in the middle, and how deep.
- **Trees:** how many are in the live tree map.

The log file `terrain_edit_log.txt` sits next to the exe and records every session. Useful lines:

| Line | Means |
| --- | --- |
| `STROKE BEGIN` / `STROKE REFUSED <reason>` | A stroke started, or why it couldn't (no texture chosen, unknown pattern) |
| `STROKE …` | A finished edit, with its settings and path points |
| `STROKE DROPPED` | A shape too small or too thin, with its drag points |
| `REBUILD REQUEST` / `REBUILT after` / `NOT REBUILT` | The on-screen terrain rebuild after an edit |
| `RESULT shape middle` | Ground in a shape's middle before and after |
| `RAMP`, `FLATTEN UNDER` | What a ramp or Flatten under selected objects built (heights, sizes, footprint) |
| `COPY ground`, `CLIPBOARD image`, `STAMP` | What was copied or loaded, and where and how a stamp was built |
| `SCATTER` | Object scatter: spots chosen, create commands sent, objects matched back, objects destroyed |
| `WATER joins existing water`, `RIVER joins existing water`, `WATERFALL` | A lake or river taking an existing water level; a waterfall's length and top/bottom water heights |
| `SAVE TO FOLDER` | Save to my folder / Send to server, with any backup kept |
| `SMOOTH baked`, `WATER requested`, `TREES planting`, `TREES moved` | What Smooth, Water level and tree planting did |
| `STROKE LIMITS`, `LIMITS`, `spot …`, `CLEAR limited` | The limits a stroke used, and what passed, spot by spot |
| `FLORA SCATTER` | A Flora scatter change, with the planet's cell sizes and borders |
| `STALL` | The panel's timer ran late (a pause or freeze) |
| `CHECK` | The Check last stroke result |

If the log shows no new `STROKE BEGIN` lines after a test, a different copy of the client was running.

## Limits and troubleshooting

| Problem | Cause and fix |
| --- | --- |
| Nothing changed after a fix or update | A different copy of the client was started; run the one in `exe\win32_new` and check the log for new `STROKE BEGIN` lines |
| A small shape barely changed the ground | Check the message line's before/after; drag a bigger shape or lower Soft edge |
| A lake doesn't show | River/Lake depth is 0 and the ground inside isn't lower than the shore; set a depth so a basin is dug |
| A limited stroke changed nothing | Read the message line: it names the limit that blocked it. Usually the height band misses the ground, or "Only where the ground is already" is set to a texture that isn't there; set it to (any texture) |
| Planted bushes stand in rows | Raise **Flora scatter, whole planet** towards 100% |
| "Nothing built" after a shape | The drag was a click or a line; drag corner to corner, out from the centre, or all the way round |
| Painted texture looks mixed with the old one | Pick a texture with a higher priority (lower in the list), or a bigger brush |
| Trees don't appear in an area | They're inside a Clear flora area, every tile already has a tree, or the planet has no baked tree map |
| Brush does nothing, status says disabled | Set `[ClientTerrain] terrainMultiThreaded=true` and restart |

**Known limits**

- At most one tree per 16 m tile, and at most one bush or grass plant per flora cell.
- No facing-direction limit (the engine's direction filter covers only half the compass).
- Smooth, Ramp, Stamp, Road, River, shapes and trees appear only when you let go of the mouse, not while dragging. The outlines show where they will go.
- The stamp clipboard isn't saved; it's empty again after a restart.
- Object scatter: at most 250 objects per stroke. The objects live on the server, not in the `.trn`. After a client restart, Undo can no longer remove them; delete them like any other object.
- The client takes about 16 seconds to close; this is not a hang.
- Edits reach the server only through a saved `.trn` copied to it and a server restart.

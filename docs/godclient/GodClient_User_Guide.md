# SWG God Client — User Guide

2026-11-05

The God Client is the developer build of the Star Wars Galaxies client. It lets you fly a free camera through a live world and select, move, rotate and create objects on the server. It also lets you edit object scripts and variables, browse templates, and send system messages. The rebuilt client adds an in-game Terrain Editor.

This guide covers the rebuilt client in `E:\SWG_GodClient\swg\exe\win32_new`. Everything here comes from that client's source code and its configuration.

## 1. Starting up

### Which client to run

| What | Path |
| --- | --- |
| Rebuilt God Client (with Terrain Editor) | `E:\SWG_GodClient\swg\exe\win32_new\SwgGodClient_r.exe` |
| Launcher for it | `J:\SWG_Claude\God Client Terrain Editor (E).cmd` |
| Original 2016 client (no Terrain Editor) | `J:\SWG_Claude\God Client (E).cmd` |
| Source code | `E:\client-tools\src\game\client\application\SwgGodClient` (branch `godclient-terrain-tool`) |

Start the client from its own folder, as the launcher does, so it finds its `.cfg` files and data.

### Logging in

1. The client skips the intro (`skipIntro=true`) and shows the login screen.
2. It connects to the login server set in its config: `loginServerAddress0=192.168.1.200`, port `44453`.
3. Log in with an account that has god (admin) rights on your server. Without them, the server ignores move, create and edit requests.
4. Pick a character. The main window opens with the game view and the dockable tool windows.

### Configuration files

The client reads `client.cfg`, which includes `common.cfg`, `SwgGodClient.cfg` and `user.cfg`. All four are in the `win32_new` folder.

| Section and key | Meaning |
| --- | --- |
| `[ClientGame] loginServerAddress0`, `loginServerPort0` | Login server to connect to |
| `[ClientGame] skipIntro` | Skip the intro movies |
| `[GodClient] connectToPerforce` | Use Perforce for scripts and templates (SOE's original setup) |
| `[GodClient] loadServerObjects` | Show server-side objects in the Tree Browser |
| `[GodClient] scriptEditor`, `templateEditor` | Program used to view and edit scripts and templates (default `Textpad.exe`) |
| `[GodClient] local…Path`, `script…Path`, `template…Path` | Where source and compiled data live |
| `[GodClient] frameRateLimit` | Frame rate cap (0 = none) |
| `[ClientTerrain] terrainMultiThreaded` | Must be `true` for the Terrain Editor |
| `[SharedFoundation] demoMode=true` (in `user.cfg`) | Hides the "warnings logged" box at exit |

## 2. The main window

The main window has a menu bar, toolbars, a game window and dockable tool windows. Toolbars: **File**, **Edit**, **Game**, **View**, **Script**, **ServerObjectTemplate** and **ClientObjectTemplate**. Each button matches a menu item below.

The **Game Window** shows the 3D view. Its status bar shows the camera pivot position, the pivot distance and the buildout region under the camera. Its **Game Focus** button hands the keyboard and mouse to the game, as in the normal client.

Two ideas run through all the editing:

- **Selection:** the objects you have picked, shown highlighted.
- **Ghosts:** see-through copies of the selection that you move, rotate and scale freely. **Apply Transform** (Ctrl+T) sends the ghosts' positions to the server, which moves the real objects. Until then the server copies don't change.

## 3. Camera controls

The camera uses Maya-style controls: hold **Alt** and drag. The camera turns around a pivot point in front of it.

| Input | Action |
| --- | --- |
| Alt + left drag | Tumble: orbit around the pivot |
| Alt + right drag | Dolly: slide the camera parallel to the view |
| Alt + Ctrl + right drag | Slide the pivot along the ground, keeping the camera's height above it |
| Alt + middle drag (or left + right) | Zoom: move the pivot nearer or further |
| Alt + Ctrl + left or middle drag | Fly: free-look turning in place |
| Mouse wheel | Zoom in and out |
| C | Move the camera to the point under the cursor |
| = | Go to: type an exact pivot position (x, y, z) and distance |
| Ctrl+E / Ctrl+Alt+E | Center the camera on the selection / on the ghosts |
| Ctrl+F / Ctrl+Alt+F | Fit the selection / the ghosts to the screen |
| View → Reset Camera | Back to the default camera |
| Ctrl+F11 | Interior camera (for inside buildings) |
| Enter (or Ctrl+F8) | Toggle game focus: play as the avatar instead of the free camera |
| A (single-player only) | Move the avatar to the camera |

## 4. Selecting objects

| Input | Action |
| --- | --- |
| Left click an object | Select only that object |
| Shift + left click | Add to the selection |
| Ctrl + left click | Toggle the object in or out of the selection |
| Left drag on empty ground | Rubber-band select everything in the box |
| Shift / Ctrl + rubber band | Add / toggle everything in the box |
| Left drag a selected object | Drag it over the ground and other objects |
| Tree Browser (Objects tab) | Pick objects from a list; double-click to jump to one |
| Window → Filters | Limit what the lists show: by distance from the avatar, network id range, or template |

**Locked objects:** structures and other important objects start locked so they aren't moved by accident. Use **Edit → Unlock Selected** (Ctrl+U), **Unlock All**, **Unlock Non-Structures** or **Unlock Server Only Objects** to unlock them.

**Selection groups:** **Ctrl+F1 … Ctrl+F4** stores the current selection as group 1–4. **F1 … F4** restores that group.

## 5. Moving, rotating and scaling

Hold a key and drag with the mouse. You are moving the ghosts; press **Ctrl+T** (Apply Transform) to send the result to the server.

| Hold | Left drag | Right drag | Middle drag |
| --- | --- | --- | --- |
| **T** (translate) | Move in the view plane | Move up and down | Pull toward or push away from the camera |
| **G** (ground translate) | Move along the ground | Move up and down | Pull or push along the ground |
| **R** (rotate) | Yaw | Roll | Pitch |
| **S** (scale) | Scale height | Scale width and depth | — |

Hold **Ctrl** while dragging with **G** to align the ghosts to the terrain as they move.

**Keyboard nudges** (selection, one step each):

| Key | Action |
| --- | --- |
| Arrow keys | Nudge north, south, west, east |
| ; and ' | Up and down |
| > and < | Yaw +45° and −45° |

**Edit menu transform commands**

| Command | Shortcut | What it does |
| --- | --- | --- |
| Apply Transform | Ctrl+T | Send the ghosts' positions and rotations to the server |
| Drop to Terrain | Ctrl+D | Put the selection on the ground |
| Align to Terrain | Ctrl+Alt+D | Tilt the selection to match the slope |
| Toggle Drop to Terrain / Toggle Align to Terrain | — | Do this automatically while moving |
| Reset Rotation | — | Back to upright, facing north |
| Rotate +45 / Rotate −45 | — | Yaw by 45° |
| Random Rotate | — | A random yaw for each selected object (good for scattered props) |
| Set Transform | — | Type exact position (metres) and yaw, pitch and roll |
| Rotation mode (Yaw, Pitch, Roll) | — | Which axis the rotate buttons turn |
| Pivot mode (Self, Selection Center, Last Selection) | — | What the selection turns around |
| Tools → Snap To Grid… | — | Snap moves to a grid: block size, horizontal and vertical segments, sensitivity |

## 6. Creating, copying and deleting

| Command | Shortcut | What it does |
| --- | --- | --- |
| Create New Object [template] | Ctrl+N | Create an object from the template selected in the Tree Browser (its name shows in the brackets) |
| Copy / Cut / Paste | Ctrl+C / Ctrl+X / Ctrl+V | Copy, cut and paste selected objects (paste creates new server objects) |
| Delete | Delete | Destroy the selected objects on the server |
| Undo / Redo | Ctrl+Z / Ctrl+Y | Undo or redo object edits (moves, creates, deletes) |
| Replace selection with Server/Client template | — | Swap the selected objects for another template (ObjectTemplate menu) |
| Copy For POI | — | Copy the selection's layout for use in a point of interest (POI) |
| Create Theater | — | Write the selection as a theater: server and shared templates plus a data table, in the POI folders set in the config |
| Create ClientDataFile | — | Write the selection as a client data file |
| Edit Triggers… | — | Set a trigger volume's name and radius |
| Edit Form Data… | — | Edit the form fields of the selected object |
| Show Object Axes / Toggle Particle Systems Pause | — | Display aids |

## 7. Brushes and palettes

A **brush** is a saved group of objects with their relative positions, so you can stamp the same arrangement again and again. A **palette** is a named collection of templates and brushes.

1. Select the objects and choose **Tools → Save As Brush**.
2. Open **Window → Group Object Editor**. It has **Brushes** and **Palettes** tabs.
3. Pick a brush and use **Paste Brush Here**, or **Edit → Paste Brush**, to create it at the cursor.
4. **Create Brush**, **Delete Brush**, **Create Palette** and **Delete Palette** manage the lists.

Brushes are saved on your PC and loaded when the client starts.

## 8. Tool windows

All of these are under the **Window** menu and can be docked or floated.

| Window | What it's for |
| --- | --- |
| Tree Browser | Tabs: **Objects** (objects near you, filtered), **ServerTemplates**, **ClientTemplates**, **Scripts**, **Buildout Areas**. Each has a Refresh button |
| Object Editor | The selected object's **Attributes**, **ObjVars** (object variables), **Scripts** and **Skills**. Right-click a script or objvar to **Detach**, **Remove Objvar** or **Set Objvar** |
| Bookmarks | Camera and object bookmarks; jump to one, or delete it |
| Favorites | Templates you starred with **Add to favorites** in the template lists |
| Group Object Editor | Brushes and palettes (section 7) |
| Filters | What the object lists show: distance from the avatar, network id range, client or server template |
| Console | The God Client's message and command console (the game's own console is **~**, Game → Toggle Console) |
| Game Window | The 3D view |
| Region Viewer | Draws server regions on the map. Filter by type (Geographical, Mission, Municipal, PvP, Buildable, Spawnable, Difficulty), or show only regions whose value isn't 0 |
| Terrain Editor | Sculpt, paint and plant the terrain (section 11) |

## 9. Game and View menus

**Game menu**

| Item | Shortcut | What it does |
| --- | --- | --- |
| Game Focus | Enter | Play as the avatar instead of the free camera |
| HUD | Ctrl+H | Show or hide the game's on-screen interface |
| Toggle Console | ~ | Open the game console |
| Render Wireframe / Render Textured | — | Wireframe or normal rendering |
| Show Object Names / Show Object NetworkIds | — | Labels over objects |
| Show Debug Menu | — | Debug builds only |
| Terrain → Purge Terrain / Reload Terrain | — | Throw away and rebuild the terrain, or reload it from disk |
| Terrain → Terrain Editor | — | Open the Terrain Editor |
| Show Target Path, Ignore Target Path, Show Path Map | — | AI pathfinding display |
| Show Region Browser | — | Open the Region Viewer |

**View menu**

| Item | Shortcut | What it does |
| --- | --- | --- |
| Add Camera Bookmark | Ctrl+B | Save the camera position |
| Add Object Bookmark | Ctrl+O | Bookmark the selected object |
| Remove Object Bookmark | Ctrl+Alt+O | Remove its bookmark |
| Bookmarks ▸ | — | Jump to a camera or object bookmark |
| Center Selection / Center Ghosts | Ctrl+E / Ctrl+Alt+E | Aim the camera at them |
| Fit Selection / Fit Ghosts to Screen | Ctrl+F / Ctrl+Alt+F | Frame them |
| Show Trigger Volumes | — | Show trigger areas |
| Get SphereTree Snapshot / Turn Off Debug Spheres | — | Collision debug display |
| Reset Camera | — | Default camera |

## 10. Scripts, templates and server tools

The **Script** and **ObjectTemplate** menus were built for SOE's Perforce depot. They can create, view, edit, revert, submit and compile scripts and object templates, and reload them on the server. They need `connectToPerforce=true`, a Perforce server and the depot paths in `[GodClient]`. Without Perforce, use **View** to open files in the configured editor, and **Refresh** to rebuild the lists.

| Menu | Items |
| --- | --- |
| Script | Refresh, New, Edit, View, Revert, Submit, Compile, Server Reload |
| ObjectTemplate → Server | Refresh, New, Edit, View, Revert, Submit, Compile, Server Reload, Server Replace, Add to favorites |
| ObjectTemplate → Client | Refresh, New, Edit, View, Revert, Submit, Compile, Replace, Add to favorites |
| Buildout Areas (Tree Browser tab) | Refresh, Edit (mark for editing), Save |

**Other tools**

- **Tools → Send a System Message:** a message to one player, a chat room, the current planet, the current galaxy, or all of SWG.
- **Tools → Save Interior Layout:** write the objects inside the current building's cells to a layout file.
- **Tools → Grab Relative Coordinates:** the cursor's position relative to the selection, for building layouts.

## 11. Terrain Editor

Open it with **Window → Terrain Editor** (also **Tools** and **Game → Terrain**), then tick **Terrain brush on**. With the brush on:

- left drag uses the tool
- right drag uses its alternate (Raise ↔ Lower), or picks a height for Flatten, Level and Water level
- Alt + mouse still moves the camera

**Tools:** Raise, Lower, Flatten, Level, Paint texture, Clear flora, Roughen, Terrace, Plant flora, Road, River, Water level, Smooth, Ramp, Copy ground, Stamp and Object scatter.

- **Copy ground** and **Stamp:** copy the shape of a piece of ground and stamp it elsewhere, turned, mirrored or scaled, or stamp a greyscale heightmap image as a mountain or crater.
- Outlines on the ground show the brush, the shape being dragged, and where a stamp will go.
- **Object scatter:** paints real server objects (the template selected in the Tree Browser) at any density, with Undo and Redo. They live on the server, not in the `.trn`.

- **Ramp:** drag from one end to the other for a straight slope between the two ground heights.
- **Flatten under selected objects:** a button that levels the ground under the selected buildings to their base height.

- Plant flora also plants bushes, grass and trees.
- Every tool except Road and River can fill a rectangle, circle or polygon instead of a brush stroke.
- The **Limits** box keeps Paint, Plant flora and Clear flora to a slope range, a height band or one existing texture.
- **Flora scatter, whole planet (%)** stops bushes, rocks and grass from standing in rows.
- Water level keeps lakes just below the shoreline, in a dug basin.

Edits have Undo and Redo. **Save** writes the planet's `.trn` (backed up first), and **Revert to saved** reloads it. The server only sees terrain edits after you copy the saved `.trn` into its data and restart it.

The full Terrain Editor manual is a separate file: `J:\SWG_Claude\GodClient_Terrain_Editor_Manual.pdf` (or `.md`).

## 12. Troubleshooting

| Problem | Cause and fix |
| --- | --- |
| Moves don't stick | You moved the ghosts; press **Ctrl+T** (Apply Transform). Locked objects need **Unlock Selected** (Ctrl+U) first |
| Server ignores edits | The account has no god rights on the server |
| Can't log in | Check `loginServerAddress0` and `loginServerPort0` in the cfg files, and that the server is running |
| Script or template commands fail | They need Perforce (`connectToPerforce`) and the depot paths; use View and the configured editor instead |
| Keys move the avatar, not the camera | Game focus is on; press **Enter** to toggle it |
| Left click doesn't select | The Terrain Editor brush is on; untick **Terrain brush on** |
| "Warnings logged" box at exit | Harmless data warnings; `demoMode=true` in `user.cfg` hides it |
| Client slow to close (about 16–25 s) | Normal shutdown; not a hang |
| Terrain edits or fixes don't show | A different copy of the client was started; use the one in `exe\win32_new` |

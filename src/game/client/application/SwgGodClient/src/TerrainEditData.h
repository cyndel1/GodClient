// ======================================================================
//
// TerrainEditData.h
// In-game terrain editing for the God Client (raise / lower / flatten / level).
//
// Every brush stroke becomes one run-time terrain layer (BoundaryPolyline +
// AffectorHeightConstant) added on top of the planet's own rules, the same way
// the engine adds the flattening layers under player buildings.
//
// Thread safety: the client builds terrain chunks on a worker thread that reads
// the terrain generator outside any lock. So a layer is never changed once it is
// in the generator. While dragging, a complete replacement layer is built off to
// the side and swapped in (one remove + one add, as the engine does for
// buildings). Swapped-out and undone layers are only deleted once terrain
// generation has settled.
//
// ======================================================================

#ifndef INCLUDED_TerrainEditData_H
#define INCLUDED_TerrainEditData_H

// ======================================================================

#include "sharedMath/Rectangle2d.h"
#include "sharedMath/Vector2d.h"
#include "sharedTerrain/TerrainGenerator.h"

#include "sharedMath/Vector.h"

#include <map>
#include <set>
#include <string>
#include <vector>

class AppearanceTemplate;
class ClientProceduralTerrainAppearance;
class Object;
class PackedIntegerMap;
class VectorArgb;

// ======================================================================

class TerrainEditData
{
public:

	enum Tool
	{
		T_raise,
		T_lower,
		T_flatten,
		T_level,
		T_paint,        // stamp a ground texture (shader family)
		T_clearFlora,   // remove trees, rocks, bushes and grass
		T_roughen,      // add natural bumps from one of the planet's fractal patterns
		T_terrace,      // turn slopes into steps
		T_plantFlora,   // add bushes / rocks (static flora) or grass (radial flora)
		T_road,         // draw a path: follows the ground, smoothed, with a road texture
		T_river,        // draw a path: carves a channel and fills it with water
		T_water,        // flat water (lake / pond) at the Flatten/Level height, inside a shape
		T_smooth,       // soften bumps: the ground is sampled, blurred and baked when the drag ends
		T_ramp,         // drag from one end to the other: a straight slope between the ground heights there
		T_copy,         // drag a rectangle: copies the shape of the ground there to the stamp clipboard (no edit)
		T_stamp,        // click: stamps the clipboard (copied ground or a heightmap image) centred there
		T_scatter,      // paint server objects (the template selected in the Tree Browser) along the drag or in a shape
		T_waterfall     // drag from the top to the bottom: a sheet of fast water just above the ground
	};

	enum StrokeKind
	{
		SK_height,
		SK_paint,
		SK_flora,
		SK_roughen,
		SK_terrace,
		SK_plant,
		SK_road,
		SK_river,
		SK_water,
		SK_smooth,      // m_samples.points = (x, target height, z) per terrain pole; m_param2 = half patch size
		SK_tree,        // trees in the baked flora map: m_treeTiles; the layer is an empty marker
		SK_ramp,        // baked like SK_smooth: m_samples.points = (x, ramp height, z) per pole; m_param2 = half patch
		SK_copy,        // copy to the clipboard: never becomes an edit
		SK_stamp,       // baked like SK_smooth: the clipboard stamped at m_points [0]
		SK_scatter,     // server objects (m_scatter [strokeId]); the layer is an empty marker, never saved
		SK_waterfall    // a water ribbon along m_points with m_samples.waterHeights; m_radius = half width
	};

	//-- how a drag is turned into an area. Brush = along the drag; the others fill a shape
	//-- (the drag sets two corners, the centre and radius, or the outline). Roads/rivers are always paths.
	enum Shape
	{
		SH_brush,
		SH_rectangle,
		SH_circle,
		SH_polygon
	};

	//-- ground heights sampled along a road/river path before it was added (like SOE's Terrain
	//-- Editor "bake"), so the same path can be rebuilt identically for undo/redo/save
	struct PathSamples
	{
		std::vector<Vector> points;   // x, ground height, z
		std::vector<int>    counts;   // samples per path segment
		std::vector<float>  waterHeights;   // river / waterfall: water surface per path point (empty = river default)
	};

	enum PlantKind
	{
		PK_static,      // FloraGroup family: bushes, rocks (static non-collidable flora)
		PK_radial,      // RadialGroup family: grass and small plants (dynamic near flora)
		PK_tree         // FloraGroup family placed in the .trn's baked tree map (one per 16 m tile)
	};

	//-- Smart paint: optional limits for Paint, Plant flora (trees too) and Clear flora. The stroke
	//-- only acts where the ground passes every limit that is on. Paint / plant / clear use the
	//-- engine's own layer filters (saved in the .trn); trees are checked once, when planted.
	struct Limits
	{
		bool  m_slopeOn;
		float m_slopeMin;       // degrees from flat, 0..90
		float m_slopeMax;
		bool  m_heightOn;
		float m_heightLow;      // metres
		float m_heightHigh;
		int   m_onTexture;      // shader family id; 0 = any texture

		Limits () : m_slopeOn (false), m_slopeMin (30.0f), m_slopeMax (90.0f), m_heightOn (false), m_heightLow (0.0f), m_heightHigh (100.0f), m_onTexture (0) {}
		bool any () const { return m_slopeOn || m_heightOn || m_onTexture > 0; }
	};

	enum Status
	{
		S_ok,
		S_noTerrain,          // not on a ground planet yet
		S_singleThreaded      // terrain rebuilds need [ClientTerrain] terrainMultiThreaded=true
	};

public:

	static TerrainEditData & getInstance ();

	//-- brush settings
	void        setTool (Tool tool);
	Tool        getTool () const;
	void        setRadius (float radius);
	float       getRadius () const;
	void        setFeather (float feather);   // 0..1, soft edge as a fraction of the radius
	float       getFeather () const;
	void        setStrength (float strength); // metres, for raise / lower
	float       getStrength () const;
	void        setLevelHeight (float height);    // also used as the sticky Flatten height
	float       getLevelHeight () const;
	bool        isFlattenHeightSet () const;      // false until the first Flatten stroke (or a pick) sets it
	void        setPaintFamily (int familyId);    // shader family used by the Paint tool (0 = none chosen)
	int         getPaintFamily () const;

	//-- the current planet's ground textures (shader families), for the Paint picker
	int         getNumberOfShaderFamilies ();
	int         getShaderFamilyId (int index);
	const char *getShaderFamilyName (int familyId);

	//-- Roughen: one of the planet's fractal patterns, and how many metres of bumps
	int         getNumberOfFractalFamilies ();
	int         getFractalFamilyId (int index);
	const char *getFractalFamilyName (int familyId);
	void        setRoughenFamily (int familyId);
	int         getRoughenFamily () const;
	void        setRoughness (float metres);
	float       getRoughness () const;

	//-- Terrace: step height and the flat part of each step (0..1)
	void        setTerraceStep (float metres);
	float       getTerraceStep () const;
	void        setTerraceFlat (float fraction);
	float       getTerraceFlat () const;

	//-- Plant flora: static families first, then radial families
	int         getNumberOfPlantChoices ();
	bool        getPlantChoice (int index, PlantKind & kind, int & familyId);
	const char *getPlantName (PlantKind kind, int familyId);
	void        setPlant (PlantKind kind, int familyId);
	PlantKind   getPlantKind () const;
	int         getPlantFamily () const;
	void        setPlantDensity (float density);   // 0..1
	float       getPlantDensity () const;

	//-- River: channel depth below the bank (metres); water sits about 30% of the way down
	//-- Smooth: how far each stroke moves the ground towards the blurred ground (0..1)
	void        setSmoothStrength (float fraction);
	float       getSmoothStrength () const;

	void        setRiverDepth (float metres);
	float       getRiverDepth () const;

	void        setShape (Shape shape);
	Shape       getShape () const;

	//-- Flora scatter, whole planet: 0 = the planet's plants stay near the middle of their grid
	//-- cells (a grid look), 100 = anywhere in the cell. Bushes/rocks and grass. Saved in the .trn.
	int         getFloraScatter ();            // -1 = not on a ground planet
	void        setFloraScatter (int percent);

	//-- Smart paint limits (used by Paint, Plant flora and Clear flora)
	void          setLimits (const Limits & limits);
	const Limits &getLimits () const;
	static bool   toolUsesLimits (Tool tool);

	void        setBrushMode (bool on);
	bool        isBrushMode () const;

	//-- strokes, called by the game window
	bool        beginStroke (const Vector2d & position_w, bool alternate);
	void        continueStroke (const Vector2d & position_w);
	void        endStroke ();
	bool        isStrokeActive () const;

	void        setCursor (const Vector2d & position_w);
	bool        sampleCursorHeight (float & height);   // expensive: call a few times a second at most
	bool        sampleCursorGround (float & height, float & slopeDegrees, int & shaderFamily);   // as the Limits see it

	//-- draws the brush circle, the shape being dragged and the stamp footprint as lines on the
	//-- ground. Called every frame from the God Client's 3D drawing (vertex-colour shader, identity transform set).
	void        drawOverlay ();

	//-- Flatten under selection: levels the ground under each selected object to its base height,
	//-- over its footprint plus a 1 m margin, with a soft rim as wide as the brush size. One edit per
	//-- object. Returns the number of edits made; the message line says what happened.
	int         flattenUnderSelection ();

	//-- Object scatter: server objects of the template selected in the Tree Browser (ServerTemplates),
	//-- placed at Object spacing x Plant density, snapped to the ground, at most 250 per stroke
	void        setScatterSpacing (float metres);
	float       getScatterSpacing () const;
	void        setScatterRandomTurn (bool on);
	bool        getScatterRandomTurn () const;

	//-- Waterfall: the water look ("" = the planet's own water) and whether to flip the flow direction
	void        setWaterfallShader (const std::string & shaderTemplate);
	const std::string & getWaterfallShader () const;
	void        setWaterfallFlip (bool flip);
	bool        getWaterfallFlip () const;
	static std::string getScatterTemplate ();   // "" = none selected

	//-- Stamp clipboard: the shape of copied ground (heights relative to the copied area's edge), or a
	//-- greyscale heightmap image (values 0..1, scaled by the image height). Stamps sit on the ground:
	//-- the stamp's edge meets the average ground along the stamp's outline.
	bool        hasClipboard () const;
	std::string describeClipboard () const;
	void        setClipboardFromImage (const char * name, int width, int height, const std::vector<float> & values01);
	void        setStampRotation (float degrees);
	float       getStampRotation () const;
	void        setStampMirror (bool mirror);
	bool        getStampMirror () const;
	void        setStampHeightScale (float scale);   // 1 = as copied / as the image; negative = upside down
	float       getStampHeightScale () const;
	void        setImageSize (float metres);         // heightmap images: width of the stamp
	float       getImageSize () const;
	void        setImageHeight (float metres);       // heightmap images: height of white
	float       getImageHeight () const;

	//-- history
	bool        undo ();
	bool        redo ();
	void        removeAll ();
	int         getNumberOfEdits () const;
	const char *getEditName (int index) const;
	int         getNumberOfRedos () const;

	//-- housekeeping, call a few times a second
	void        tick ();

	Status      getStatus ();
	int         getRevision () const;             // changes whenever the edit list changes

	//-- diagnostics: measures the generated ground along the newest stroke and writes
	//-- details to terrain_edit_log.txt in the client folder. Returns a short summary.
	void        diagnoseLastEdit (char * summary, int summarySize);
	void        logStall (unsigned long milliseconds);   // called by the panel when its timer fires late
	void        logEvent (const char * format, ...) const;   // timestamped line in terrain_edit_log.txt
	bool        takeMessage (std::string & message);         // a note for the panel since the last call

	//-- saving. Every save writes: the planet's .trn as it was when this session first saved
	//-- (kept in memory) + the session's current strokes. Building-flattening layers that the
	//-- engine adds while playing are never written. Messages describe what happened.
	bool        save (char * message, int messageSize);                              // overwrite the planet's .trn (backed up first)
	bool        saveCopyTo (const char * folder, char * message, int messageSize);   // writes <folder>/<terrain/name.trn>
	bool        revertToSaved ();                                                    // reload the planet from disk
	bool        hasUnsavedEdits () const;
	bool        getTerrainFileName (char * buffer, int bufferSize);
	bool        getDefaultSavePath (char * buffer, int bufferSize, bool & existsOnDisk);

private:

	typedef std::vector<Vector2d>                  PointList;

	struct Edit
	{
		TerrainGenerator::Layer * m_layer;
		Rectangle2d               m_extent;
		PointList                 m_points;       // the stroke path (also used for saving)
		StrokeKind                m_kind;
		TerrainGeneratorOperation m_operation;    // height strokes
		float                     m_height;       // height strokes
		int                       m_familyId;     // paint strokes
		float                     m_radius;
		float                     m_feather;
		int                       m_strokeId;     // unique id; flora strokes use it for their clear-flora keys
		float                     m_param;        // roughen: metres; terrace: step; plant: density
		float                     m_param2;       // terrace: flat fraction
		int                       m_subKind;      // plant: PlantKind
		PathSamples               m_samples;      // road / river
		std::string               m_waterShader;  // river, water
		Shape                     m_shape;        // SH_brush: m_points is the path; otherwise the closed outline
		PointList                 m_floraPoints;  // flora strokes: centres of the clear-flora circles
		float                     m_floraRadius;
		std::vector<int>          m_treeTiles;       // tree strokes: baked-map tile index (z * width + x), all empty before
		std::vector<float>        m_treeHeights;     // tree strokes: baked tree height per tile
		std::vector<float>        m_treeOldHeights;  // tree strokes: the tile's baked height before
		Limits                    m_limits;          // Smart paint limits (paint, plant, flora, tree)
	};

	typedef std::vector<Edit>                      EditList;
	typedef std::vector<TerrainGenerator::Layer *> LayerList;

	void        logStroke (const Edit & edit) const;

	static TerrainGenerator::Layer * createLayer (const char * name, const PointList & points, StrokeKind kind, TerrainGeneratorOperation operation, float height, int familyId, float radius, float feather, float param, float param2, int subKind, const PathSamples & samples, const char * waterShader, Shape shape, const Limits & limits);
	static TerrainGenerator::Layer * addLimitLayers (TerrainGenerator::Layer * layer, const Limits & limits);
	static bool                      kindUsesLimits (StrokeKind kind);
	bool                             sampleGround (const Vector2d & position_w, float & height, float & slopeDegrees, int & shaderFamily);
	bool                             passesLimits (const Vector2d & position_w, const Limits & limits);
	void                             bakeLimitedClear ();
	static std::string               describeLimits (const Limits & limits);
	static PointList                 buildOutline (Shape shape, const PointList & dragPoints);
	static PointList                 coverPoints (const PointList & outline, float & radius);
	static bool                      isInsidePolygon (const Vector2d & point, const PointList & outline);
	void                             refreshSwimWater ();
	bool                             bakeSmooth ();
	bool                             bakeRamp ();
	void                             copyGround ();
	bool                             bakeScatter ();
	bool                             bakeWaterfall ();
	bool                             existingWaterHeight (const Vector2d & position_w, float ground, float & waterHeight) const;   // water already there, at or above the ground
	static TerrainGenerator::Layer * createWaterfallLayer (const char * name, const PointList & points, float radius, const PathSamples & samples, const char * waterShader, bool flipFlow);
	void                             createScatterObjects (int strokeId);
	void                             removeScatterObjects (int strokeId);
	void                             updateScatter ();
	bool                             bakeStamp ();
	float                            sampleClipboard (float u, float v) const;   // u, v in grid cells; bilinear
	void                             drawGroundLine (const PointList & points, bool closed, const VectorArgb & colour);
	PointList                        stampCorners (const Vector2d & centre) const;
	bool                             sampleGroundGrid (const Rectangle2d & area, int margin, float & spacing, int & gx0, int & gz0, int & nx, int & nz, std::vector<float> & ground);
	bool                             loadTreeMaps ();
	void                             resnapTrees (const Rectangle2d & area);   // baked tree heights := current ground
	static bool                      changesHeight (StrokeKind kind);
	bool                             bakeTrees ();
	void                             setTreeTiles (const std::vector<int> & tiles, int familyId, const std::vector<float> & heights);
	static TerrainGenerator::Layer * createSmoothLayer (const char * name, const PathSamples & samples, float halfPatch);
	static TerrainGenerator::Layer * createPathLayer (const char * name, const PointList & points, StrokeKind kind, int familyId, float radius, float feather, float depth, const PathSamples & samples, const char * waterShader);
	static PointList                 smoothPath (const PointList & points, float spacing, int passes);
	void                             samplePath (const PointList & points, PathSamples & samples);
	std::string                      findWaterShader ();
	const TerrainGenerator *         getGenerator ();

	//-- trees and rocks on most planets come from the .trn's baked flora map, not from terrain
	//-- rules, so Clear flora also uses the engine's run-time "clear collidable flora" circles
	void        addFloraClearing (int strokeId, const PointList & points, int firstPoint, float radius);
	void        removeFloraClearing (int strokeId, int numberOfPoints);
	void        refreshRadialFlora ();
	void        diagnoseSurfaceEdit (const Edit & edit, char * summary, int summarySize);
	PackedIntegerMap * buildClearedFloraMap (const PackedIntegerMap & original) const;
	bool        loadOriginal (char * message, int messageSize);
	bool        writeTerrain (const char * diskPath, char * message, int messageSize);

private:


	TerrainEditData ();
	~TerrainEditData ();
	TerrainEditData (const TerrainEditData &);
	TerrainEditData & operator= (const TerrainEditData &);

	ClientProceduralTerrainAppearance * getAppearance ();
	TerrainGenerator::Layer *           buildStrokeLayer () const;
	void                                swapStrokeLayer (bool force);
	void                                addToTerrain (TerrainGenerator::Layer * layer, const Rectangle2d & dirty);
	void                                removeFromTerrain (TerrainGenerator::Layer * layer, const Rectangle2d & dirty);
	void                                retire (TerrainGenerator::Layer * layer);
	void                                markDirty (const Rectangle2d & area);
	void                                flushDirty ();
	void                                updateIdle ();
	bool                                isTerrainIdle () const;
	void                                clearRedo ();
	static Rectangle2d                  pointExtent (const PointList & points, int first, float radius);

private:

	//-- settings
	Tool        m_tool;
	float       m_radius;
	float       m_feather;
	float       m_strength;
	float       m_levelHeight;
	bool        m_flattenNeedsSample;
	int         m_paintFamilyId;
	int         m_roughenFamilyId;
	float       m_roughness;
	float       m_terraceStep;
	float       m_terraceFlat;
	PlantKind   m_plantKind;
	int         m_plantFamilyId;
	float       m_plantDensity;
	float       m_riverDepth;
	float       m_smoothStrength;
	std::string m_waterShader;      // water shader used by River strokes (found on the planet)
	Shape       m_shape;
	Limits      m_limits;

	//-- stamp clipboard
	struct Clipboard
	{
		bool               m_valid;
		bool               m_isImage;
		std::string        m_name;
		int                m_width;      // grid points
		int                m_height;
		float              m_sizeX;      // metres (copied ground; images use m_imageSize)
		float              m_sizeZ;
		std::vector<float> m_values;     // copied: metres above the copied edge; image: 0..1
		Clipboard () : m_valid (false), m_isImage (false), m_name (), m_width (0), m_height (0), m_sizeX (0.0f), m_sizeZ (0.0f), m_values () {}
	};
	Clipboard   m_clipboard;

	//-- object scatter. The server creates objects from a console command and sends them back
	//-- later without saying which command made them, so each placement is matched to the object
	//-- that appears at its spot (same template, within 0.4 m) to learn its id for Undo.
	struct ScatterItem
	{
		std::string   m_serverTemplate;
		std::string   m_sharedTemplate;
		Vector        m_position;
		float         m_yaw;
		std::string   m_id;              // network id once seen ("" = not seen yet)
		bool          m_live;            // created and not undone
		bool          m_deleteWhenSeen;  // undone before the server sent it back
		unsigned long m_createdAt;
		ScatterItem () : m_serverTemplate (), m_sharedTemplate (), m_position (), m_yaw (0.0f), m_id (), m_live (false), m_deleteWhenSeen (false), m_createdAt (0) {}
	};
	typedef std::vector<ScatterItem>          ScatterList;
	std::map<int, ScatterList>                m_scatter;          // by stroke id
	ScatterList                               m_strokeScatter;
	std::set<int>                             m_scatterWarned;    // strokes already reported as "nothing came back"
	float       m_scatterSpacing;
	std::string m_waterfallShader;
	bool        m_waterfallFlip;
	std::string m_strokeWaterfallShader;
	bool        m_scatterRandomTurn;
	unsigned long m_lastScatterScan;
	float       m_stampRotation;
	bool        m_stampMirror;
	float       m_stampHeightScale;
	float       m_imageSize;
	float       m_imageHeight;
	std::string m_message;          // for the panel (takeMessage)
	bool        m_brushMode;

	//-- the terrain the edits belong to
	ClientProceduralTerrainAppearance *         m_appearance;
	const AppearanceTemplate *          m_appearanceTemplate;

	//-- current stroke
	bool                      m_strokeActive;
	bool                      m_strokeInverted;
	StrokeKind                m_strokeKind;
	int                       m_strokeFamilyId;
	std::string               m_strokeFamilyName;
	float                     m_strokeParam;
	float                     m_strokeParam2;
	int                       m_strokeSubKind;
	PathSamples               m_strokeSamples;
	int                       m_strokeFloraPointsCleared;   // flora strokes: points already given clear circles
	Shape                     m_strokeShape;
	bool                      m_strokeOutlineReady;   // shape strokes made in code: m_strokePoints is already the outline
	std::string               m_strokeNameOverride;   // edit-list name for strokes made in code
	Limits                    m_strokeLimits;
	std::vector<int>          m_strokeTreeTiles;
	std::vector<float>        m_strokeTreeHeights;
	std::vector<float>        m_strokeTreeOldHeights;

	//-- the baked tree maps of the current terrain, unpacked (loaded on first use, kept in step
	//-- with the template's maps, which only this tool changes)
	int                       m_treeMapWidth;      // 0 = not loaded
	std::vector<int>          m_treeFamilies;
	std::vector<float>        m_treeHeightMap;
	std::vector<int>          m_bakedTreeList;     // tree families used in the baked map, for the Plant picker
	bool                      m_bakedTreeListBuilt;
	PointList                 m_strokeFloraPoints;          // shape flora strokes: clear-flora circle centres
	float                     m_strokeFloraRadius;
	float                     m_strokeHeight;
	TerrainGeneratorOperation m_strokeOperation;
	PointList                 m_strokePoints;
	int                       m_strokePointsShown;   // points included in the live layer
	TerrainGenerator::Layer * m_strokeLayer;
	unsigned long             m_lastSwapTime;

	//-- history
	EditList    m_edits;          // in the terrain, oldest first
	EditList    m_redo;           // undone, not in the terrain, most recent last
	LayerList   m_graveyard;      // out of the terrain, waiting to be deleted safely
	unsigned long m_lastTerrainChangeTime;

	//-- area waiting to be rebuilt on screen. The engine's invalidateRegion throws away pending
	//-- and just-built chunks (which then stay missing until the camera view changes), so the
	//-- rebuild is only requested while terrain generation is idle, one merged area at a time.
	Rectangle2d m_pendingDirty;
	bool        m_hasPendingDirty;
	unsigned long m_lastFlushTime;

	//-- diagnostics: is each requested rebuild actually carried out? (watches the on-screen
	//-- chunk under the middle of the rebuilt area and logs when it is replaced)
	unsigned long m_pendingSince;
	bool          m_pendingWaitLogged;
	bool          m_watching;
	const void *  m_watchChunk;
	Vector2d      m_watchPoint;
	unsigned long m_watchTime;

	//-- when the terrain worker last became idle (0 = busy). Growing the engine's layer list
	//-- frees its old memory block while the worker may be reading it, so routine layer swaps
	//-- only happen while the worker is idle.
	unsigned long m_idleSince;
	int         m_nameCounter;
	int         m_revision;

	//-- cursor
	Vector2d    m_cursor;
	bool        m_hasCursor;

	//-- diagnostics: what the tool last did, for stall reports
	const char *  m_lastAction;
	unsigned long m_lastActionTime;
	int           m_lastActionCount;

	//-- grass, bushes and small rocks: one engine "clear non-collidable flora" object per stroke point
	struct RadialClear
	{
		Object *                                 m_object;
		std::vector<std::pair<Vector, float> > * m_list;
	};
	typedef std::map<int, RadialClear> RadialClearMap;
	RadialClearMap m_radialClears;

	//-- saving
	TerrainGenerator * m_originalGenerator;   // the planet's rules as on disk at the first save (no run-time layers)
	bool        m_unsaved;
	bool        m_backupDone;
};

// ======================================================================

#endif

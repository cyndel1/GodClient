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

#include <vector>

class AppearanceTemplate;
class ClientProceduralTerrainAppearance;

// ======================================================================

class TerrainEditData
{
public:

	enum Tool
	{
		T_raise,
		T_lower,
		T_flatten,
		T_level
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

	void        setBrushMode (bool on);
	bool        isBrushMode () const;

	//-- strokes, called by the game window
	bool        beginStroke (const Vector2d & position_w, bool alternate);
	void        continueStroke (const Vector2d & position_w);
	void        endStroke ();
	bool        isStrokeActive () const;

	void        setCursor (const Vector2d & position_w);
	bool        sampleCursorHeight (float & height);   // expensive: call a few times a second at most

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
		PointList                 m_points;       // the stroke path, for diagnostics
		TerrainGeneratorOperation m_operation;
		float                     m_height;
		float                     m_radius;
		float                     m_feather;
	};

	typedef std::vector<Edit>                      EditList;
	typedef std::vector<TerrainGenerator::Layer *> LayerList;

	void        logStroke (const Edit & edit) const;

	static TerrainGenerator::Layer * createLayer (const char * name, const PointList & points, TerrainGeneratorOperation operation, float height, float radius, float feather);
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
	bool        m_brushMode;

	//-- the terrain the edits belong to
	ClientProceduralTerrainAppearance *         m_appearance;
	const AppearanceTemplate *          m_appearanceTemplate;

	//-- current stroke
	bool                      m_strokeActive;
	bool                      m_strokeInverted;
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

	//-- when the terrain worker last became idle (0 = busy). Growing the engine's layer list
	//-- frees its old memory block while the worker may be reading it, so routine layer swaps
	//-- only happen while the worker is idle.
	unsigned long m_idleSince;
	int         m_nameCounter;
	int         m_revision;

	//-- cursor
	Vector2d    m_cursor;
	bool        m_hasCursor;

	//-- saving
	TerrainGenerator * m_originalGenerator;   // the planet's rules as on disk at the first save (no run-time layers)
	bool        m_unsaved;
	bool        m_backupDone;
};

// ======================================================================

#endif

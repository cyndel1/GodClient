// ======================================================================
//
// TerrainEditData.cpp
// In-game terrain editing for the God Client. See TerrainEditData.h.
//
// ======================================================================

#include "SwgGodClient/FirstSwgGodClient.h"
#include "TerrainEditData.h"

#include "clientGame/Game.h"
#include "clientGame/GroundScene.h"
#include "clientTerrain/ClientProceduralTerrainAppearance.h"
#include "clientTerrain/ClientRadialFloraManager.h"
#include "sharedObject/Object.h"
#include "sharedFile/Iff.h"
#include "sharedFile/TreeFile.h"
#include "sharedFoundation/Os.h"
#include "sharedRandom/RandomGenerator.h"
#include "sharedTerrain/AffectorColor.h"
#include "sharedTerrain/AffectorFloraDynamic.h"
#include "sharedTerrain/AffectorFloraStatic.h"
#include "sharedTerrain/AffectorHeight.h"
#include "sharedTerrain/AffectorRibbon.h"
#include "sharedTerrain/AffectorRiver.h"
#include "sharedTerrain/AffectorRoad.h"
#include "sharedTerrain/AffectorShader.h"
#include "sharedTerrain/Boundary.h"
#include "sharedTerrain/ShaderGroup.h"
#include "sharedUtility/PackedIntegerMap.h"
#include "sharedTerrain/ProceduralTerrainAppearance.h"
#include "sharedTerrain/ProceduralTerrainAppearanceTemplate.h"
#include "sharedTerrain/TerrainObject.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

// ======================================================================

namespace TerrainEditDataNamespace
{
	const unsigned long cs_swapIntervalMs     = 250;    // live preview updates while dragging
	const unsigned long cs_minimumFlushGapMs  = 400;    // minimum time between on-screen rebuild requests
	const unsigned long cs_idleConfirmMs      = 150;    // worker must look idle this long before layers are swapped
	const unsigned long cs_settleDelayMs      = 3000;   // wait this long before deleting retired layers
	const int           cs_maximumStrokePoints = 400;   // a longer drag is split into more strokes
	const float         cs_minimumRadius      = 1.0f;
	const float         cs_maximumRadius      = 2048.0f;

	unsigned long now ()
	{
		return ::GetTickCount ();
	}

	const char * toolName (TerrainEditData::Tool tool, bool inverted)
	{
		switch (tool)
		{
		case TerrainEditData::T_raise:      return inverted ? "Lower" : "Raise";
		case TerrainEditData::T_lower:      return inverted ? "Raise" : "Lower";
		case TerrainEditData::T_flatten:    return "Flatten";
		case TerrainEditData::T_level:      return "Level";
		case TerrainEditData::T_paint:      return "Paint";
		case TerrainEditData::T_clearFlora: return "Clear flora";
		case TerrainEditData::T_roughen:    return "Roughen";
		case TerrainEditData::T_terrace:    return "Terrace";
		case TerrainEditData::T_plantFlora: return "Plant";
		case TerrainEditData::T_road:       return "Road";
		case TerrainEditData::T_river:      return "River";
		case TerrainEditData::T_water:      return "Water level";
		case TerrainEditData::T_smooth:     return "Smooth";
		default:                            return "Edit";
		}
	}

	//-- opaque keys for the engine's clear-collidable-flora map (it only uses them as map keys,
	//-- never dereferences them): one per stroke point, unique per stroke
	const Object * floraClearKey (const int strokeId, const int pointIndex)
	{
		const unsigned value = 0x40000000u + (static_cast<unsigned> (strokeId & 0xFFFFF) << 10) + static_cast<unsigned> (pointIndex & 0x3FF);
		return reinterpret_cast<const Object *> (static_cast<uintptr_t> (value));
	}

	float distanceSquaredToPolyline (const Vector2d & p, const std::vector<Vector2d> & points)
	{
		float best = FLT_MAX;
		const size_t n = points.size ();
		for (size_t i = 0; i < n; ++i)
		{
			best = std::min (best, p.magnitudeBetweenSquared (points [i]));
			if (i + 1 < n)
			{
				const Vector2d & a = points [i];
				const Vector2d & b = points [i + 1];
				const float dx = b.x - a.x;
				const float dy = b.y - a.y;
				const float lengthSquared = dx * dx + dy * dy;
				if (lengthSquared > 0.0001f)
				{
					const float u = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared;
					if (u > 0.0f && u < 1.0f)
					{
						const Vector2d closest (a.x + u * dx, a.y + u * dy);
						best = std::min (best, p.magnitudeBetweenSquared (closest));
					}
				}
			}
		}
		return best;
	}
}

using namespace TerrainEditDataNamespace;

// ======================================================================

TerrainEditData & TerrainEditData::getInstance ()
{
	static TerrainEditData s_instance;
	return s_instance;
}

// ----------------------------------------------------------------------

TerrainEditData::TerrainEditData () :
	m_tool (T_raise),
	m_radius (16.0f),
	m_feather (0.5f),
	m_strength (2.0f),
	m_levelHeight (0.0f),
	m_flattenNeedsSample (true),
	m_paintFamilyId (0),
	m_roughenFamilyId (0),
	m_roughness (4.0f),
	m_terraceStep (4.0f),
	m_terraceFlat (0.5f),
	m_plantKind (PK_static),
	m_plantFamilyId (0),
	m_plantDensity (0.5f),
	m_riverDepth (3.0f),
	m_smoothStrength (1.0f),
	m_waterShader (),
	m_shape (SH_brush),
	m_brushMode (false),
	m_appearance (0),
	m_appearanceTemplate (0),
	m_strokeActive (false),
	m_strokeInverted (false),
	m_strokeKind (SK_height),
	m_strokeFamilyId (0),
	m_strokeFamilyName (),
	m_strokeParam (0.0f),
	m_strokeParam2 (0.0f),
	m_strokeSubKind (0),
	m_strokeSamples (),
	m_strokeFloraPointsCleared (0),
	m_strokeShape (SH_brush),
	m_strokeTreeTiles (),
	m_strokeTreeHeights (),
	m_strokeTreeOldHeights (),
	m_treeMapWidth (0),
	m_treeFamilies (),
	m_treeHeightMap (),
	m_bakedTreeList (),
	m_bakedTreeListBuilt (false),
	m_strokeFloraPoints (),
	m_strokeFloraRadius (0.0f),
	m_strokeHeight (0.0f),
	m_strokeOperation (TGO_add),
	m_strokePoints (),
	m_strokePointsShown (0),
	m_strokeLayer (0),
	m_lastSwapTime (0),
	m_edits (),
	m_redo (),
	m_graveyard (),
	m_lastTerrainChangeTime (0),
	m_pendingDirty (),
	m_hasPendingDirty (false),
	m_lastFlushTime (0),
	m_pendingSince (0),
	m_pendingWaitLogged (false),
	m_watching (false),
	m_watchChunk (0),
	m_watchPoint (),
	m_watchTime (0),
	m_idleSince (0),
	m_nameCounter (0),
	m_revision (0),
	m_cursor (),
	m_hasCursor (false),
	m_lastAction ("none"),
	m_lastActionTime (0),
	m_lastActionCount (0),
	m_originalGenerator (0),
	m_unsaved (false),
	m_backupDone (false)
{
}

// ----------------------------------------------------------------------

TerrainEditData::~TerrainEditData ()
{
	//-- the process is exiting; the terrain may already be gone, so leave layers alone
}

// ======================================================================
// settings
// ======================================================================

void TerrainEditData::setTool (const Tool tool)
{
	//-- choosing Flatten (again) means: the next stroke picks a fresh height
	if (tool == T_flatten)
		m_flattenNeedsSample = true;

	m_tool = tool;
}

bool TerrainEditData::isFlattenHeightSet () const      { return !m_flattenNeedsSample; }
void TerrainEditData::setShape (const Shape shape)      { m_shape = shape; }
void  TerrainEditData::setSmoothStrength (const float f) { m_smoothStrength = std::max (0.05f, std::min (1.0f, f)); }
float TerrainEditData::getSmoothStrength () const        { return m_smoothStrength; }

bool TerrainEditData::takeMessage (std::string & message)
{
	if (m_message.empty ())
		return false;
	message.swap (m_message);
	m_message.clear ();
	return true;
}
TerrainEditData::Shape TerrainEditData::getShape () const { return m_shape; }
void TerrainEditData::setPaintFamily (const int id)     { m_paintFamilyId = id; }
int  TerrainEditData::getPaintFamily () const           { return m_paintFamilyId; }

// ----------------------------------------------------------------------

int TerrainEditData::getNumberOfShaderFamilies ()
{
	ClientProceduralTerrainAppearance * const appearance = getAppearance ();
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (appearance->getAppearanceTemplate ()) : 0;
	return terrainTemplate && terrainTemplate->getTerrainGenerator () ? terrainTemplate->getTerrainGenerator ()->getShaderGroup ().getNumberOfFamilies () : 0;
}

int TerrainEditData::getShaderFamilyId (const int index)
{
	ClientProceduralTerrainAppearance * const appearance = getAppearance ();
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (appearance->getAppearanceTemplate ()) : 0;
	return terrainTemplate && terrainTemplate->getTerrainGenerator () ? terrainTemplate->getTerrainGenerator ()->getShaderGroup ().getFamilyId (index) : 0;
}

const char * TerrainEditData::getShaderFamilyName (const int familyId)
{
	ClientProceduralTerrainAppearance * const appearance = getAppearance ();
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (appearance->getAppearanceTemplate ()) : 0;
	if (!terrainTemplate || !terrainTemplate->getTerrainGenerator ())
		return "";
	const ShaderGroup & shaderGroup = terrainTemplate->getTerrainGenerator ()->getShaderGroup ();
	if (!shaderGroup.hasFamily (familyId))
		return "";
	const char * const name = shaderGroup.getFamilyName (familyId);
	return name ? name : "";
}
// ----------------------------------------------------------------------

const TerrainGenerator * TerrainEditData::getGenerator ()
{
	ClientProceduralTerrainAppearance * const appearance = getAppearance ();
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (appearance->getAppearanceTemplate ()) : 0;
	return terrainTemplate ? terrainTemplate->getTerrainGenerator () : 0;
}

int TerrainEditData::getNumberOfFractalFamilies ()
{
	const TerrainGenerator * const generator = getGenerator ();
	return generator ? generator->getFractalGroup ().getNumberOfFamilies () : 0;
}

int TerrainEditData::getFractalFamilyId (const int index)
{
	const TerrainGenerator * const generator = getGenerator ();
	return generator ? generator->getFractalGroup ().getFamilyId (index) : 0;
}

const char * TerrainEditData::getFractalFamilyName (const int familyId)
{
	const TerrainGenerator * const generator = getGenerator ();
	if (!generator || !generator->getFractalGroup ().hasFamily (familyId))
		return "";
	const char * const name = generator->getFractalGroup ().getFamilyName (familyId);
	return name ? name : "";
}

void  TerrainEditData::setRoughenFamily (const int id)  { m_roughenFamilyId = id; }
int   TerrainEditData::getRoughenFamily () const        { return m_roughenFamilyId; }
void  TerrainEditData::setRoughness (const float m)     { m_roughness = std::max (0.0f, std::min (500.0f, m)); }
float TerrainEditData::getRoughness () const            { return m_roughness; }
void  TerrainEditData::setTerraceStep (const float m)   { m_terraceStep = std::max (0.1f, std::min (500.0f, m)); }
float TerrainEditData::getTerraceStep () const          { return m_terraceStep; }
void  TerrainEditData::setTerraceFlat (const float f)   { m_terraceFlat = std::max (0.0f, std::min (0.95f, f)); }
float TerrainEditData::getTerraceFlat () const          { return m_terraceFlat; }

int TerrainEditData::getNumberOfPlantChoices ()
{
	const TerrainGenerator * const generator = getGenerator ();
	if (!generator)
		return 0;

	//-- trees: the families already used in the baked tree map (found once per terrain)
	if (!m_bakedTreeListBuilt && loadTreeMaps ())
	{
		m_bakedTreeListBuilt = true;
		std::map<int, int> used;
		for (std::vector<int>::const_iterator i = m_treeFamilies.begin (); i != m_treeFamilies.end (); ++i)
			if (*i != 0)
				++used [*i];
		for (std::map<int, int>::const_iterator j = used.begin (); j != used.end (); ++j)
			if (generator->getFloraGroup ().hasFamily (j->first))
				m_bakedTreeList.push_back (j->first);
		logEvent ("TREES baked map %d x %d: %d tree families in use", m_treeMapWidth, m_treeMapWidth, static_cast<int> (m_bakedTreeList.size ()));
	}

	return generator->getFloraGroup ().getNumberOfFamilies () + generator->getRadialGroup ().getNumberOfFamilies () + static_cast<int> (m_bakedTreeList.size ());
}

bool TerrainEditData::getPlantChoice (const int index, PlantKind & kind, int & familyId)
{
	const TerrainGenerator * const generator = getGenerator ();
	if (!generator || index < 0)
		return false;

	const int numberOfStatic = generator->getFloraGroup ().getNumberOfFamilies ();
	if (index < numberOfStatic)
	{
		kind     = PK_static;
		familyId = generator->getFloraGroup ().getFamilyId (index);
		return true;
	}

	const int numberOfRadial = generator->getRadialGroup ().getNumberOfFamilies ();
	if (index - numberOfStatic < numberOfRadial)
	{
		kind     = PK_radial;
		familyId = generator->getRadialGroup ().getFamilyId (index - numberOfStatic);
		return true;
	}

	const int treeIndex = index - numberOfStatic - numberOfRadial;
	if (treeIndex < static_cast<int> (m_bakedTreeList.size ()))
	{
		kind     = PK_tree;
		familyId = m_bakedTreeList [static_cast<size_t> (treeIndex)];
		return true;
	}

	return false;
}

const char * TerrainEditData::getPlantName (const PlantKind kind, const int familyId)
{
	const TerrainGenerator * const generator = getGenerator ();
	if (!generator)
		return "";

	const char * name = 0;
	if (kind == PK_static || kind == PK_tree)
		name = generator->getFloraGroup ().hasFamily (familyId) ? generator->getFloraGroup ().getFamilyName (familyId) : 0;
	else
		name = generator->getRadialGroup ().hasFamily (familyId) ? generator->getRadialGroup ().getFamilyName (familyId) : 0;
	return name ? name : "";
}

void TerrainEditData::setPlant (const PlantKind kind, const int familyId)
{
	m_plantKind     = kind;
	m_plantFamilyId = familyId;
}

TerrainEditData::PlantKind TerrainEditData::getPlantKind () const { return m_plantKind; }
int   TerrainEditData::getPlantFamily () const                    { return m_plantFamilyId; }
void  TerrainEditData::setPlantDensity (const float d)            { m_plantDensity = std::max (0.0f, std::min (1.0f, d)); }
float TerrainEditData::getPlantDensity () const                   { return m_plantDensity; }
void  TerrainEditData::setRiverDepth (const float m)              { m_riverDepth = std::max (0.5f, std::min (100.0f, m)); }
float TerrainEditData::getRiverDepth () const                     { return m_riverDepth; }

// ----------------------------------------------------------------------

TerrainEditData::Tool TerrainEditData::getTool () const { return m_tool; }

void TerrainEditData::setRadius (const float radius)
{
	m_radius = std::max (cs_minimumRadius, std::min (cs_maximumRadius, radius));
}

float TerrainEditData::getRadius () const              { return m_radius; }

void TerrainEditData::setFeather (const float feather)
{
	m_feather = std::max (0.0f, std::min (1.0f, feather));
}

float TerrainEditData::getFeather () const             { return m_feather; }
void  TerrainEditData::setStrength (const float s)     { m_strength = std::max (0.0f, s); }
float TerrainEditData::getStrength () const            { return m_strength; }
void TerrainEditData::setLevelHeight (const float h)
{
	m_levelHeight        = h;
	m_flattenNeedsSample = false;   // a typed height is used by Flatten too
}
float TerrainEditData::getLevelHeight () const         { return m_levelHeight; }

void TerrainEditData::setBrushMode (const bool on)
{
	if (!on && m_strokeActive)
		endStroke ();

	m_brushMode = on;
}

bool TerrainEditData::isBrushMode () const             { return m_brushMode; }
bool TerrainEditData::isStrokeActive () const          { return m_strokeActive; }
int  TerrainEditData::getRevision () const             { return m_revision; }
int  TerrainEditData::getNumberOfEdits () const        { return static_cast<int> (m_edits.size ()); }
int  TerrainEditData::getNumberOfRedos () const        { return static_cast<int> (m_redo.size ()); }

const char * TerrainEditData::getEditName (const int index) const
{
	if (index < 0 || index >= static_cast<int> (m_edits.size ()))
		return "";

	const char * const name = m_edits [static_cast<size_t> (index)].m_layer->getName ();
	return name ? name : "";
}

// ======================================================================
// terrain access
// ======================================================================

ClientProceduralTerrainAppearance * TerrainEditData::getAppearance ()
{
	TerrainObject * const terrainObject = TerrainObject::getInstance ();
	ClientProceduralTerrainAppearance * const appearance = terrainObject ? dynamic_cast<ClientProceduralTerrainAppearance *> (terrainObject->getAppearance ()) : 0;
	const AppearanceTemplate * const appearanceTemplate = appearance ? appearance->getAppearanceTemplate () : 0;

	if (appearance != m_appearance || appearanceTemplate != m_appearanceTemplate)
	{
		//-- the terrain was replaced (planet change or Reload Terrain). The old edit layers
		//-- belonged to the old terrain: forget them without deleting (the old terrain may
		//-- have deleted them already). Layers that are not in any terrain are ours to delete.
		m_strokeActive = false;
		m_strokeLayer  = 0;
		m_strokePoints.clear ();
		m_edits.clear ();

		for (EditList::iterator i = m_redo.begin (); i != m_redo.end (); ++i)
			delete i->m_layer;
		m_redo.clear ();

		for (LayerList::iterator j = m_graveyard.begin (); j != m_graveyard.end (); ++j)
			delete *j;
		m_graveyard.clear ();

		//-- the radial flora clear list is global (not per terrain), so take ours out of it
		for (RadialClearMap::iterator k = m_radialClears.begin (); k != m_radialClears.end (); ++k)
		{
			ClientRadialFloraManager::removeClearFloraObject (k->second.m_object);
			delete k->second.m_object;
			delete k->second.m_list;
		}
		m_radialClears.clear ();

		//-- the baked tree maps belong to the old terrain
		m_treeMapWidth       = 0;
		m_treeFamilies.clear ();
		m_treeHeightMap.clear ();
		m_bakedTreeList.clear ();
		m_bakedTreeListBuilt = false;

		m_appearance         = appearance;
		m_appearanceTemplate = appearanceTemplate;
		m_hasPendingDirty    = false;
		m_idleSince          = 0;

		//-- a new terrain was loaded (possibly the file we just saved): start a new save session
		delete m_originalGenerator;
		m_originalGenerator  = 0;
		m_unsaved            = false;
		m_backupDone         = false;
		++m_revision;
	}

	return appearance;
}

// ----------------------------------------------------------------------

TerrainEditData::Status TerrainEditData::getStatus ()
{
	if (!getAppearance ())
		return S_noTerrain;

	if (!ClientProceduralTerrainAppearance::getUseMultiThreadedTerrainGeneration ())
		return S_singleThreaded;

	return S_ok;
}

// ----------------------------------------------------------------------

void TerrainEditData::addToTerrain (TerrainGenerator::Layer * const layer, const Rectangle2d & dirty)
{
	ClientProceduralTerrainAppearance * const appearance = m_appearance;
	NOT_NULL (appearance);

	appearance->addLayer (layer);
	appearance->prepareGenerator ();
	layer->calculateExtent ();
	m_lastTerrainChangeTime = now ();
	markDirty (dirty);
	flushDirty ();
}

// ----------------------------------------------------------------------

void TerrainEditData::removeFromTerrain (TerrainGenerator::Layer * const layer, const Rectangle2d & dirty)
{
	ClientProceduralTerrainAppearance * const appearance = m_appearance;
	NOT_NULL (appearance);

	appearance->removeLayer (layer);
	appearance->prepareGenerator ();
	m_lastTerrainChangeTime = now ();
	markDirty (dirty);
	flushDirty ();
}

// ----------------------------------------------------------------------

void TerrainEditData::updateIdle ()
{
	//-- idle = no pending chunk requests, seen on checks at least cs_idleConfirmMs apart, which
	//-- also covers a chunk still being built just after the queue emptied
	if (m_appearance && m_appearance->terrainGenerationStabilized ())
	{
		if (m_idleSince == 0)
			m_idleSince = now ();
	}
	else
		m_idleSince = 0;
}

// ----------------------------------------------------------------------

bool TerrainEditData::isTerrainIdle () const
{
	return m_idleSince != 0 && now () - m_idleSince >= cs_idleConfirmMs;
}

// ----------------------------------------------------------------------

void TerrainEditData::markDirty (const Rectangle2d & area)
{
	if (m_hasPendingDirty)
		m_pendingDirty.expand (area);
	else
	{
		m_pendingDirty      = area;
		m_hasPendingDirty   = true;
		m_pendingSince      = now ();
		m_pendingWaitLogged = false;
	}
}

// ----------------------------------------------------------------------

void TerrainEditData::flushDirty ()
{
	//-- only ask for a rebuild while nothing is queued, so no requested chunk is thrown away.
	//-- The gap also covers the engine's short "clearing" stage, when its queue is empty but
	//-- the previous rebuild is still starting (its lock is private, so it can't be checked).
	if (!m_hasPendingDirty || !m_appearance)
		return;

	if (now () - m_lastFlushTime < cs_minimumFlushGapMs || !isTerrainIdle () || !m_appearance->terrainGenerationStabilized ())
	{
		if (!m_pendingWaitLogged && now () - m_pendingSince > 2000)
		{
			m_pendingWaitLogged = true;
			logEvent ("REBUILD WAITING %lu ms: idle=%s stabilized=%s sinceLastFlush=%lu ms", now () - m_pendingSince,
				isTerrainIdle () ? "yes" : "no", m_appearance->terrainGenerationStabilized () ? "yes" : "no", now () - m_lastFlushTime);
		}
		return;
	}

	//-- watch the on-screen chunk under the middle of the area: it is replaced when the rebuild lands
	m_watchPoint = Vector2d ((m_pendingDirty.x0 + m_pendingDirty.x1) * 0.5f, (m_pendingDirty.y0 + m_pendingDirty.y1) * 0.5f);
	m_watchChunk = m_appearance->getChunkExtent (Vector (m_watchPoint.x, 0.0f, m_watchPoint.y));
	m_watching   = true;
	m_watchTime  = now ();
	logEvent ("REBUILD REQUEST %.0f %.0f .. %.0f %.0f (waited %lu ms) chunk=%p", m_pendingDirty.x0, m_pendingDirty.y0, m_pendingDirty.x1, m_pendingDirty.y1,
		now () - m_pendingSince, m_watchChunk);

	m_appearance->invalidateRegion (m_pendingDirty);
	m_hasPendingDirty       = false;
	m_lastFlushTime         = now ();
	m_lastTerrainChangeTime = m_lastFlushTime;
	m_lastAction            = "rebuild request (invalidateRegion)";
	m_lastActionTime        = m_lastFlushTime;
	m_lastActionCount       = static_cast<int> ((m_pendingDirty.x1 - m_pendingDirty.x0) * (m_pendingDirty.y1 - m_pendingDirty.y0));   // area in square metres
}

// ----------------------------------------------------------------------

void TerrainEditData::retire (TerrainGenerator::Layer * const layer)
{
	if (layer)
		m_graveyard.push_back (layer);
}

// ----------------------------------------------------------------------

void TerrainEditData::clearRedo ()
{
	for (EditList::iterator i = m_redo.begin (); i != m_redo.end (); ++i)
		retire (i->m_layer);
	m_redo.clear ();
}

// ----------------------------------------------------------------------

Rectangle2d TerrainEditData::pointExtent (const PointList & points, const int first, const float radius)
{
	Rectangle2d extent (FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);

	//-- include the point before 'first' so the joining segment is covered
	const int start = std::max (0, first - 1);
	for (int i = start; i < static_cast<int> (points.size ()); ++i)
		extent.expand (points [static_cast<size_t> (i)]);

	extent.x0 -= radius;
	extent.y0 -= radius;
	extent.x1 += radius;
	extent.y1 += radius;
	return extent;
}

// ======================================================================
// strokes
// ======================================================================

TerrainGenerator::Layer * TerrainEditData::buildStrokeLayer () const
{
	char name [192];
	switch (m_strokeKind)
	{
	case SK_paint:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Paint %s (r %.0f)", m_nameCounter, m_strokeFamilyName.empty () ? "?" : m_strokeFamilyName.c_str (), m_radius));
		break;

	case SK_flora:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Clear flora (r %.0f)", m_nameCounter, m_radius));
		break;

	case SK_roughen:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Roughen %s %.1f m (r %.0f)", m_nameCounter, m_strokeFamilyName.empty () ? "?" : m_strokeFamilyName.c_str (), m_strokeParam, m_radius));
		break;

	case SK_terrace:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Terrace %.1f m steps, %.0f%% flat (r %.0f)", m_nameCounter, m_strokeParam, m_strokeParam2 * 100.0f, m_radius));
		break;

	case SK_plant:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Plant %s %s %.0f%% (r %.0f)", m_nameCounter, m_strokeSubKind == PK_radial ? "grass" : "flora", m_strokeFamilyName.empty () ? "?" : m_strokeFamilyName.c_str (), m_strokeParam * 100.0f, m_radius));
		break;

	case SK_road:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Road %s (w %.0f)", m_nameCounter, m_strokeFamilyName.empty () ? "?" : m_strokeFamilyName.c_str (), m_radius * 2.0f));
		break;

	case SK_river:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d River %.1f m deep (w %.0f)", m_nameCounter, m_strokeParam, m_radius * 2.0f));
		break;

	case SK_tree:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Plant trees %s %.0f%%, %d trees (r %.0f)", m_nameCounter, m_strokeFamilyName.empty () ? "?" : m_strokeFamilyName.c_str (), m_strokeParam * 100.0f, static_cast<int> (m_strokeTreeTiles.size ()), m_radius));
		break;

	case SK_smooth:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Smooth %.0f%% (r %.0f)", m_nameCounter, m_strokeParam * 100.0f, m_radius));
		break;

	case SK_water:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Water level %.1f m, %.1f m deep (r %.0f)", m_nameCounter, m_strokeHeight, m_strokeParam, m_radius));
		break;

	case SK_height:
	default:
		if (m_strokeOperation == TGO_replace)
			IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d %s to %.1f m (r %.0f)", m_nameCounter, toolName (m_tool, m_strokeInverted), m_strokeHeight, m_radius));
		else
			IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d %s %.1f m (r %.0f)", m_nameCounter, toolName (m_tool, m_strokeInverted), m_strokeHeight, m_radius));
		break;
	}
	name [sizeof (name) - 1] = '\0';

	//-- shapes: describe the shape instead of the brush size
	if (m_strokeShape != SH_brush && m_strokePoints.size () >= 3)
	{
		char * const tail = strstr (name, " (r ");
		if (tail)
		{
			const Rectangle2d extent = pointExtent (m_strokePoints, 0, 0.0f);
			const char * const shapeName = m_strokeShape == SH_rectangle ? "rectangle" : m_strokeShape == SH_circle ? "circle" : "polygon";
			IGNORE_RETURN (_snprintf (tail, sizeof (name) - static_cast<size_t> (tail - name), " (%s %.0f x %.0f)", shapeName, extent.x1 - extent.x0, extent.y1 - extent.y0));
			name [sizeof (name) - 1] = '\0';
		}
	}

	return createLayer (name, m_strokePoints, m_strokeKind, m_strokeOperation, m_strokeHeight, m_strokeFamilyId, m_radius, m_feather, m_strokeParam, m_strokeParam2, m_strokeSubKind, m_strokeSamples, m_waterShader.c_str (), m_strokeShape);
}

// ----------------------------------------------------------------------

TerrainEditData::PointList TerrainEditData::buildOutline (const Shape shape, const PointList & dragPoints)
{
	PointList outline;
	if (dragPoints.size () < 2)
		return outline;

	switch (shape)
	{
	case SH_rectangle:
		{
			const Vector2d & a = dragPoints.front ();
			const Vector2d & b = dragPoints.back ();
			if (fabsf (b.x - a.x) < 1.0f || fabsf (b.y - a.y) < 1.0f)
				return outline;
			outline.push_back (Vector2d (std::min (a.x, b.x), std::min (a.y, b.y)));
			outline.push_back (Vector2d (std::max (a.x, b.x), std::min (a.y, b.y)));
			outline.push_back (Vector2d (std::max (a.x, b.x), std::max (a.y, b.y)));
			outline.push_back (Vector2d (std::min (a.x, b.x), std::max (a.y, b.y)));
		}
		break;

	case SH_circle:
		{
			const Vector2d & centre = dragPoints.front ();
			const float r = centre.magnitudeBetween (dragPoints.back ());
			if (r < 1.0f)
				return outline;
			//-- about one point every 4 m, 16..96 points
			const int count = std::max (16, std::min (96, static_cast<int> (2.0f * PI * r / 4.0f)));
			for (int i = 0; i < count; ++i)
			{
				const float angle = 2.0f * PI * static_cast<float> (i) / static_cast<float> (count);
				outline.push_back (Vector2d (centre.x + r * cosf (angle), centre.y + r * sinf (angle)));
			}
		}
		break;

	case SH_polygon:
	default:
		{
			//-- the drag is the outline: even it out (at most ~120 points), then close it
			float length = 0.0f;
			for (size_t i = 1; i < dragPoints.size (); ++i)
				length += dragPoints [i - 1].magnitudeBetween (dragPoints [i]);
			length += dragPoints.back ().magnitudeBetween (dragPoints.front ());
			const float spacing = std::max (2.0f, length / 120.0f);

			PointList closed = dragPoints;
			closed.push_back (dragPoints.front ());
			outline = smoothPath (closed, spacing, 1);
			if (!outline.empty () && outline.back ().magnitudeBetween (outline.front ()) < spacing * 0.5f)
				outline.pop_back ();
			if (outline.size () < 3)
			{
				outline.clear ();
				return outline;
			}

			//-- reject slivers: the area must be at least 4 square metres
			float area = 0.0f;
			for (size_t i = 0; i < outline.size (); ++i)
			{
				const Vector2d & p = outline [i];
				const Vector2d & q = outline [(i + 1) % outline.size ()];
				area += p.x * q.y - q.x * p.y;
			}
			if (fabsf (area) * 0.5f < 4.0f)
				outline.clear ();
		}
		break;
	}

	return outline;
}

// ----------------------------------------------------------------------

bool TerrainEditData::isInsidePolygon (const Vector2d & point, const PointList & outline)
{
	bool inside = false;
	const size_t n = outline.size ();
	for (size_t i = 0, j = n - 1; i < n; j = i++)
	{
		const Vector2d & a = outline [i];
		const Vector2d & b = outline [j];
		if (((a.y > point.y) != (b.y > point.y)) && (point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x))
			inside = !inside;
	}
	return inside;
}

// ----------------------------------------------------------------------

TerrainEditData::PointList TerrainEditData::coverPoints (const PointList & outline, float & radius)
{
	//-- a grid of clear-flora circles that covers the shape: grid spacing s, circles of radius
	//-- s * 0.75 (just over half the diagonal), at most ~900 circles (the clear-flora keys allow 1024)
	PointList result;
	const Rectangle2d extent = pointExtent (outline, 0, 0.0f);
	const float width  = extent.x1 - extent.x0;
	const float height = extent.y1 - extent.y0;
	const float spacing = std::max (8.0f, sqrtf (width * height / 900.0f) * 1.05f);
	radius = spacing * 0.75f;

	if (outline.size () < 3)
		return result;

	PointList ring (outline);
	ring.push_back (outline.front ());

	for (float z = extent.y0 + spacing * 0.5f; z < extent.y1 + spacing * 0.5f; z += spacing)
		for (float x = extent.x0 + spacing * 0.5f; x < extent.x1 + spacing * 0.5f; x += spacing)
		{
			//-- keep grid points inside the shape, or close enough to its edge that their circle reaches in
			const Vector2d p (x, z);
			if (isInsidePolygon (p, outline) || distanceSquaredToPolyline (p, ring) < spacing * spacing * 0.5f)
				result.push_back (p);
			if (result.size () >= 1000)
				return result;
		}

	return result;
}

// ----------------------------------------------------------------------

void TerrainEditData::refreshSwimWater ()
{
	//-- the engine's swim / water-height list is built when the planet loads; rebuild it so run-time
	//-- water (rivers, Water level) can be swum in. The layers are already in or out of the generator.
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	if (terrainTemplate)
		const_cast<ProceduralTerrainAppearanceTemplate *> (terrainTemplate)->rebuildWaterTables ();
}

// ----------------------------------------------------------------------

bool TerrainEditData::bakeSmooth ()
{
	//-- There is no "smooth" terrain rule (and a new one would make the .trn unreadable for the
	//-- server), so the result is baked: the ground is sampled on the terrain's own pole grid, blurred,
	//-- and every pole that moves gets a tiny fixed-height patch (createSmoothLayer).
	m_strokeSamples.points.clear ();
	m_strokeSamples.counts.clear ();

	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	const TerrainGenerator * const generator = terrainTemplate ? terrainTemplate->getTerrainGenerator () : 0;
	if (!generator || m_strokePoints.empty ())
		return false;

	const bool  isShape = m_strokeShape != SH_brush;
	const PointList & points = m_strokePoints;
	const Rectangle2d area = pointExtent (points, 0, isShape ? 0.0f : m_radius);
	const float halfSize = isShape ? 0.5f * std::min (area.x1 - area.x0, area.y1 - area.y0) : m_radius;

	//-- the finest pole spacing (2 m on Tatooine); coarser for very large areas (at most ~6000 poles)
	float spacing = std::max (0.5f, terrainTemplate->getTileWidthInMeters () * 0.5f);
	while (((area.x1 - area.x0) / spacing + 1.0f) * ((area.y1 - area.y0) / spacing + 1.0f) > 6000.0f)
		spacing *= 2.0f;

	//-- blur width: half the brush radius (shapes: a quarter of the smaller side), 2 poles .. 32 m
	const float blur = std::max (2.0f * spacing, std::min (32.0f, halfSize * 0.5f));
	const int   k    = std::max (1, static_cast<int> (blur / spacing + 0.5f));
	const int   margin = 2 * k + 1;

	const int gx0 = static_cast<int> (floorf (area.x0 / spacing)) - margin;
	const int gz0 = static_cast<int> (floorf (area.y0 / spacing)) - margin;
	const int gx1 = static_cast<int> (ceilf  (area.x1 / spacing)) + margin;
	const int gz1 = static_cast<int> (ceilf  (area.y1 / spacing)) + margin;
	const int nx  = gx1 - gx0 + 1;
	const int nz  = gz1 - gz0 + 1;
	std::vector<float> ground (static_cast<size_t> (nx * nz), 0.0f);

	//-- sample the ground in blocks with the generator, exactly the client's chunk pole count:
	//-- the generator sizes its fractal caches once, from the first chunk it builds, and a larger
	//-- block would write past them (heap corruption -> crash a moment later)
	const int block = 2 * terrainTemplate->getNumberOfTilesPerChunk () + terrainTemplate->getChunkOriginOffset () + terrainTemplate->getChunkUpperPad ();
	if (block < 2)
		return false;
	TerrainGenerator::CreateChunkBuffer chunkBuffer;
	chunkBuffer.allocate (block);
	for (int bz = 0; bz < nz; bz += block)
		for (int bx = 0; bx < nx; bx += block)
		{
			TerrainGenerator::GeneratorChunkData cd (terrainTemplate->getLegacyMode ());
			cd.shaderGroup                 = &generator->getShaderGroup ();
			cd.floraGroup                  = &generator->getFloraGroup ();
			cd.radialGroup                 = &generator->getRadialGroup ();
			cd.environmentGroup            = &generator->getEnvironmentGroup ();
			cd.fractalGroup                = &generator->getFractalGroup ();
			cd.bitmapGroup                 = &generator->getBitmapGroup ();
			cd.heightMap                   = &chunkBuffer.heightMap;
			cd.colorMap                    = &chunkBuffer.colorMap;
			cd.shaderMap                   = &chunkBuffer.shaderMap;
			cd.floraStaticCollidableMap    = &chunkBuffer.floraStaticCollidableMap;
			cd.floraStaticNonCollidableMap = &chunkBuffer.floraStaticNonCollidableMap;
			cd.floraDynamicNearMap         = &chunkBuffer.floraDynamicNearMap;
			cd.floraDynamicFarMap          = &chunkBuffer.floraDynamicFarMap;
			cd.environmentMap              = &chunkBuffer.environmentMap;
			cd.vertexPositionMap           = &chunkBuffer.vertexPositionMap;
			cd.vertexNormalMap             = &chunkBuffer.vertexNormalMap;
			cd.excludeMap                  = &chunkBuffer.excludeMap;
			cd.passableMap                 = &chunkBuffer.passableMap;
			cd.start                       = Vector (static_cast<float> (gx0 + bx) * spacing, 0.0f, static_cast<float> (gz0 + bz) * spacing);
			cd.originOffset                = 0;
			cd.upperPad                    = 0;
			cd.numberOfPoles               = block;
			cd.distanceBetweenPoles        = spacing;
			generator->generateChunk (cd);

			for (int z = 0; z < block && bz + z < nz; ++z)
				for (int x = 0; x < block && bx + x < nx; ++x)
					ground [static_cast<size_t> ((bz + z) * nx + bx + x)] = chunkBuffer.heightMap.getData (x, z);
		}

	//-- blur: two box passes each way (close to a gaussian)
	std::vector<float> blurred (ground);
	{
		std::vector<float> temp (blurred.size ());
		for (int pass = 0; pass < 2; ++pass)
		{
			for (int z = 0; z < nz; ++z)
				for (int x = 0; x < nx; ++x)
				{
					float sum = 0.0f;
					for (int i = -k; i <= k; ++i)
						sum += blurred [static_cast<size_t> (z * nx + std::max (0, std::min (nx - 1, x + i)))];
					temp [static_cast<size_t> (z * nx + x)] = sum / static_cast<float> (2 * k + 1);
				}
			for (int z = 0; z < nz; ++z)
				for (int x = 0; x < nx; ++x)
				{
					float sum = 0.0f;
					for (int i = -k; i <= k; ++i)
						sum += temp [static_cast<size_t> (std::max (0, std::min (nz - 1, z + i)) * nx + x)];
					blurred [static_cast<size_t> (z * nx + x)] = sum / static_cast<float> (2 * k + 1);
				}
		}
	}

	//-- how much of the smoothing each pole gets: full inside, fading out over the soft edge
	PointList ring (points);
	if (isShape && !ring.empty ())
		ring.push_back (points.front ());
	const float shapeFeather = m_feather * halfSize;
	const float brushInner   = m_radius * (1.0f - m_feather);

	float largest = 0.0f;
	for (int z = margin; z < nz - margin; ++z)
		for (int x = margin; x < nx - margin; ++x)
		{
			const Vector2d p (static_cast<float> (gx0 + x) * spacing, static_cast<float> (gz0 + z) * spacing);
			float weight = 0.0f;
			if (isShape)
			{
				if (!isInsidePolygon (p, points))
					continue;
				const float edge = sqrtf (distanceSquaredToPolyline (p, ring));
				weight = shapeFeather > 0.0f ? std::min (1.0f, edge / shapeFeather) : 1.0f;
			}
			else
			{
				const float d = sqrtf (distanceSquaredToPolyline (p, points));
				if (d >= m_radius)
					continue;
				weight = d <= brushInner ? 1.0f : 1.0f - (d - brushInner) / std::max (0.001f, m_radius - brushInner);
			}
			weight = weight * weight * (3.0f - 2.0f * weight);   // ease in / out

			const size_t index = static_cast<size_t> (z * nx + x);
			const float change = (blurred [index] - ground [index]) * weight * m_strokeParam;
			if (!_finite (change) || !_finite (ground [index]) || fabsf (change) < 0.02f)
				continue;
			largest = std::max (largest, fabsf (change));
			m_strokeSamples.points.push_back (Vector (p.x, ground [index] + change, p.y));
		}

	m_strokeParam2 = spacing * 0.5f;
	logEvent ("SMOOTH baked %d patches: pole spacing %.1f m, blur %.1f m, largest change %.2f m", static_cast<int> (m_strokeSamples.points.size ()), spacing, static_cast<float> (k) * spacing, largest);

	if (m_strokeSamples.points.empty ())
	{
		m_message = "Smooth: the ground here is already smooth (nothing moved more than 2 cm).";
		return false;
	}

	char note [160];
	IGNORE_RETURN (_snprintf (note, sizeof (note), "Smooth: %d ground points moved, by up to %.1f m.", static_cast<int> (m_strokeSamples.points.size ()), largest));
	note [sizeof (note) - 1] = '\0';
	m_message = note;
	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::changesHeight (const StrokeKind kind)
{
	return kind == SK_height || kind == SK_roughen || kind == SK_terrace || kind == SK_road || kind == SK_river || kind == SK_water || kind == SK_smooth;
}

// ----------------------------------------------------------------------

void TerrainEditData::resnapTrees (const Rectangle2d & area)
{
	//-- trees on most planets sit at a height stored in the baked tree map, not on the ground,
	//-- so after the ground changes they are moved to the new ground (this also goes into the save)
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	if (!terrainTemplate || !loadTreeMaps ())
		return;

	const float tileWidth    = 16.0f;
	const float tileBorder   = terrainTemplate->getCollidableTileBorder ();
	const int   width        = m_treeMapWidth;
	const int   centerOffset = width / 2;

	const int tx0 = static_cast<int> (floorf (area.x0 / tileWidth)) - 1;
	const int tx1 = static_cast<int> (floorf (area.x1 / tileWidth)) + 1;
	const int tz0 = static_cast<int> (floorf (area.y0 / tileWidth)) - 1;
	const int tz1 = static_cast<int> (floorf (area.y1 / tileWidth)) + 1;

	int moved = 0;
	float largest = 0.0f;
	for (int tz = tz0; tz <= tz1; ++tz)
		for (int tx = tx0; tx <= tx1; ++tx)
		{
			const int keyX = tx + centerOffset;
			const int keyZ = tz + centerOffset;
			if (keyX < 0 || keyZ < 0 || keyX >= width || keyZ >= width)
				continue;

			const size_t tile = static_cast<size_t> (keyZ * width + keyX);
			if (m_treeFamilies [tile] == 0)
				continue;

			RandomGenerator random (static_cast<uint32> (tile));
			const float xOffset = random.randomReal (0.f, 1.f);
			const float zOffset = random.randomReal (0.f, 1.f);
			const Vector2d tree (static_cast<float> (tx) * tileWidth + tileBorder + xOffset * (tileWidth - 2.0f * tileBorder),
			                     static_cast<float> (tz) * tileWidth + tileBorder + zOffset * (tileWidth - 2.0f * tileBorder));
			if (!area.isWithin (tree.x, tree.y))
				continue;

			const float ground = m_appearance->generateHeight_expensive (tree);
			if (!_finite (ground) || fabsf (ground - m_treeHeightMap [tile]) < 0.05f)
				continue;

			largest = std::max (largest, fabsf (ground - m_treeHeightMap [tile]));
			m_treeHeightMap [tile] = ground;
			++moved;
		}

	if (moved == 0)
		return;

	const_cast<ProceduralTerrainAppearanceTemplate *> (terrainTemplate)->setStaticCollidableFloraMaps (&m_treeFamilies [0], &m_treeHeightMap [0]);
	logEvent ("TREES moved %d trees to the new ground (by up to %.1f m)", moved, largest);

	//-- rebuild the area so its chunks place the trees again at the new heights
	markDirty (area);
	flushDirty ();
}

// ----------------------------------------------------------------------

bool TerrainEditData::loadTreeMaps ()
{
	if (m_treeMapWidth > 0)
		return true;

	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	if (!terrainTemplate || terrainTemplate->getLegacyMode ())
		return false;   // legacy planets place trees from terrain rules, not from the baked map

	const int width = terrainTemplate->getMapWidthInFlora ();
	if (width <= 0)
		return false;

	m_treeFamilies.resize (static_cast<size_t> (width * width));
	m_treeHeightMap.resize (static_cast<size_t> (width * width));
	for (int z = 0; z < width; ++z)
		for (int x = 0; x < width; ++x)
		{
			m_treeFamilies  [static_cast<size_t> (z * width + x)] = terrainTemplate->getStaticCollidableFloraFamily (x, z);
			m_treeHeightMap [static_cast<size_t> (z * width + x)] = terrainTemplate->getStaticCollidableFloraHeight (x, z);
		}

	m_treeMapWidth = width;
	return true;
}

// ----------------------------------------------------------------------

void TerrainEditData::setTreeTiles (const std::vector<int> & tiles, const int familyId, const std::vector<float> & heights)
{
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	if (!terrainTemplate || !loadTreeMaps () || tiles.size () != heights.size ())
		return;

	const int numberOfTiles = m_treeMapWidth * m_treeMapWidth;
	for (size_t i = 0; i < tiles.size (); ++i)
		if (tiles [i] >= 0 && tiles [i] < numberOfTiles)
		{
			m_treeFamilies  [static_cast<size_t> (tiles [i])] = familyId;
			m_treeHeightMap [static_cast<size_t> (tiles [i])] = heights [i];
		}

	//-- the engine's maps are packed, so they are rebuilt whole (a fraction of a second)
	const_cast<ProceduralTerrainAppearanceTemplate *> (terrainTemplate)->setStaticCollidableFloraMaps (&m_treeFamilies [0], &m_treeHeightMap [0]);
}

// ----------------------------------------------------------------------

bool TerrainEditData::bakeTrees ()
{
	m_strokeTreeTiles.clear ();
	m_strokeTreeHeights.clear ();
	m_strokeTreeOldHeights.clear ();

	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	if (!terrainTemplate || !loadTreeMaps () || m_strokePoints.empty ())
	{
		m_message = "Plant trees: this planet has no baked tree map.";
		return false;
	}

	//-- the engine puts one tree per 16 m tile, at a seeded spot inside it (ProceduralTerrainAppearance::createFlora)
	const float tileWidth    = 16.0f;
	const float tileBorder   = terrainTemplate->getCollidableTileBorder ();
	const int   width        = m_treeMapWidth;
	const int   centerOffset = width / 2;

	const bool isShape = m_strokeShape != SH_brush;
	const PointList & points = m_strokePoints;
	const Rectangle2d area = pointExtent (points, 0, isShape ? 0.0f : m_radius);
	const float radiusSquared = m_radius * m_radius;

	const int tx0 = static_cast<int> (floorf (area.x0 / tileWidth)) - 1;
	const int tx1 = static_cast<int> (floorf (area.x1 / tileWidth)) + 1;
	const int tz0 = static_cast<int> (floorf (area.y0 / tileWidth)) - 1;
	const int tz1 = static_cast<int> (floorf (area.y1 / tileWidth)) + 1;

	int occupied = 0;
	for (int tz = tz0; tz <= tz1; ++tz)
		for (int tx = tx0; tx <= tx1; ++tx)
		{
			const int keyX = tx + centerOffset;
			const int keyZ = tz + centerOffset;
			if (keyX < 0 || keyZ < 0 || keyX >= width || keyZ >= width)
				continue;

			const int tile = keyZ * width + keyX;
			RandomGenerator random (static_cast<uint32> (tile));
			const float xOffset = random.randomReal (0.f, 1.f);
			const float zOffset = random.randomReal (0.f, 1.f);
			const Vector2d tree (static_cast<float> (tx) * tileWidth + tileBorder + xOffset * (tileWidth - 2.0f * tileBorder),
			                     static_cast<float> (tz) * tileWidth + tileBorder + zOffset * (tileWidth - 2.0f * tileBorder));

			if (isShape ? !isInsidePolygon (tree, points) : distanceSquaredToPolyline (tree, points) > radiusSquared)
				continue;

			//-- only empty tiles: a tile that already has a tree keeps it
			if (m_treeFamilies [static_cast<size_t> (tile)] != 0)
			{
				++occupied;
				continue;
			}

			//-- density: a different random pick each stroke, so going over an area again adds more
			RandomGenerator pick (static_cast<uint32> (tile) * 2654435761u + static_cast<uint32> (m_nameCounter) * 40503u + 17u);
			if (pick.randomReal (0.f, 1.f) >= m_strokeParam)
				continue;

			m_strokeTreeTiles.push_back (tile);
			m_strokeTreeHeights.push_back (m_appearance->generateHeight_expensive (tree));
			m_strokeTreeOldHeights.push_back (m_treeHeightMap [static_cast<size_t> (tile)]);
			if (m_strokeTreeTiles.size () >= 4000)
				break;
		}

	logEvent ("TREES planting %d (family %d, density %.0f%%); %d tiles in the area already had a tree", static_cast<int> (m_strokeTreeTiles.size ()), m_strokeFamilyId, m_strokeParam * 100.0f, occupied);

	if (m_strokeTreeTiles.empty ())
	{
		m_message = occupied > 0 ? "Plant trees: every 16 m tile here already has a tree (one tree per tile)." : "Plant trees: no tree spot inside the area (trees go one per 16 m tile; try a bigger brush or a higher density).";
		return false;
	}

	char note [160];
	IGNORE_RETURN (_snprintf (note, sizeof (note), "Planted %d trees (one per 16 m tile at most).", static_cast<int> (m_strokeTreeTiles.size ())));
	note [sizeof (note) - 1] = '\0';
	m_message = note;
	return true;
}

// ----------------------------------------------------------------------

TerrainGenerator::Layer * TerrainEditData::createSmoothLayer (const char * const name, const PathSamples & samples, const float halfPatch)
{
	//-- one sublayer per moved pole: a square around the pole (no soft edge, slightly overlapping its
	//-- neighbours so no pole falls between two) that sets the pole's baked height. The engine skips
	//-- sublayers whose extent misses a chunk, so only the few near each chunk are evaluated.
	TerrainGenerator::Layer * const layer = new TerrainGenerator::Layer ();
	layer->setName (name ? name : "godclient_edit");

	const float h = std::max (0.25f, halfPatch) + 0.01f;
	for (std::vector<Vector>::const_iterator i = samples.points.begin (); i != samples.points.end (); ++i)
	{
		BoundaryRectangle * const boundary = new BoundaryRectangle ();
		boundary->setName ("godclient_smooth_patch");
		boundary->setFeatherDistance (0.0f);
		boundary->setRectangle (Rectangle2d (i->x - h, i->z - h, i->x + h, i->z + h));

		AffectorHeightConstant * const affector = new AffectorHeightConstant ();
		affector->setName ("godclient_smooth");
		affector->setOperation (TGO_replace);
		affector->setHeight (i->y);

		TerrainGenerator::Layer * const patch = new TerrainGenerator::Layer ();
		patch->setName ("godclient_smooth_patch");
		patch->addBoundary (boundary);
		patch->addAffector (affector);
		patch->calculateExtent ();
		layer->addLayer (patch);
	}

	layer->calculateExtent ();
	return layer;
}

// ----------------------------------------------------------------------

TerrainGenerator::Layer * TerrainEditData::createLayer (const char * const name, const PointList & points, const StrokeKind kind, const TerrainGeneratorOperation operation, const float height, const int familyId, const float radius, const float feather, const float param, const float param2, const int subKind, const PathSamples & samples, const char * const waterShader, const Shape shape)
{
	if (kind == SK_road || kind == SK_river)
		return createPathLayer (name, points, kind, familyId, radius, feather, param, samples, waterShader);

	if (kind == SK_smooth)
		return createSmoothLayer (name, samples, param2);

	if (kind == SK_tree)
	{
		//-- trees live in the baked flora map; the layer only marks the edit (no boundaries or
		//-- affectors, so the generator skips it). It is never saved.
		TerrainGenerator::Layer * const marker = new TerrainGenerator::Layer ();
		marker->setName (name ? name : "godclient_edit");
		return marker;
	}

	TerrainGenerator::Layer * const layer = new TerrainGenerator::Layer ();
	layer->setName (name ? name : "godclient_edit");

	if (shape != SH_brush || kind == SK_water)
	{
		//-- filled shape: a closed polygon. Its soft edge (metres, inside the outline) is the
		//-- Soft edge fraction of the brush radius.
		BoundaryPolygon * const boundary = new BoundaryPolygon ();
		boundary->setName ("godclient_shape");
		boundary->setFeatherFunction (TGFF_easeInOut);
		for (PointList::const_iterator i = points.begin (); i != points.end (); ++i)
			boundary->addPoint (*i);
		if (!points.empty ())
			boundary->replacePoint (0, points.front ());   // addPoint leaves the extent stale; this recalculates it
		//-- soft edge: the Soft edge fraction of the way from the outline to the middle, so small
		//-- shapes still reach full strength inside (water: the slope of the dug basin's banks)
		{
			const Rectangle2d shapeExtent = pointExtent (points, 0, 0.0f);
			const float halfSize = 0.5f * std::min (shapeExtent.x1 - shapeExtent.x0, shapeExtent.y1 - shapeExtent.y0);
			boundary->setFeatherDistance (feather * std::max (0.0f, halfSize));
		}
		UNREF (radius);

		if (kind == SK_water)
		{
			boundary->setLocalWaterTable (true);
			boundary->setLocalWaterTableHeight (height);
			boundary->setLocalWaterTableShaderSize (8.0f);
			boundary->setLocalWaterTableShaderTemplateName (waterShader && *waterShader ? waterShader : "shader/water_anim_bump_shader.sht");
			boundary->setWaterType (TGWT_water);
		}

		layer->addBoundary (boundary);
	}
	else
	{
		BoundaryPolyline * const boundary = new BoundaryPolyline ();
		boundary->setName ("godclient_stroke");
		boundary->setFeatherFunction (TGFF_easeInOut);
		boundary->setFeatherDistance (feather);
		for (PointList::const_iterator i = points.begin (); i != points.end (); ++i)
			boundary->addPoint (*i);
		boundary->setWidth (radius);   // also recalculates the extent
		layer->addBoundary (boundary);
	}

	switch (kind)
	{
	case SK_water:
		//-- the boundary's local water table is the water surface. With a depth, the ground inside
		//-- is also dug down to (water height - depth), sloping back up over the soft edge.
		if (param > 0.0f)
		{
			AffectorHeightConstant * const basin = new AffectorHeightConstant ();
			basin->setName ("godclient_lake_basin");
			basin->setOperation (TGO_replace);
			basin->setHeight (height - param);
			layer->addAffector (basin);
		}
		break;

	case SK_paint:
		{
			AffectorShaderConstant * const affector = new AffectorShaderConstant ();
			affector->setName ("godclient_paint");
			affector->setFamilyId (familyId);
			layer->addAffector (affector);

			//-- the planet's colour layers tint every texture (desert colour-ramp fractals give the
			//-- ground its patchy look), which leaves dark/blotchy areas on the painted texture.
			//-- Reset the tint to neutral (white = untinted) inside the stroke, feathered at the edge.
			AffectorColorConstant * const tint = new AffectorColorConstant ();
			tint->setName ("godclient_paint_neutral_tint");
			tint->setOperation (TGO_replace);
			tint->setColor (PackedRgb::solidWhite);
			layer->addAffector (tint);
		}
		break;

	case SK_flora:
		{
			//-- remove every kind of flora. On most planets trees/rocks come from the baked flora map
			//-- instead; those are handled by addFloraClearing and buildClearedFloraMap.
			AffectorFloraStaticCollidableConstant * const collidable = new AffectorFloraStaticCollidableConstant ();
			collidable->setName ("godclient_clear_collidable");
			collidable->setRemoveAll (true);
			layer->addAffector (collidable);

			AffectorFloraStaticNonCollidableConstant * const nonCollidable = new AffectorFloraStaticNonCollidableConstant ();
			nonCollidable->setName ("godclient_clear_noncollidable");
			nonCollidable->setRemoveAll (true);
			layer->addAffector (nonCollidable);

			AffectorFloraDynamicNearConstant * const nearFlora = new AffectorFloraDynamicNearConstant ();
			nearFlora->setName ("godclient_clear_radial_near");
			nearFlora->setRemoveAll (true);
			layer->addAffector (nearFlora);

			AffectorFloraDynamicFarConstant * const farFlora = new AffectorFloraDynamicFarConstant ();
			farFlora->setName ("godclient_clear_radial_far");
			farFlora->setRemoveAll (true);
			layer->addAffector (farFlora);
		}
		break;

	case SK_roughen:
		{
			//-- the planet's fractal gives 0..1; scaled by the roughness and then lowered by half of it,
			//-- so the bumps go up and down around the existing ground instead of raising it
			AffectorHeightFractal * const fractal = new AffectorHeightFractal ();
			fractal->setName ("godclient_roughen");
			fractal->setFamilyId (familyId);
			fractal->setOperation (TGO_add);
			fractal->setScaleY (param);
			layer->addAffector (fractal);

			AffectorHeightConstant * const centre = new AffectorHeightConstant ();
			centre->setName ("godclient_roughen_centre");
			centre->setOperation (TGO_subtract);
			centre->setHeight (param * 0.5f);
			layer->addAffector (centre);
		}
		break;

	case SK_terrace:
		{
			AffectorHeightTerrace * const terrace = new AffectorHeightTerrace ();
			terrace->setName ("godclient_terrace");
			terrace->setHeight (param);
			terrace->setFraction (param2);
			layer->addAffector (terrace);
		}
		break;

	case SK_plant:
		if (subKind == PK_radial)
		{
			AffectorFloraDynamicNearConstant * const grass = new AffectorFloraDynamicNearConstant ();
			grass->setName ("godclient_plant_radial");
			grass->setFamilyId (familyId);
			grass->setOperation (TGO_add);
			grass->setDensityOverride (true);
			grass->setDensityOverrideDensity (param);
			layer->addAffector (grass);
		}
		else
		{
			AffectorFloraStaticNonCollidableConstant * const flora = new AffectorFloraStaticNonCollidableConstant ();
			flora->setName ("godclient_plant_static");
			flora->setFamilyId (familyId);
			flora->setOperation (TGO_add);
			flora->setDensityOverride (true);
			flora->setDensityOverrideDensity (param);
			layer->addAffector (flora);
		}
		break;

	case SK_height:
	default:
		{
			AffectorHeightConstant * const affector = new AffectorHeightConstant ();
			affector->setName ("godclient_height");
			affector->setOperation (operation);
			affector->setHeight (height);
			layer->addAffector (affector);
		}
		break;
	}

	layer->calculateExtent ();
	return layer;
}

// ----------------------------------------------------------------------

void TerrainEditData::addFloraClearing (const int strokeId, const PointList & points, const int firstPoint, const float radius)
{
	if (!m_appearance)
		return;

	for (int i = std::max (0, firstPoint); i < static_cast<int> (points.size ()); ++i)
	{
		const Vector2d & p = points [static_cast<size_t> (i)];
		m_appearance->addClearCollidableFloraObject (floraClearKey (strokeId, i), Vector (p.x, 0.0f, p.y), radius);

		//-- grass / bushes / small rocks (radial flora managers). The engine keeps a pointer to the
		//-- list and reads the object's position, so both live until the clearing is removed.
		const int key = static_cast<int> (reinterpret_cast<uintptr_t> (floraClearKey (strokeId, i)));
		if (m_radialClears.find (key) == m_radialClears.end ())
		{
			RadialClear clear;
			clear.m_object = new Object ();
			clear.m_object->setPosition_p (Vector (p.x, 0.0f, p.y));
			clear.m_list   = new std::vector<std::pair<Vector, float> > ();
			clear.m_list->push_back (std::make_pair (Vector::zero, radius));
			m_radialClears [key] = clear;
			m_appearance->addClearNonCollidableFloraObject (clear.m_object, *clear.m_list);
		}
	}

	refreshRadialFlora ();
}

// ----------------------------------------------------------------------

void TerrainEditData::refreshRadialFlora ()
{
	//-- nothing to do: the engine now re-plants flora in every rebuilt area
	//-- (ClientRadialFloraManager::markRegionReplant, called when a terrain rebuild lands)
}

// ----------------------------------------------------------------------

void TerrainEditData::removeFloraClearing (const int strokeId, const int numberOfPoints)
{
	if (!m_appearance)
		return;

	for (int i = 0; i < numberOfPoints; ++i)
	{
		m_appearance->removeClearCollidableFloraObject (floraClearKey (strokeId, i));

		const RadialClearMap::iterator found = m_radialClears.find (static_cast<int> (reinterpret_cast<uintptr_t> (floraClearKey (strokeId, i))));
		if (found != m_radialClears.end ())
		{
			m_appearance->removeClearNonCollidableFloraObject (found->second.m_object);
			delete found->second.m_object;
			delete found->second.m_list;
			m_radialClears.erase (found);
		}
	}

	refreshRadialFlora ();
}

// ----------------------------------------------------------------------

PackedIntegerMap * TerrainEditData::buildClearedFloraMap (const PackedIntegerMap & original) const
{
	const int width  = original.getWidth ();
	const int height = original.getHeight ();
	if (width <= 0 || height <= 0)
		return 0;

	std::vector<int> values (static_cast<size_t> (width * height));
	for (int z = 0; z < height; ++z)
		for (int x = 0; x < width; ++x)
			values [static_cast<size_t> (z * width + x)] = original.getValue (x, z);

	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	const float tileWidth  = 16.0f;   // ProceduralTerrainAppearance::createFlora uses 16 m flora tiles
	const float tileBorder = terrainTemplate ? terrainTemplate->getCollidableTileBorder () : 0.0f;
	const int   centerOffset = width / 2;

	int cleared = 0;
	for (EditList::const_iterator e = m_edits.begin (); e != m_edits.end (); ++e)
	{
		if (e->m_kind != SK_flora)
			continue;

		const float radiusSquared = e->m_radius * e->m_radius;
		const int x0 = std::max (0,          static_cast<int> (floorf (e->m_extent.x0 / tileWidth)) - 1 + centerOffset);
		const int x1 = std::min (width - 1,  static_cast<int> (floorf (e->m_extent.x1 / tileWidth)) + 1 + centerOffset);
		const int z0 = std::max (0,          static_cast<int> (floorf (e->m_extent.y0 / tileWidth)) - 1 + centerOffset);
		const int z1 = std::min (height - 1, static_cast<int> (floorf (e->m_extent.y1 / tileWidth)) + 1 + centerOffset);

		for (int keyZ = z0; keyZ <= z1; ++keyZ)
			for (int keyX = x0; keyX <= x1; ++keyX)
			{
				int & value = values [static_cast<size_t> (keyZ * width + keyX)];
				if (value == 0)
					continue;

				//-- the tile's tree sits where ProceduralTerrainAppearance::createFlora puts it
				RandomGenerator random (static_cast<uint32> (keyZ * width + keyX));
				const float xOffset = random.randomReal (0.f, 1.f);
				const float zOffset = random.randomReal (0.f, 1.f);
				const Vector2d tree (static_cast<float> (keyX - centerOffset) * tileWidth + tileBorder + xOffset * (tileWidth - 2.0f * tileBorder),
				                     static_cast<float> (keyZ - centerOffset) * tileWidth + tileBorder + zOffset * (tileWidth - 2.0f * tileBorder));

				if (e->m_shape != SH_brush ? isInsidePolygon (tree, e->m_points) : distanceSquaredToPolyline (tree, e->m_points) <= radiusSquared)
				{
					value = 0;
					++cleared;
				}
			}
	}

	UNREF (cleared);
	return new PackedIntegerMap (width, height, &values [0]);
}

// ----------------------------------------------------------------------

bool TerrainEditData::beginStroke (const Vector2d & position_w, const bool alternate)
{
	if (m_strokeActive)
		endStroke ();

	{
		const Status status = getStatus ();
		if (status != S_ok)
		{
			logEvent ("STROKE REFUSED: %s", status == S_noTerrain ? "no procedural terrain found (TerrainObject missing or not a ClientProceduralTerrainAppearance)" : "terrain generation is single-threaded");
			return false;
		}
	}

	logEvent ("STROKE BEGIN tool=%d (%s)", static_cast<int> (m_tool), toolName (m_tool, alternate));

	ClientProceduralTerrainAppearance * const appearance = m_appearance;

	//-- right button: raise <-> lower, or pick the height under the cursor for flatten / level
	//-- (the tool stays the same)
	if (alternate && (m_tool == T_flatten || m_tool == T_level || m_tool == T_water))
	{
		m_levelHeight        = appearance->generateHeight_expensive (position_w);
		m_flattenNeedsSample = false;
		++m_revision;
		return false;
	}

	m_strokeInverted = alternate;
	m_strokeKind     = SK_height;
	m_strokeFamilyId = 0;
	m_strokeFloraPointsCleared = 0;
	m_strokeParam    = 0.0f;
	m_strokeParam2   = 0.0f;
	m_strokeSubKind  = 0;
	m_strokeSamples.points.clear ();
	m_strokeSamples.counts.clear ();
	m_strokeFloraPoints.clear ();
	m_strokeFloraRadius = m_radius;

	//-- roads and rivers are always paths; water always fills a shape (a brush drag draws its outline)
	if (m_tool == T_road || m_tool == T_river)
		m_strokeShape = SH_brush;
	else if (m_tool == T_water && m_shape == SH_brush)
		m_strokeShape = SH_polygon;
	else
		m_strokeShape = m_shape;

	switch (m_tool)
	{
	case T_smooth:
		m_strokeKind      = SK_smooth;
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = 0.0f;
		m_strokeParam     = m_smoothStrength;
		break;

	case T_water:
		m_strokeKind      = SK_water;
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = m_levelHeight;
		m_strokeParam     = m_riverDepth;   // basin depth below the water
		if (m_waterShader.empty ())
			m_waterShader = findWaterShader ();
		break;

	case T_paint:
		if (m_paintFamilyId <= 0 || !*getShaderFamilyName (m_paintFamilyId))
			return false;   // no texture chosen
		m_strokeKind       = SK_paint;
		m_strokeFamilyId   = m_paintFamilyId;
		m_strokeFamilyName = getShaderFamilyName (m_paintFamilyId);
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = 0.0f;
		break;

	case T_clearFlora:
		m_strokeKind      = SK_flora;
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = 0.0f;
		break;

	case T_roughen:
		{
			const TerrainGenerator * const generator = getGenerator ();
			if (!generator || !generator->getFractalGroup ().hasFamily (m_roughenFamilyId) || m_roughness <= 0.0f)
			{
				logEvent ("STROKE REFUSED roughen: pattern %d %s, roughness %.2f", m_roughenFamilyId,
					generator && generator->getFractalGroup ().hasFamily (m_roughenFamilyId) ? "ok" : "unknown", m_roughness);
				return false;
			}
		}
		m_strokeKind       = SK_roughen;
		m_strokeFamilyId   = m_roughenFamilyId;
		m_strokeFamilyName = getFractalFamilyName (m_roughenFamilyId);
		m_strokeParam      = m_roughness;
		m_strokeOperation  = TGO_add;
		m_strokeHeight     = 0.0f;
		break;

	case T_terrace:
		m_strokeKind      = SK_terrace;
		m_strokeParam     = m_terraceStep;
		m_strokeParam2    = m_terraceFlat;
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = 0.0f;
		break;

	case T_plantFlora:
		{
			const TerrainGenerator * const generator = getGenerator ();
			const bool known = generator && (m_plantKind == PK_radial ? generator->getRadialGroup ().hasFamily (m_plantFamilyId) : generator->getFloraGroup ().hasFamily (m_plantFamilyId))
				&& (m_plantKind != PK_tree || loadTreeMaps ());
			if (!known || m_plantDensity <= 0.0f)
			{
				logEvent ("STROKE REFUSED plant: kind %d family %d %s, density %.2f", static_cast<int> (m_plantKind), m_plantFamilyId, known ? "ok" : "unknown", m_plantDensity);
				return false;
			}
		}
		m_strokeKind       = m_plantKind == PK_tree ? SK_tree : SK_plant;
		m_strokeFamilyId   = m_plantFamilyId;
		m_strokeFamilyName = getPlantName (m_plantKind, m_plantFamilyId);
		m_strokeSubKind    = m_plantKind;
		m_strokeParam      = m_plantDensity;
		m_strokeOperation  = TGO_add;
		m_strokeHeight     = 0.0f;
		break;

	case T_road:
	case T_river:
		if (m_paintFamilyId <= 0 || !*getShaderFamilyName (m_paintFamilyId))
		{
			logEvent ("STROKE REFUSED %s: no texture chosen in Paint texture", m_tool == T_road ? "road" : "river");
			return false;
		}
		m_strokeKind       = m_tool == T_road ? SK_road : SK_river;
		m_strokeFamilyId   = m_paintFamilyId;
		m_strokeFamilyName = getShaderFamilyName (m_paintFamilyId);
		m_strokeParam      = m_riverDepth;
		m_strokeOperation  = TGO_replace;
		m_strokeHeight     = 0.0f;
		if (m_tool == T_river && m_waterShader.empty ())
			m_waterShader = findWaterShader ();
		break;

	case T_raise:
		m_strokeOperation = alternate ? TGO_subtract : TGO_add;
		m_strokeHeight    = m_strength;
		break;

	case T_lower:
		m_strokeOperation = alternate ? TGO_add : TGO_subtract;
		m_strokeHeight    = m_strength;
		break;

	case T_flatten:
		//-- sticky: the first stroke picks the height under the click; later strokes reuse it so
		//-- overlapping strokes join up flat instead of leaving terraces
		if (m_flattenNeedsSample)
		{
			m_levelHeight        = appearance->generateHeight_expensive (position_w);
			m_flattenNeedsSample = false;
			++m_revision;   // show the picked height in the panel
		}
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = m_levelHeight;
		break;

	case T_level:
	default:
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = m_levelHeight;
		break;
	}

	if (m_strokeKind == SK_height && m_strokeOperation != TGO_replace && m_strokeHeight <= 0.0f)
		return false;

	clearRedo ();

	++m_nameCounter;
	m_strokeActive      = true;
	m_strokePoints.clear ();
	m_strokePoints.push_back (position_w);
	m_strokePointsShown = 0;
	m_strokeLayer       = 0;

	//-- the first preview appears as soon as the terrain worker is idle (endStroke forces it otherwise)
	swapStrokeLayer (false);
	return true;
}

// ----------------------------------------------------------------------

void TerrainEditData::continueStroke (const Vector2d & position_w)
{
	IGNORE_RETURN (getAppearance ());   // cancels the stroke if the terrain was replaced
	if (!m_strokeActive)
		return;

	setCursor (position_w);

	//-- shapes are built when the drag ends. Rectangle / circle: first point + the current point.
	//-- Polygon: the drag is the outline.
	if (m_strokeShape == SH_rectangle || m_strokeShape == SH_circle)
	{
		if (m_strokePoints.size () < 2)
			m_strokePoints.push_back (position_w);
		else
			m_strokePoints.back () = position_w;
		return;
	}

	if (m_strokeShape == SH_polygon)
	{
		if (m_strokePoints.size () < 4000 && m_strokePoints.back ().magnitudeBetweenSquared (position_w) >= 4.0f)
			m_strokePoints.push_back (position_w);
		return;
	}

	//-- space points about a quarter of the brush apart
	const float spacing = std::max (1.0f, m_radius * 0.25f);
	if (m_strokePoints.back ().magnitudeBetweenSquared (position_w) < spacing * spacing)
		return;

	m_strokePoints.push_back (position_w);

	//-- very long drags become several strokes so each layer stays cheap to evaluate
	if (static_cast<int> (m_strokePoints.size ()) >= cs_maximumStrokePoints)
	{
		const bool inverted = m_strokeInverted;
		endStroke ();
		IGNORE_RETURN (beginStroke (position_w, inverted));
		return;
	}

	swapStrokeLayer (false);
}

// ----------------------------------------------------------------------

void TerrainEditData::swapStrokeLayer (const bool force)
{
	if (!m_strokeActive || !m_appearance)
		return;

	int numberOfPoints = static_cast<int> (m_strokePoints.size ());
	if (numberOfPoints == m_strokePointsShown)
		return;

	if (!force && (now () - m_lastSwapTime < cs_swapIntervalMs || !isTerrainIdle ()))
		return;

	//-- shapes are built once, when the drag ends
	const bool isShape = m_strokeShape != SH_brush;
	if (isShape)
	{
		if (!force || m_strokeLayer)
			return;

		const PointList outline = buildOutline (m_strokeShape, m_strokePoints);
		if (outline.size () < 3)
		{
			const Rectangle2d dragExtent = pointExtent (m_strokePoints, 0, 0.0f);
			const char * const shapeName = m_strokeShape == SH_rectangle ? "rectangle" : m_strokeShape == SH_circle ? "circle" : "polygon";
			logEvent ("STROKE DROPPED: %s too small or too thin (%d drag points, drag covers %.1f x %.1f m)", shapeName, numberOfPoints,
				dragExtent.x1 - dragExtent.x0, dragExtent.y1 - dragExtent.y0);
			for (int i = 0; i < numberOfPoints; ++i)
				logEvent ("  d%03d %.2f %.2f", i, m_strokePoints [static_cast<size_t> (i)].x, m_strokePoints [static_cast<size_t> (i)].y);

			m_message = m_strokeShape == SH_rectangle ? "Nothing built: drag from one corner to the opposite corner (at least 1 m each way)."
				: m_strokeShape == SH_circle ? "Nothing built: drag outward from the centre (at least 1 m)."
				: "Nothing built: the outline encloses no area. Drag all the way around the area (a loop), not along a line; the shape closes itself when you let go.";
			return;
		}
		m_strokePoints = outline;
		numberOfPoints = static_cast<int> (m_strokePoints.size ());

		if (m_strokeKind == SK_flora)
			m_strokeFloraPoints = coverPoints (m_strokePoints, m_strokeFloraRadius);

		//-- water below all of the ground inside the shape would be invisible: lift it to just
		//-- above the lowest ground found (outline + inner grid), and show the new height in the panel
		if (m_strokeKind == SK_water)
		{
			float lowest = FLT_MAX;
			const int step = std::max (1, numberOfPoints / 32);
			for (int i = 0; i < numberOfPoints; i += step)
				lowest = std::min (lowest, m_appearance->generateHeight_expensive (m_strokePoints [static_cast<size_t> (i)]));
			float gridRadius = 0.0f;
			const PointList inner = coverPoints (m_strokePoints, gridRadius);
			const int innerStep = std::max (1, static_cast<int> (inner.size ()) / 32);
			for (size_t i = 0; i < inner.size (); i += static_cast<size_t> (innerStep))
				if (isInsidePolygon (inner [i], m_strokePoints))
					lowest = std::min (lowest, m_appearance->generateHeight_expensive (inner [i]));

			if (lowest < FLT_MAX && m_strokeHeight <= lowest)
			{
				logEvent ("WATER height %.1f m is under all the ground in the shape (lowest %.1f m): using %.1f m", m_strokeHeight, lowest, lowest + 1.0f);
				m_strokeHeight = lowest + 1.0f;
				m_levelHeight  = m_strokeHeight;
				char note [160];
				IGNORE_RETURN (_snprintf (note, sizeof (note), "The water height was under all the ground there, so the lake was made at %.1f m (1 m above the lowest ground).", m_strokeHeight));
				note [sizeof (note) - 1] = '\0';
				m_message = note;
				++m_revision;
			}
			else
				logEvent ("WATER height %.1f m (lowest ground in the shape %.1f m)", m_strokeHeight, lowest);
		}
	}

	//-- roads and rivers are built once, when the stroke ends: their heights are sampled from the
	//-- ground along the path, which a live preview would already have dug into or flattened
	const bool isPath = m_strokeKind == SK_road || m_strokeKind == SK_river;
	if (isPath)
	{
		if (!force || m_strokeLayer || numberOfPoints < 2)
			return;

		//-- a hand-drawn stroke wiggles every few metres; a road or river many metres wide built on
		//-- that zig-zags and overlaps itself. Resample it evenly and round it off first.
		//-- rivers: the engine makes one water tile per path section, each with its own flow
		//-- direction, so every section joint shows as a seam. Use long sections with light rounding.
		m_strokePoints = m_strokeKind == SK_river ? smoothPath (m_strokePoints, std::max (16.0f, m_radius * 2.0f), 1) : smoothPath (m_strokePoints, std::max (4.0f, m_radius), 2);
		numberOfPoints = static_cast<int> (m_strokePoints.size ());
		if (numberOfPoints < 2)
			return;
		samplePath (m_strokePoints, m_strokeSamples);
	}

	//-- smooth: built once, when the drag ends, from the ground as it is before the stroke
	if (m_strokeKind == SK_smooth)
	{
		if (!force || m_strokeLayer)
			return;
		if (!bakeSmooth ())
			return;
	}

	//-- trees: chosen once, when the drag ends, then written into the baked tree map. The area
	//-- rebuild below makes the chunks there re-create their trees from the map.
	if (m_strokeKind == SK_tree)
	{
		if (!force || m_strokeLayer)
			return;
		if (!bakeTrees ())
			return;
		setTreeTiles (m_strokeTreeTiles, m_strokeFamilyId, m_strokeTreeHeights);
	}

	//-- build the complete replacement first, then swap it in. The live layer is never changed in place.
	TerrainGenerator::Layer * const newLayer = buildStrokeLayer ();
	const Rectangle2d dirty = isShape ? pointExtent (m_strokePoints, 0, 2.0f) : isPath ? pointExtent (m_strokePoints, 0, m_radius * 1.5f) : pointExtent (m_strokePoints, m_strokePointsShown, m_radius);

	if (m_strokeLayer)
	{
		//-- the area already covered gives the same result with the new layer, so only the new part is dirty
		m_appearance->removeLayer (m_strokeLayer);
		retire (m_strokeLayer);
	}

	m_strokeLayer       = newLayer;
	m_strokePointsShown = numberOfPoints;
	m_lastSwapTime      = now ();

	//-- shapes: measure the ground in the middle before and after, to show what the fill did
	const bool measure = isShape && (m_strokeKind == SK_height || m_strokeKind == SK_roughen || m_strokeKind == SK_terrace || m_strokeKind == SK_water || m_strokeKind == SK_smooth);
	const Vector2d middle ((dirty.x0 + dirty.x1) * 0.5f, (dirty.y0 + dirty.y1) * 0.5f);
	const float before = measure ? m_appearance->generateHeight_expensive (middle) : 0.0f;

	addToTerrain (newLayer, dirty);

	if (measure)
	{
		const float after = m_appearance->generateHeight_expensive (middle);
		logEvent ("RESULT shape middle %.1f %.1f: ground %.2f m -> %.2f m (change %+.2f m)", middle.x, middle.y, before, after, after - before);
		char note [160];
		IGNORE_RETURN (_snprintf (note, sizeof (note), "Ground in the middle of the shape: %.1f m -> %.1f m (%+.1f m).", before, after, after - before));
		note [sizeof (note) - 1] = '\0';
		if (m_message.empty ())
			m_message = note;
	}
	if (m_strokeKind == SK_river || m_strokeKind == SK_water)
	{
		m_appearance->addRunTimeWater (newLayer, m_nameCounter);
		refreshSwimWater ();
	}
	m_lastAction      = "stroke layer swap";
	m_lastActionTime  = now ();
	m_lastActionCount = numberOfPoints;

	//-- baked trees and rocks: clear around the new stroke points (main thread only)
	if (m_strokeKind == SK_flora && isShape)
	{
		addFloraClearing (m_nameCounter, m_strokeFloraPoints, 0, m_strokeFloraRadius);
		m_lastAction      = "flora clearing (destroy/create trees)";
		m_lastActionTime  = now ();
		m_lastActionCount = static_cast<int> (m_strokeFloraPoints.size ());
	}
	else if (m_strokeKind == SK_flora)
	{
		addFloraClearing (m_nameCounter, m_strokePoints, m_strokeFloraPointsCleared, m_radius);
		m_lastAction      = "flora clearing (destroy/create trees)";
		m_lastActionTime  = now ();
		m_lastActionCount = numberOfPoints - m_strokeFloraPointsCleared;
		m_strokeFloraPointsCleared = numberOfPoints;
	}
}

// ----------------------------------------------------------------------

void TerrainEditData::endStroke ()
{
	IGNORE_RETURN (getAppearance ());   // cancels the stroke if the terrain was replaced
	if (!m_strokeActive)
		return;

	swapStrokeLayer (true);

	if (m_strokeLayer)
	{
		Edit edit;
		edit.m_layer     = m_strokeLayer;
		edit.m_extent    = pointExtent (m_strokePoints, 0, m_strokeShape != SH_brush ? 2.0f : (m_strokeKind == SK_road || m_strokeKind == SK_river) ? m_radius * 1.5f : m_radius);
		edit.m_points    = m_strokePoints;
		edit.m_kind      = m_strokeKind;
		edit.m_operation = m_strokeOperation;
		edit.m_height    = m_strokeHeight;
		edit.m_familyId  = m_strokeFamilyId;
		edit.m_radius    = m_radius;
		edit.m_feather   = m_feather;
		edit.m_strokeId  = m_nameCounter;
		edit.m_param     = m_strokeParam;
		edit.m_param2    = m_strokeParam2;
		edit.m_subKind   = m_strokeSubKind;
		edit.m_samples   = m_strokeSamples;
		edit.m_waterShader = m_waterShader;
		edit.m_shape       = m_strokeShape;
		edit.m_floraPoints = m_strokeShape != SH_brush ? m_strokeFloraPoints : m_strokePoints;
		edit.m_floraRadius = m_strokeShape != SH_brush ? m_strokeFloraRadius : m_radius;
		edit.m_treeTiles      = m_strokeTreeTiles;
		edit.m_treeHeights    = m_strokeTreeHeights;
		edit.m_treeOldHeights = m_strokeTreeOldHeights;
		m_edits.push_back (edit);
		logStroke (edit);
		m_unsaved = true;

		if (changesHeight (edit.m_kind))
			resnapTrees (edit.m_extent);

		//-- live swaps only rebuilt the newly painted part. Rebuild the whole stroke once so no
		//-- chunk generated during a swap is left without it.
		if (m_strokePoints.size () > 1)
		{
			markDirty (edit.m_extent);
			flushDirty ();
		}
	}

	m_strokeActive = false;
	m_strokeLayer  = 0;
	m_strokePoints.clear ();
	m_strokePointsShown = 0;
	++m_revision;
}

// ----------------------------------------------------------------------

TerrainEditData::PointList TerrainEditData::smoothPath (const PointList & points, const float spacing, const int passes)
{
	if (points.size () < 2)
		return points;

	//-- 1. resample at even spacing along the path (about the brush radius, at least 4 m)
	PointList even;
	even.push_back (points.front ());
	float carried = 0.0f;
	for (size_t i = 0; i + 1 < points.size (); ++i)
	{
		const Vector2d a = points [i];
		const Vector2d b = points [i + 1];
		const float length = a.magnitudeBetween (b);
		if (length < 0.0001f)
			continue;

		float position = spacing - carried;
		while (position <= length)
		{
			const float t = position / length;
			even.push_back (Vector2d (a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t));
			position += spacing;
		}
		carried = length - (position - spacing);
	}
	if (even.back ().magnitudeBetween (points.back ()) > spacing * 0.25f)
		even.push_back (points.back ());
	else
		even.back () = points.back ();

	if (even.size () < 3)
		return even;

	//-- 2. round the corners (Chaikin corner cutting, two passes, ends kept in place)
	PointList smooth = even;
	for (int pass = 0; pass < passes; ++pass)
	{
		PointList next;
		next.push_back (smooth.front ());
		for (size_t i = 0; i + 1 < smooth.size (); ++i)
		{
			const Vector2d & a = smooth [i];
			const Vector2d & b = smooth [i + 1];
			next.push_back (Vector2d (a.x * 0.75f + b.x * 0.25f, a.y * 0.75f + b.y * 0.25f));
			next.push_back (Vector2d (a.x * 0.25f + b.x * 0.75f, a.y * 0.25f + b.y * 0.75f));
		}
		next.push_back (smooth.back ());
		smooth.swap (next);
	}

	//-- no zero-length or near-duplicate segments (the engine's height lookup divides by their length)
	{
		PointList unique;
		unique.push_back (smooth.front ());
		for (size_t i = 1; i < smooth.size (); ++i)
			if (smooth [i].magnitudeBetweenSquared (unique.back ()) >= 1.0f)
				unique.push_back (smooth [i]);
		//-- (a path shorter than 1 m ends up with one point and is not built)
		if (unique.size () >= 2 && unique [unique.size () - 2].magnitudeBetweenSquared (smooth.back ()) >= 1.0f)
			unique.back () = smooth.back ();
		smooth.swap (unique);
	}

	//-- keep the point count sensible (the engine's path rules walk every segment per pole)
	if (smooth.size () > 200)
	{
		PointList thinned;
		const size_t step = (smooth.size () + 199) / 200;
		for (size_t i = 0; i < smooth.size (); i += step)
			thinned.push_back (smooth [i]);
		if (thinned.back ().magnitudeBetweenSquared (smooth.back ()) > 0.0001f)
			thinned.push_back (smooth.back ());
		smooth.swap (thinned);
	}

	return smooth;
}

// ----------------------------------------------------------------------

void TerrainEditData::samplePath (const PointList & points, PathSamples & samples)
{
	//-- same sampling as SOE's Terrain Editor (MapView): every segment walked in steps of about
	//-- 8 m, ground height taken at each step and at the segment's end
	samples.points.clear ();
	samples.counts.clear ();
	if (!m_appearance)
		return;

	const float desiredLength = 8.0f;
	for (size_t j = 0; j + 1 < points.size (); ++j)
	{
		const Vector2d & segmentStart = points [j];
		const Vector2d & segmentEnd   = points [j + 1];
		const float segmentLength     = segmentEnd.magnitudeBetween (segmentStart);
		//-- at least 2 samples per segment: the engine's lookup (HeightData::Segment::find) interpolates
		//-- between a segment's first and last sample and walks off the end of a 1-sample segment
		//-- (garbage heights = stretched cliffs, or a crash). SOE's long road segments never hit this.
		const int   numberOfSteps     = std::max (2, 1 + static_cast<int> (segmentLength / desiredLength));
		const float stepLength        = segmentLength / static_cast<float> (numberOfSteps);
		const Vector2d step           = segmentLength > 0.0001f ? Vector2d::normalized (segmentEnd - segmentStart) * stepLength : Vector2d (0.0f, 0.0f);

		int count = 0;
		Vector2d current = segmentStart;
		for (int i = 0; i < numberOfSteps - 1; ++i)
		{
			samples.points.push_back (Vector (current.x, m_appearance->generateHeight_expensive (current), current.y));
			++count;
			current += step;
		}

		samples.points.push_back (Vector (segmentEnd.x, m_appearance->generateHeight_expensive (segmentEnd), segmentEnd.y));
		++count;
		samples.counts.push_back (count);
	}
}

// ----------------------------------------------------------------------

std::string TerrainEditData::findWaterShader ()
{
	//-- use a water shader the planet itself uses (local water tables, rivers, global water)
	std::string result;
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	const TerrainGenerator * const generator = terrainTemplate ? terrainTemplate->getTerrainGenerator () : 0;

	struct Finder
	{
		static void scan (const TerrainGenerator::Layer * const layer, std::string & found)
		{
			for (int i = 0; found.empty () && i < layer->getNumberOfBoundaries (); ++i)
			{
				const TerrainGenerator::Boundary * const boundary = layer->getBoundary (i);
				if (boundary->getType () == TGBT_polygon)
				{
					const BoundaryPolygon * const polygon = safe_cast<const BoundaryPolygon *> (boundary);
					if (polygon->isLocalWaterTable () && polygon->getLocalWaterTableShaderTemplateName () && *polygon->getLocalWaterTableShaderTemplateName ())
						found = polygon->getLocalWaterTableShaderTemplateName ();
				}
				else if (boundary->getType () == TGBT_rectangle)
				{
					const BoundaryRectangle * const rectangle = safe_cast<const BoundaryRectangle *> (boundary);
					if (rectangle->isLocalWaterTable () && rectangle->getLocalWaterTableShaderTemplateName () && *rectangle->getLocalWaterTableShaderTemplateName ())
						found = rectangle->getLocalWaterTableShaderTemplateName ();
				}
			}

			for (int i = 0; found.empty () && i < layer->getNumberOfAffectors (); ++i)
			{
				const TerrainGenerator::Affector * const affector = layer->getAffector (i);
				if (affector->getType () == TGAT_river)
				{
					const AffectorRiver * const river = safe_cast<const AffectorRiver *> (affector);
					if (river->getHasLocalWaterTable () && river->getLocalWaterTableShaderTemplateName () && *river->getLocalWaterTableShaderTemplateName ())
						found = river->getLocalWaterTableShaderTemplateName ();
				}
			}

			for (int i = 0; found.empty () && i < layer->getNumberOfLayers (); ++i)
				scan (layer->getLayer (i), found);
		}
	};

	if (generator)
		for (int i = 0; result.empty () && i < generator->getNumberOfLayers (); ++i)
			Finder::scan (generator->getLayer (i), result);

	if (result.empty () && terrainTemplate && terrainTemplate->getGlobalWaterTableShaderTemplateName () && *terrainTemplate->getGlobalWaterTableShaderTemplateName ())
		result = terrainTemplate->getGlobalWaterTableShaderTemplateName ();

	//-- planets without any water (e.g. Tatooine): fall back to a stock animated water shader
	if (result.empty ())
	{
		const char * const candidates [] = { "shader/water_anim_bump_shader.sht", "shader/water_test.sht" };
		for (size_t i = 0; result.empty () && i < sizeof (candidates) / sizeof (candidates [0]); ++i)
			if (TreeFile::exists (candidates [i]))
				result = candidates [i];
	}

	logEvent ("WATER SHADER %s", result.empty () ? "(none found)" : result.c_str ());
	return result;
}

// ----------------------------------------------------------------------

TerrainGenerator::Layer * TerrainEditData::createPathLayer (const char * const name, const PointList & points, const StrokeKind kind, const int familyId, const float radius, const float feather, const float depth, const PathSamples & samples, const char * const waterShader)
{
	TerrainGenerator::Layer * const layer = new TerrainGenerator::Layer ();
	layer->setName (name ? name : "godclient_edit");

	AffectorBoundaryPoly * affector = 0;
	if (kind == SK_road)
	{
		AffectorRoad * const road = new AffectorRoad ();
		road->setName ("godclient_road");
		road->setFamilyId (familyId);
		road->setFeatherFunctionShader (TGFF_linear);
		road->setFeatherDistanceShader (std::min (1.0f, feather * 0.5f));
		road->setHasFixedHeights (false);
		affector = road;
	}
	else
	{
		AffectorRiver * const river = new AffectorRiver ();
		river->setName ("godclient_river");
		river->setBankFamilyId (familyId);
		river->setBottomFamilyId (familyId);
		river->setTrenchDepth (depth);
		river->setVelocity (1.0f);
		river->setWaterType (TGWT_water);
		//-- the river rule only carves the channel; its old water (one flat tile per path section,
		//-- seams at every joint) is replaced by a ribbon below
		river->setHasLocalWaterTable (false);
		affector = river;
	}

	for (PointList::const_iterator i = points.begin (); i != points.end (); ++i)
		affector->addPoint (*i);
	affector->setWidth (radius * 2.0f);
	affector->setFeatherFunction (TGFF_easeInOut);
	affector->setFeatherDistance (feather);

	//-- the baked ground heights along the path
	affector->clearHeightData ();
	size_t next = 0;
	for (size_t s = 0; s < samples.counts.size (); ++s)
	{
		affector->addSegmentHeightData ();
		for (int k = 0; k < samples.counts [s] && next < samples.points.size (); ++k)
			affector->addPointHeightData (samples.points [next++]);
	}
	affector->createHeightData ();

	layer->addAffector (affector);

	//-- river water: one continuous ribbon strip (no seams), its surface 30% of the depth below the
	//-- baked bank height at each path point. Ribbons also count as water for swimming.
	if (kind == SK_river && waterShader && *waterShader && points.size () >= 2 && !samples.counts.empty ())
	{
		AffectorRibbon * const ribbon = new AffectorRibbon ();
		ribbon->setName ("godclient_river_water");
		for (PointList::const_iterator i = points.begin (); i != points.end (); ++i)
			ribbon->addPoint (*i);
		ribbon->setWidth (radius * 2.0f * 0.8f);
		ribbon->setFeatherFunction (TGFF_linear);
		ribbon->setFeatherDistance (0.0f);
		ribbon->setRibbonWaterShaderTemplateName (waterShader);
		ribbon->setWaterShaderSize (8.0f);
		ribbon->setVelocity (1.0f);
		ribbon->setCapWidth (0.0f);
		ribbon->setWaterType (TGWT_water);
		ribbon->setTerrainShaderFamilyId (0);   // water only, the river rule paints the bed

		//-- one height per path point: the first sample of each segment, then the very last sample
		ArrayList<float> heights;
		size_t first = 0;
		for (size_t s = 0; s < samples.counts.size () && first < samples.points.size (); ++s)
		{
			heights.add (samples.points [first].y - depth * 0.3f);
			first += static_cast<size_t> (samples.counts [s]);
		}
		heights.add (samples.points.back ().y - depth * 0.3f);
		while (heights.getNumberOfElements () < static_cast<int> (points.size ()))
			heights.add (samples.points.back ().y - depth * 0.3f);
		ribbon->copyHeightList (heights);

		layer->addAffector (ribbon);
	}

	//-- like Paint: reset the planet's colour tint along the path so the texture shows cleanly
	//-- (a sub-layer, because the path rule itself has no boundary and would tint everything)
	{
		BoundaryPolyline * const boundary = new BoundaryPolyline ();
		boundary->setName ("godclient_path_tint_area");
		boundary->setFeatherFunction (TGFF_easeInOut);
		boundary->setFeatherDistance (feather);
		for (PointList::const_iterator i = points.begin (); i != points.end (); ++i)
			boundary->addPoint (*i);
		boundary->setWidth (radius);

		AffectorColorConstant * const tint = new AffectorColorConstant ();
		tint->setName ("godclient_path_neutral_tint");
		tint->setOperation (TGO_replace);
		tint->setColor (PackedRgb::solidWhite);

		TerrainGenerator::Layer * const tintLayer = new TerrainGenerator::Layer ();
		tintLayer->setName ("godclient_path_tint");
		tintLayer->addBoundary (boundary);
		tintLayer->addAffector (tint);
		tintLayer->calculateExtent ();
		layer->addLayer (tintLayer);
	}

	layer->calculateExtent ();
	return layer;
}

// ----------------------------------------------------------------------

void TerrainEditData::setCursor (const Vector2d & position_w)
{
	m_cursor    = position_w;
	m_hasCursor = true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::sampleCursorHeight (float & height)
{
	if (!m_hasCursor || getStatus () == S_noTerrain)
		return false;

	height = m_appearance->generateHeight_expensive (m_cursor);
	return true;
}

// ======================================================================
// diagnostics
// ======================================================================

namespace TerrainEditDataNamespace
{
	//-- always next to the exe: a file dialog (Save copy to folder...) can change the current folder
	const char * logFilePath ()
	{
		static char s_path [MAX_PATH] = "";
		if (!s_path [0])
		{
			char exe [MAX_PATH];
			const DWORD length = ::GetModuleFileNameA (0, exe, MAX_PATH);
			std::string folder = (length > 0 && length < MAX_PATH) ? std::string (exe, length) : std::string ();
			const std::string::size_type slash = folder.find_last_of ("\\/");
			folder = slash == std::string::npos ? std::string (".") : folder.substr (0, slash);
			IGNORE_RETURN (_snprintf (s_path, sizeof (s_path), "%s\\terrain_edit_log.txt", folder.c_str ()));
			s_path [sizeof (s_path) - 1] = '\0';
		}
		return s_path;
	}

#define cs_logFileName logFilePath ()

	const char * kindName (const int kind)
	{
		switch (kind)
		{
		case TerrainEditData::SK_paint:   return "paint";
		case TerrainEditData::SK_flora:   return "flora";
		case TerrainEditData::SK_roughen: return "roughen";
		case TerrainEditData::SK_terrace: return "terrace";
		case TerrainEditData::SK_plant:   return "plant";
		case TerrainEditData::SK_road:    return "road";
		case TerrainEditData::SK_river:   return "river";
		case TerrainEditData::SK_water:   return "water";
		case TerrainEditData::SK_smooth:  return "smooth";
		case TerrainEditData::SK_tree:    return "tree";
		case TerrainEditData::SK_height:
		default:                          return "height";
		}
	}

	const char * operationName (const TerrainGeneratorOperation operation)
	{
		switch (operation)
		{
		case TGO_add:      return "add";
		case TGO_subtract: return "subtract";
		case TGO_replace:  return "replace";
		default:           return "other";
		}
	}
}

// ----------------------------------------------------------------------

void TerrainEditData::logEvent (const char * const format, ...) const
{
	FILE * const file = fopen (cs_logFileName, "a");
	if (!file)
		return;

	fprintf (file, "[%lu] ", now ());
	va_list va;
	va_start (va, format);
	IGNORE_RETURN (vfprintf (file, format, va));
	va_end (va);
	fprintf (file, "\n");
	fclose (file);
}

// ----------------------------------------------------------------------

void TerrainEditData::logStall (const unsigned long milliseconds)
{
	FILE * const file = fopen (cs_logFileName, "a");
	if (!file)
		return;

	fprintf (file, "STALL %lu ms | stroke=%s kind=%d points=%d | idle=%s pendingRebuild=%s | last action: %s (%d) %lu ms before the stall ended\n",
		milliseconds,
		m_strokeActive ? "active" : "none",
		static_cast<int> (m_strokeKind),
		static_cast<int> (m_strokePoints.size ()),
		isTerrainIdle () ? "yes" : "no",
		m_hasPendingDirty ? "yes" : "no",
		m_lastAction ? m_lastAction : "?",
		m_lastActionCount,
		now () - m_lastActionTime);

	fclose (file);
}

// ----------------------------------------------------------------------

void TerrainEditData::logStroke (const Edit & edit) const
{
	FILE * const file = fopen (cs_logFileName, "a");
	if (!file)
		return;

	float length = 0.0f;
	float maxStep = 0.0f;
	for (size_t i = 1; i < edit.m_points.size (); ++i)
	{
		const float step = edit.m_points [i - 1].magnitudeBetween (edit.m_points [i]);
		length += step;
		maxStep = std::max (maxStep, step);
	}

	fprintf (file, "STROKE %s | kind=%s op=%s height=%.2f family=%d radius=%.1f feather=%.2f points=%d length=%.1f maxStep=%.1f\n",
		edit.m_layer->getName () ? edit.m_layer->getName () : "?",
		kindName (edit.m_kind),
		operationName (edit.m_operation), edit.m_height, edit.m_familyId, edit.m_radius, edit.m_feather,
		static_cast<int> (edit.m_points.size ()), length, maxStep);

	for (size_t j = 0; j < edit.m_points.size (); ++j)
		fprintf (file, "  p%03d %.2f %.2f\n", static_cast<int> (j), edit.m_points [j].x, edit.m_points [j].y);

	fclose (file);
}

// ----------------------------------------------------------------------

void TerrainEditData::diagnoseSurfaceEdit (const Edit & edit, char * const summary, const int summarySize)
{
	//-- runs the terrain generator (with every layer, ours included) on a small patch around each
	//-- checked stroke point and reports what it produces there: ground texture family for Paint,
	//-- flora families for Clear flora. Also reports what the live terrain has at the same spot.
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ());
	const TerrainGenerator * const generator = terrainTemplate ? terrainTemplate->getTerrainGenerator () : 0;
	if (!generator)
	{
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize), "No terrain generator."));
		summary [summarySize - 1] = '\0';
		return;
	}

	const int   originOffset  = terrainTemplate->getChunkOriginOffset ();
	const int   upperPad      = terrainTemplate->getChunkUpperPad ();
	const int   inner         = 4;
	const int   numberOfPoles = originOffset + inner + 1 + upperPad;
	const float spacing       = 1.0f;
	const int   centre        = originOffset + inner / 2;

	TerrainGenerator::CreateChunkBuffer buffer;
	buffer.allocate (numberOfPoles);

	FILE * const file = fopen (cs_logFileName, "a");
	if (file)
		fprintf (file, "CHECK %s | kind=%s family=%d (%s) legacy=%d originOffset=%d upperPad=%d layers=%d\n",
			edit.m_layer->getName () ? edit.m_layer->getName () : "?", kindName (edit.m_kind),
			edit.m_familyId, edit.m_kind == SK_paint ? generator->getShaderGroup ().getFamilyName (edit.m_familyId) : "-",
			terrainTemplate->getLegacyMode () ? 1 : 0, originOffset, upperPad, generator->getNumberOfLayers ());

	const int numberOfPoints = static_cast<int> (edit.m_points.size ());
	const int checks = std::min (numberOfPoints, 5);
	int matches = 0;
	int floraLeft = 0;

	for (int c = 0; c < checks; ++c)
	{
		const int index = checks > 1 ? (c * (numberOfPoints - 1)) / (checks - 1) : 0;
		const Vector2d & p = edit.m_points [static_cast<size_t> (index)];

		TerrainGenerator::GeneratorChunkData data (terrainTemplate->getLegacyMode ());
		data.shaderGroup                 = &generator->getShaderGroup ();
		data.floraGroup                  = &generator->getFloraGroup ();
		data.radialGroup                 = &generator->getRadialGroup ();
		data.environmentGroup            = &generator->getEnvironmentGroup ();
		data.fractalGroup                = &generator->getFractalGroup ();
		data.bitmapGroup                 = &generator->getBitmapGroup ();
		data.heightMap                   = &buffer.heightMap;
		data.colorMap                    = &buffer.colorMap;
		data.shaderMap                   = &buffer.shaderMap;
		data.floraStaticCollidableMap    = &buffer.floraStaticCollidableMap;
		data.floraStaticNonCollidableMap = &buffer.floraStaticNonCollidableMap;
		data.floraDynamicNearMap         = &buffer.floraDynamicNearMap;
		data.floraDynamicFarMap          = &buffer.floraDynamicFarMap;
		data.environmentMap              = &buffer.environmentMap;
		data.vertexPositionMap           = &buffer.vertexPositionMap;
		data.vertexNormalMap             = &buffer.vertexNormalMap;
		data.excludeMap                  = &buffer.excludeMap;
		data.passableMap                 = &buffer.passableMap;
		data.start                       = Vector (p.x - static_cast<float> (centre) * spacing, 0.0f, p.y - static_cast<float> (centre) * spacing);
		data.originOffset                = originOffset;
		data.upperPad                    = upperPad;
		data.numberOfPoles               = numberOfPoles;
		data.distanceBetweenPoles        = spacing;

		generator->generateChunk (data);

		const int shaderFamily  = buffer.shaderMap.getData (centre, centre).getFamilyId ();
		const int collidable    = buffer.floraStaticCollidableMap.getData (centre, centre).getFamilyId ();
		const int nonCollidable = buffer.floraStaticNonCollidableMap.getData (centre, centre).getFamilyId ();
		const int nearRadial    = buffer.floraDynamicNearMap.getData (centre, centre).getFamilyId ();
		const int farRadial     = buffer.floraDynamicFarMap.getData (centre, centre).getFamilyId ();

		if (edit.m_kind == SK_paint && shaderFamily == edit.m_familyId)
			++matches;
		if (edit.m_kind == SK_flora && (collidable || nonCollidable || nearRadial || farRadial))
			++floraLeft;
		if (edit.m_kind == SK_plant && ((edit.m_subKind == PK_radial ? nearRadial : nonCollidable) == edit.m_familyId))
			++matches;

		//-- the live terrain at the same spot
		const Vector position (p.x, 0.0f, p.y);
		//-- (not findStaticCollidableFlora: it crashed here; client chunks may have no tree map)
		const BoxExtent * const chunkExtent = m_appearance->getChunkExtent (position);

		//-- what the live (on-screen) chunk holds for bushes and grass at this spot
		ClientProceduralTerrainAppearance::StaticFloraData liveStatic;
		ClientProceduralTerrainAppearance::DynamicFloraData liveNear;
		bool allowedStatic = false;
		bool allowedNear = false;
		const bool hasLiveStatic = m_appearance->findStaticNonCollidableFlora (p.x, p.y, liveStatic, allowedStatic);
		const bool hasLiveNear   = m_appearance->findDynamicNearFlora (p.x, p.y, liveNear, allowedNear);

		//-- regenerate the on-screen chunk exactly the way the client does (same start, pole count
		//-- and spacing as createClientChunk) and read the pole nearest the stroke point
		if (chunkExtent && file)
		{
			const float chunkWidth   = terrainTemplate->getChunkWidthInMeters ();
			const float tileWidth2   = terrainTemplate->getTileWidthInMeters ();
			const int   tilesPerChunk = terrainTemplate->getNumberOfTilesPerChunk ();
			const float liveWidth    = chunkExtent->getMax ().x - chunkExtent->getMin ().x;
			const int   chunkSize    = std::max (1, static_cast<int> (floorf (liveWidth / chunkWidth + 0.5f)));
			const float poleSpacing  = tileWidth2 * 0.5f * static_cast<float> (chunkSize);
			const int   chunkPoles   = 2 * tilesPerChunk + originOffset + upperPad;
			const float chunkX0      = floorf (chunkExtent->getMin ().x / chunkWidth + 0.5f) * chunkWidth;
			const float chunkZ0      = floorf (chunkExtent->getMin ().z / chunkWidth + 0.5f) * chunkWidth;

			TerrainGenerator::CreateChunkBuffer chunkBuffer;
			chunkBuffer.allocate (chunkPoles);

			TerrainGenerator::GeneratorChunkData cd (terrainTemplate->getLegacyMode ());
			cd.shaderGroup                 = &generator->getShaderGroup ();
			cd.floraGroup                  = &generator->getFloraGroup ();
			cd.radialGroup                 = &generator->getRadialGroup ();
			cd.environmentGroup            = &generator->getEnvironmentGroup ();
			cd.fractalGroup                = &generator->getFractalGroup ();
			cd.bitmapGroup                 = &generator->getBitmapGroup ();
			cd.heightMap                   = &chunkBuffer.heightMap;
			cd.colorMap                    = &chunkBuffer.colorMap;
			cd.shaderMap                   = &chunkBuffer.shaderMap;
			cd.floraStaticCollidableMap    = &chunkBuffer.floraStaticCollidableMap;
			cd.floraStaticNonCollidableMap = &chunkBuffer.floraStaticNonCollidableMap;
			cd.floraDynamicNearMap         = &chunkBuffer.floraDynamicNearMap;
			cd.floraDynamicFarMap          = &chunkBuffer.floraDynamicFarMap;
			cd.environmentMap              = &chunkBuffer.environmentMap;
			cd.vertexPositionMap           = &chunkBuffer.vertexPositionMap;
			cd.vertexNormalMap             = &chunkBuffer.vertexNormalMap;
			cd.excludeMap                  = &chunkBuffer.excludeMap;
			cd.passableMap                 = &chunkBuffer.passableMap;
			cd.start                       = Vector (chunkX0 - static_cast<float> (originOffset) * poleSpacing, 0.0f, chunkZ0 - static_cast<float> (originOffset) * poleSpacing);
			cd.originOffset                = originOffset;
			cd.upperPad                    = upperPad;
			cd.numberOfPoles               = chunkPoles;
			cd.distanceBetweenPoles        = poleSpacing;

			generator->generateChunk (cd);

			const int px = std::max (0, std::min (chunkPoles - 1, static_cast<int> (floorf ((p.x - cd.start.x) / poleSpacing + 0.5f))));
			const int pz = std::max (0, std::min (chunkPoles - 1, static_cast<int> (floorf ((p.y - cd.start.z) / poleSpacing + 0.5f))));

			//-- count painted poles over the whole chunk too
			int painted = 0;
			for (int zz = 0; zz < chunkPoles; ++zz)
				for (int xx = 0; xx < chunkPoles; ++xx)
					if (chunkBuffer.shaderMap.getData (xx, zz).getFamilyId () == edit.m_familyId)
						++painted;

			float liveHeight = 0.0f;
			const bool hasLiveHeight = m_appearance->getHeight (Vector (p.x, 0.0f, p.y), liveHeight);

			fprintf (file, "    chunk-layout: min=(%.1f,%.1f) width=%.0f size=%d poles=%d spacing=%.2f start=(%.1f,%.1f) pole=(%d,%d) shader=%d nonCollidable=%d radialNear=%d height=%.2f | live height=%s%.2f | painted poles %d/%d\n",
				chunkExtent->getMin ().x, chunkExtent->getMin ().z, liveWidth, chunkSize, chunkPoles, poleSpacing, cd.start.x, cd.start.z, px, pz,
				chunkBuffer.shaderMap.getData (px, pz).getFamilyId (),
				chunkBuffer.floraStaticNonCollidableMap.getData (px, pz).getFamilyId (),
				chunkBuffer.floraDynamicNearMap.getData (px, pz).getFamilyId (),
				chunkBuffer.heightMap.getData (px, pz),
				hasLiveHeight ? "" : "n/a ", liveHeight, painted, chunkPoles * chunkPoles);
			fflush (file);
		}

		//-- baked tree map tile under the point
		const float tileWidth = 16.0f;
		const int   mapWidthInTiles = static_cast<int> (m_appearance->getMapWidthInMeters () / tileWidth);
		const int   keyX = static_cast<int> (floorf (p.x / tileWidth)) + mapWidthInTiles / 2;
		const int   keyZ = static_cast<int> (floorf (p.y / tileWidth)) + mapWidthInTiles / 2;
		const int   bakedFamily = terrainTemplate->getStaticCollidableFloraFamily (keyX, keyZ);

		if (file)
			fprintf (file, "  p%03d x=%.2f z=%.2f  generated: shader=%d (%s) color=%d,%d,%d collidable=%d noncollidable=%d radialNear=%d radialFar=%d | live chunk=%s %.0fm liveBush=%d(%d) liveGrass=%d(%d) | baked tree tile=%d\n",
				index, p.x, p.y, shaderFamily, generator->getShaderGroup ().hasFamily (shaderFamily) ? generator->getShaderGroup ().getFamilyName (shaderFamily) : "?",
				buffer.colorMap.getData (centre, centre).r, buffer.colorMap.getData (centre, centre).g, buffer.colorMap.getData (centre, centre).b,
				collidable, nonCollidable, nearRadial, farRadial,
				chunkExtent ? "yes" : "no", chunkExtent ? chunkExtent->getMax ().x - chunkExtent->getMin ().x : 0.0f,
				hasLiveStatic ? 1 : 0, allowedStatic ? 1 : 0, hasLiveNear ? 1 : 0, allowedNear ? 1 : 0,
				bakedFamily);
		if (file)
			fflush (file);
	}

	if (edit.m_kind == SK_paint)
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize),
			"Paint check: the terrain generator gives the painted texture at %d of %d stroke points.\n%s",
			matches, checks, matches == checks ? "Generator is correct, so the problem is on-screen refresh." : "Generator does NOT produce the paint: see terrain_edit_log.txt."));
	else if (edit.m_kind == SK_plant)
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize),
			"Plant check: the chosen plant is placed at %d of %d stroke points (fewer is normal at low density).\nDetails in terrain_edit_log.txt.", matches, checks));
	else
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize),
			"Flora check: generator still has flora at %d of %d stroke points (should be 0).\nDetails in terrain_edit_log.txt.", floraLeft, checks));
	summary [summarySize - 1] = '\0';

	if (file)
	{
		fprintf (file, "RESULT %s\n\n", summary);
		fclose (file);
	}
}

// ----------------------------------------------------------------------

void TerrainEditData::diagnoseLastEdit (char * const summary, const int summarySize)
{
	if (!summary || summarySize <= 0)
		return;

	summary [0] = '\0';

	if (!getAppearance ())
	{
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize), "No terrain loaded."));
		summary [summarySize - 1] = '\0';
		return;
	}

	if (m_edits.empty ())
	{
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize), "No strokes to check yet."));
		summary [summarySize - 1] = '\0';
		return;
	}

	const Edit & edit = m_edits.back ();
	if ((edit.m_shape != SH_brush || edit.m_kind == SK_water) && edit.m_kind != SK_tree && edit.m_kind != SK_smooth)
	{
		//-- shapes: the outline, the ground in the middle, and (water) whether the swim list knows it
		const Rectangle2d extent = pointExtent (edit.m_points, 0, 0.0f);
		const Vector2d centre ((extent.x0 + extent.x1) * 0.5f, (extent.y0 + extent.y1) * 0.5f);
		const float ground = m_appearance->generateHeight_expensive (centre);
		float waterHeight = 0.0f;
		const ProceduralTerrainAppearanceTemplate * const terrainTemplate = dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ());
		const bool hasWater = terrainTemplate && terrainTemplate->getWaterHeight (Vector (centre.x, 0.0f, centre.y), waterHeight);
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize), "%s shape: %d outline points, %.0f x %.0f m. Middle: ground %.1f m, water %s %.1f m.%s",
			edit.m_kind == SK_water ? "Water" : "Fill", static_cast<int> (edit.m_points.size ()), extent.x1 - extent.x0, extent.y1 - extent.y0,
			ground, hasWater ? "at" : "none", hasWater ? waterHeight : 0.0f,
			edit.m_kind == SK_water && hasWater && waterHeight <= ground ? " The water is below the ground here: pick a higher Water height." : ""));
		summary [summarySize - 1] = '\0';
		logEvent ("CHECK %s", summary);
		return;
	}

	if (edit.m_kind == SK_tree)
	{
		//-- read the planted tiles back from the template's live map
		const ProceduralTerrainAppearanceTemplate * const treeTemplate = dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ());
		int inMap = 0;
		const int width = treeTemplate ? treeTemplate->getMapWidthInFlora () : 0;
		for (size_t i = 0; width > 0 && i < edit.m_treeTiles.size (); ++i)
			if (treeTemplate->getStaticCollidableFloraFamily (edit.m_treeTiles [i] % width, edit.m_treeTiles [i] / width) == edit.m_familyId)
				++inMap;
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize), "Tree stroke: %d trees planted, %d found in the live baked map (family %d).", static_cast<int> (edit.m_treeTiles.size ()), inMap, edit.m_familyId));
		summary [summarySize - 1] = '\0';
		logEvent ("CHECK %s", summary);
		return;
	}

	if (edit.m_kind == SK_smooth)
	{
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize), "Smooth stroke: %d ground points baked (patches %.1f m wide).", static_cast<int> (edit.m_samples.points.size ()), edit.m_param2 * 2.0f));
		summary [summarySize - 1] = '\0';
		return;
	}

	if (edit.m_kind == SK_road || edit.m_kind == SK_river)
	{
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize), "Road/River stroke: %d path points, %d ground samples baked.%s", static_cast<int> (edit.m_points.size ()), static_cast<int> (edit.m_samples.points.size ()),
			edit.m_kind == SK_river ? (edit.m_waterShader.empty () ? " No water shader found, so no water surface." : (std::string (" Water: ") + edit.m_waterShader).c_str ()) : ""));
		summary [summarySize - 1] = '\0';
		return;
	}

	if (edit.m_kind == SK_paint || edit.m_kind == SK_flora || edit.m_kind == SK_plant)
	{
		diagnoseSurfaceEdit (edit, summary, summarySize);
		return;
	}

	const bool isReplace = edit.m_operation == TGO_replace;
	const float core = edit.m_radius * (1.0f - edit.m_feather);   // fully affected half-width
	const float offsets [] = { -0.66f, -0.33f, 0.0f, 0.33f, 0.66f };

	FILE * const file = fopen (cs_logFileName, "a");
	if (file)
		fprintf (file, "CHECK %s | target=%.2f core half-width=%.1f\n", edit.m_layer->getName () ? edit.m_layer->getName () : "?", edit.m_height, core);

	float minValue = FLT_MAX;
	float maxValue = -FLT_MAX;
	double sumAbsDeviation = 0.0;
	int samples = 0;

	const int numberOfPoints = static_cast<int> (edit.m_points.size ());
	for (int i = 0; i < numberOfPoints; ++i)
	{
		//-- direction along the stroke at this point, and the perpendicular across it
		const Vector2d & here = edit.m_points [static_cast<size_t> (i)];
		const Vector2d & prev = edit.m_points [static_cast<size_t> (std::max (0, i - 1))];
		const Vector2d & next = edit.m_points [static_cast<size_t> (std::min (numberOfPoints - 1, i + 1))];
		Vector2d along (next.x - prev.x, next.y - prev.y);
		const float alongLength = along.magnitude ();
		if (alongLength > 0.001f)
			along = Vector2d (along.x / alongLength, along.y / alongLength);
		else
			along = Vector2d (1.0f, 0.0f);
		const Vector2d across (-along.y, along.x);

		for (size_t k = 0; k < sizeof (offsets) / sizeof (offsets [0]); ++k)
		{
			const Vector2d sample (here.x + across.x * offsets [k] * core, here.y + across.y * offsets [k] * core);
			const float height = m_appearance->generateHeight_expensive (sample);
			const float value = isReplace ? height - edit.m_height : height;
			minValue = std::min (minValue, value);
			maxValue = std::max (maxValue, value);
			sumAbsDeviation += fabs (value);
			++samples;

			if (file)
				fprintf (file, "  p%03d off%+.2f  x=%.2f z=%.2f  height=%.3f%s\n", i, offsets [k], sample.x, sample.y, height,
					isReplace ? "" : "");
		}
	}

	if (isReplace)
	{
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize),
			"Last stroke (%d points): middle should be %.2f m.\nMeasured %+.2f to %+.2f m off (avg %.2f) over %d samples.\n%s",
			numberOfPoints, edit.m_height, minValue, maxValue, samples ? sumAbsDeviation / samples : 0.0, samples,
			(maxValue - minValue) < 0.05f ? "Generated ground is level: any bumps are a display problem." : "Generated ground is NOT level."));
	}
	else
	{
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize),
			"Last stroke (%d points, %s %.2f m): ground in the middle is %.2f to %.2f m.",
			numberOfPoints, operationName (edit.m_operation), edit.m_height, minValue, maxValue));
	}
	summary [summarySize - 1] = '\0';

	if (file)
	{
		fprintf (file, "RESULT %s\n\n", summary);
		fclose (file);
	}
}

// ======================================================================
// saving
// ======================================================================

namespace TerrainEditDataNamespace
{
	void setMessage (char * const buffer, const int size, const char * const format, ...)
	{
		if (!buffer || size <= 0)
			return;

		va_list va;
		va_start (va, format);
		IGNORE_RETURN (_vsnprintf (buffer, static_cast<size_t> (size), format, va));
		va_end (va);
		buffer [size - 1] = '\0';
	}

	std::string directoryOf (const std::string & path)
	{
		const std::string::size_type slash = path.find_last_of ("/\\");
		return slash == std::string::npos ? std::string (".") : path.substr (0, slash);
	}

	std::string baseNameOf (const std::string & path)
	{
		const std::string::size_type slash = path.find_last_of ("/\\");
		std::string name = slash == std::string::npos ? path : path.substr (slash + 1);
		const std::string::size_type dot = name.find_last_of ('.');
		return dot == std::string::npos ? name : name.substr (0, dot);
	}

	bool isFileOnDisk (const char * const path)
	{
		const DWORD attributes = ::GetFileAttributesA (path);
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	const ProceduralTerrainAppearanceTemplate * getTerrainTemplate (ClientProceduralTerrainAppearance * const appearance)
	{
		return appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (appearance->getAppearanceTemplate ()) : 0;
	}
}

// ----------------------------------------------------------------------

bool TerrainEditData::hasUnsavedEdits () const
{
	return m_unsaved;
}

// ----------------------------------------------------------------------

bool TerrainEditData::getTerrainFileName (char * const buffer, const int bufferSize)
{
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = getTerrainTemplate (getAppearance ());
	if (!terrainTemplate || !buffer || bufferSize <= 0)
		return false;

	//-- the base class name is the file the planet was loaded from (e.g. terrain/tatooine.trn);
	//-- ProceduralTerrainAppearanceTemplate::getName hides it with the name stored inside the file
	const char * const fileName = static_cast<const AppearanceTemplate *> (terrainTemplate)->getName ();
	if (!fileName || !*fileName)
		return false;

	setMessage (buffer, bufferSize, "%s", fileName);
	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::getDefaultSavePath (char * const buffer, const int bufferSize, bool & existsOnDisk)
{
	existsOnDisk = false;

	char fileName [512];
	if (!getTerrainFileName (fileName, sizeof (fileName)))
		return false;

	//-- if the planet came from a loose file, save over that file
	char found [1024];
	if (TreeFile::getPathName (fileName, found, sizeof (found)) && isFileOnDisk (found))
	{
		existsOnDisk = true;
		setMessage (buffer, bufferSize, "%s", found);
		return true;
	}

	//-- otherwise (it came from a .tre) write into the highest-priority loose folder, which overrides the .tre
	const char * const root = TreeFile::getNumberOfSearchPaths () > 0 ? TreeFile::getSearchPath (0) : 0;
	if (!root)
		return false;

	setMessage (buffer, bufferSize, "%s/%s", root, fileName);
	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::loadOriginal (char * const message, const int messageSize)
{
	char fileName [512];
	if (!getTerrainFileName (fileName, sizeof (fileName)))
	{
		setMessage (message, messageSize, "No ground terrain is loaded.");
		return false;
	}

	if (!TreeFile::exists (fileName))
	{
		setMessage (message, messageSize, "Cannot find %s in the client's data folders.", fileName);
		return false;
	}

	//-- read only the terrain rules (the generator) from the file as it is on disk now; the live
	//-- terrain also holds run-time layers (building flattening) that must not be saved
	Iff iff (fileName);
	const Tag form = iff.getCurrentName ();
	if (form != TAG (P,T,A,T) && form != TAG (M,P,T,A))
	{
		setMessage (message, messageSize, "%s is not a procedural terrain file.", fileName);
		return false;
	}

	iff.enterForm ();
	const Tag version = iff.getCurrentName ();
	if (version < TAG_0013 || version > TAG_0015)
	{
		setMessage (message, messageSize, "%s uses an unsupported terrain file version.", fileName);
		return false;
	}

	iff.enterForm (version);
		iff.enterChunk (TAG_DATA);
		iff.exitChunk (TAG_DATA, true);

		TerrainGenerator * const generator = new TerrainGenerator ();
		generator->load (iff);
	iff.exitForm (version, true);
	iff.exitForm (true);

	delete m_originalGenerator;
	m_originalGenerator = generator;
	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::writeTerrain (const char * const diskPath, char * const message, const int messageSize)
{
	ClientProceduralTerrainAppearance * const appearance = getAppearance ();
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = getTerrainTemplate (appearance);
	if (!terrainTemplate)
	{
		setMessage (message, messageSize, "No ground terrain is loaded.");
		return false;
	}

	if (!m_originalGenerator && !loadOriginal (message, messageSize))
		return false;

	//-- original rules + clones of this session's strokes (the live layers stay untouched)
	LayerList clones;
	for (EditList::const_iterator i = m_edits.begin (); i != m_edits.end (); ++i)
	{
		if (i->m_kind == SK_tree)
			continue;   // trees are in the baked tree map, written below as the template holds it now

		TerrainGenerator::Layer * const clone = createLayer (i->m_layer->getName (), i->m_points, i->m_kind, i->m_operation, i->m_height, i->m_familyId, i->m_radius, i->m_feather, i->m_param, i->m_param2, i->m_subKind, i->m_samples, i->m_waterShader.c_str (), i->m_shape);
		m_originalGenerator->addLayer (clone);
		clones.push_back (clone);
	}

	ProceduralTerrainAppearanceTemplate::WriterData data;
	const_cast<ProceduralTerrainAppearanceTemplate *> (terrainTemplate)->prepareWriterData (data);
	data.collidableMaximumDistance = terrainTemplate->getCollidableMaximumDistance ();   // prepareWriterData copies the non-collidable value by mistake
	data.terrainGenerator          = m_originalGenerator;

	//-- Clear flora strokes: blank the cleared tiles in a copy of the baked tree/rock map
	PackedIntegerMap * clearedFloraMap = 0;
	{
		bool hasFloraEdits = false;
		for (EditList::const_iterator f = m_edits.begin (); f != m_edits.end (); ++f)
			if (f->m_kind == SK_flora)
				hasFloraEdits = true;

		if (hasFloraEdits && data.staticCollidableFloraMap)
		{
			clearedFloraMap = buildClearedFloraMap (*data.staticCollidableFloraMap);
			if (clearedFloraMap)
				data.staticCollidableFloraMap = clearedFloraMap;
		}
	}

	bool ok = false;
	{
		const std::string target (diskPath);
		const std::string temporary (target + ".tmp");
		IGNORE_RETURN (Os::createDirectories (directoryOf (target).c_str ()));

		Iff iff (4 * 1024 * 1024, true);
		ProceduralTerrainAppearanceTemplate::write (iff, data);

		//-- write next to the target, then swap it in, so a failed write never leaves half a file
		if (iff.write (temporary.c_str (), true) && ::MoveFileExA (temporary.c_str (), target.c_str (), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			ok = true;
		else
		{
			IGNORE_RETURN (::DeleteFileA (temporary.c_str ()));
			setMessage (message, messageSize, "Could not write %s (is the folder read-only or the file open elsewhere?).", target.c_str ());
		}
	}

	for (LayerList::iterator j = clones.begin (); j != clones.end (); ++j)
		m_originalGenerator->removeLayer (*j, true);

	delete clearedFloraMap;
	return ok;
}

// ----------------------------------------------------------------------

bool TerrainEditData::save (char * const message, const int messageSize)
{
	if (m_strokeActive)
		endStroke ();

	char path [1024];
	bool existsOnDisk = false;
	if (!getDefaultSavePath (path, sizeof (path), existsOnDisk))
	{
		setMessage (message, messageSize, "Not on a ground planet, or no loose data folder is configured.");
		return false;
	}

	//-- the in-memory original must be read before the file on disk is replaced
	if (!m_originalGenerator && !loadOriginal (message, messageSize))
		return false;

	//-- back up the file once per session before overwriting it
	std::string backup;
	if (existsOnDisk && !m_backupDone)
	{
		SYSTEMTIME now;
		::GetLocalTime (&now);
		char stamp [32];
		setMessage (stamp, sizeof (stamp), "%04d%02d%02d_%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

		const std::string folder (directoryOf (path) + "/_terrain_backup");
		IGNORE_RETURN (Os::createDirectories (folder.c_str ()));
		backup = folder + "/" + baseNameOf (path) + "_" + stamp + ".trn";

		if (!::CopyFileA (path, backup.c_str (), TRUE))
		{
			setMessage (message, messageSize, "Not saved: could not back up %s to %s.", path, backup.c_str ());
			return false;
		}
		m_backupDone = true;
	}

	if (!writeTerrain (path, message, messageSize))
		return false;

	m_unsaved = false;
	++m_revision;

	if (!backup.empty ())
		setMessage (message, messageSize, "Saved %d stroke(s) to %s\nBackup of the previous file: %s", getNumberOfEdits (), path, backup.c_str ());
	else
		setMessage (message, messageSize, "Saved %d stroke(s) to %s%s", getNumberOfEdits (), path,
			existsOnDisk ? "" : "\n(new loose file: it overrides the copy inside the .tre archives; delete it to go back)");
	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::saveCopyTo (const char * const folder, char * const message, const int messageSize)
{
	if (m_strokeActive)
		endStroke ();

	char fileName [512];
	if (!folder || !*folder || !getTerrainFileName (fileName, sizeof (fileName)))
	{
		setMessage (message, messageSize, "Not on a ground planet.");
		return false;
	}

	//-- keep the terrain/... part so the folder can be dropped into a client or server data folder
	const std::string path = std::string (folder) + "/" + fileName;
	if (!writeTerrain (path.c_str (), message, messageSize))
		return false;

	setMessage (message, messageSize, "Saved a copy with %d stroke(s) to %s", getNumberOfEdits (), path.c_str ());
	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::revertToSaved ()
{
	if (m_strokeActive)
		endStroke ();

	GroundScene * const groundScene = dynamic_cast<GroundScene *> (Game::getScene ());
	if (!groundScene || !getAppearance ())
		return false;

	//-- reloads the planet from disk; the edit list starts again with the new terrain
	groundScene->reloadTerrain ();
	IGNORE_RETURN (getAppearance ());
	++m_revision;
	return true;
}

// ======================================================================
// history
// ======================================================================

bool TerrainEditData::undo ()
{
	if (m_strokeActive)
		endStroke ();

	if (!getAppearance () || m_edits.empty ())
		return false;

	const Edit edit = m_edits.back ();
	m_edits.pop_back ();
	if (edit.m_kind == SK_tree)
		setTreeTiles (edit.m_treeTiles, 0, edit.m_treeOldHeights);   // before the rebuild, so it sees the old map
	removeFromTerrain (edit.m_layer, edit.m_extent);
	if (changesHeight (edit.m_kind))
		resnapTrees (edit.m_extent);
	if (edit.m_kind == SK_flora)
		removeFloraClearing (edit.m_strokeId, static_cast<int> (edit.m_floraPoints.size ()));
	if (edit.m_kind == SK_river || edit.m_kind == SK_water)
	{
		m_appearance->removeRunTimeWater (edit.m_strokeId);
		refreshSwimWater ();
	}
	m_redo.push_back (edit);
	m_unsaved = true;
	++m_revision;
	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::redo ()
{
	if (m_strokeActive)
		endStroke ();

	if (!getAppearance () || m_redo.empty ())
		return false;

	const Edit edit = m_redo.back ();
	m_redo.pop_back ();
	if (edit.m_kind == SK_tree)
		setTreeTiles (edit.m_treeTiles, edit.m_familyId, edit.m_treeHeights);
	addToTerrain (edit.m_layer, edit.m_extent);
	if (changesHeight (edit.m_kind))
		resnapTrees (edit.m_extent);
	if (edit.m_kind == SK_flora)
		addFloraClearing (edit.m_strokeId, edit.m_floraPoints, 0, edit.m_floraRadius);
	if (edit.m_kind == SK_river || edit.m_kind == SK_water)
	{
		m_appearance->addRunTimeWater (edit.m_layer, edit.m_strokeId);
		refreshSwimWater ();
	}
	m_edits.push_back (edit);
	m_unsaved = true;
	++m_revision;
	return true;
}

// ----------------------------------------------------------------------

void TerrainEditData::removeAll ()
{
	if (m_strokeActive)
		endStroke ();

	if (!getAppearance ())
		return;

	if (m_edits.empty ())
		return;

	//-- trees: restore the baked map newest first
	for (EditList::reverse_iterator t = m_edits.rbegin (); t != m_edits.rend (); ++t)
		if (t->m_kind == SK_tree)
			setTreeTiles (t->m_treeTiles, 0, t->m_treeOldHeights);

	Rectangle2d dirty = m_edits.front ().m_extent;
	for (EditList::iterator i = m_edits.begin (); i != m_edits.end (); ++i)
	{
		dirty.expand (i->m_extent);
		m_appearance->removeLayer (i->m_layer);
		retire (i->m_layer);
		if (i->m_kind == SK_flora)
			removeFloraClearing (i->m_strokeId, static_cast<int> (i->m_floraPoints.size ()));
		if (i->m_kind == SK_river || i->m_kind == SK_water)
			m_appearance->removeRunTimeWater (i->m_strokeId);
	}
	m_edits.clear ();
	refreshSwimWater ();
	clearRedo ();
	m_unsaved = true;

	m_appearance->prepareGenerator ();
	m_lastTerrainChangeTime = now ();
	markDirty (dirty);
	flushDirty ();
	resnapTrees (dirty);
	++m_revision;
}

// ----------------------------------------------------------------------

void TerrainEditData::tick ()
{
	IGNORE_RETURN (getAppearance ());
	updateIdle ();

	//-- did the last requested rebuild land?
	if (m_watching && m_appearance)
	{
		const void * const chunk = m_appearance->getChunkExtent (Vector (m_watchPoint.x, 0.0f, m_watchPoint.y));
		if (chunk != m_watchChunk)
		{
			logEvent ("REBUILT after %lu ms (chunk %p -> %p)", now () - m_watchTime, m_watchChunk, chunk);
			m_watching = false;
		}
		else if (now () - m_watchTime > 8000)
		{
			logEvent ("NOT REBUILT after 8 s (chunk %p unchanged, stabilized=%s)", chunk, m_appearance->terrainGenerationStabilized () ? "yes" : "no");
			m_watching = false;
		}
	}

	//-- request the waiting on-screen rebuild as soon as terrain generation is idle
	flushDirty ();

	//-- keep the live preview moving even if the mouse pauses
	if (m_strokeActive)
	{
		swapStrokeLayer (false);
		return;
	}

	//-- delete retired layers once the terrain worker cannot still be reading them
	if (!m_graveyard.empty () && m_appearance && now () - m_lastTerrainChangeTime > cs_settleDelayMs && m_appearance->terrainGenerationStabilized ())
	{
		for (LayerList::iterator i = m_graveyard.begin (); i != m_graveyard.end (); ++i)
			delete *i;
		m_graveyard.clear ();
	}
}

// ======================================================================

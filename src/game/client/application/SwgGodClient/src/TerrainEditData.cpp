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
	m_brushMode (false),
	m_appearance (0),
	m_appearanceTemplate (0),
	m_strokeActive (false),
	m_strokeInverted (false),
	m_strokeKind (SK_height),
	m_strokeFamilyId (0),
	m_strokeFamilyName (),
	m_strokeFloraPointsCleared (0),
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

	case SK_height:
	default:
		if (m_strokeOperation == TGO_replace)
			IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d %s to %.1f m (r %.0f)", m_nameCounter, toolName (m_tool, m_strokeInverted), m_strokeHeight, m_radius));
		else
			IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d %s %.1f m (r %.0f)", m_nameCounter, toolName (m_tool, m_strokeInverted), m_strokeHeight, m_radius));
		break;
	}
	name [sizeof (name) - 1] = '\0';

	return createLayer (name, m_strokePoints, m_strokeKind, m_strokeOperation, m_strokeHeight, m_strokeFamilyId, m_radius, m_feather);
}

// ----------------------------------------------------------------------

TerrainGenerator::Layer * TerrainEditData::createLayer (const char * const name, const PointList & points, const StrokeKind kind, const TerrainGeneratorOperation operation, const float height, const int familyId, const float radius, const float feather)
{
	BoundaryPolyline * const boundary = new BoundaryPolyline ();
	boundary->setName ("godclient_stroke");
	boundary->setFeatherFunction (TGFF_easeInOut);
	boundary->setFeatherDistance (feather);
	for (PointList::const_iterator i = points.begin (); i != points.end (); ++i)
		boundary->addPoint (*i);
	boundary->setWidth (radius);   // also recalculates the extent

	TerrainGenerator::Layer * const layer = new TerrainGenerator::Layer ();
	layer->setName (name ? name : "godclient_edit");
	layer->addBoundary (boundary);

	switch (kind)
	{
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

				if (distanceSquaredToPolyline (tree, e->m_points) <= radiusSquared)
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

	if (getStatus () != S_ok)
		return false;

	ClientProceduralTerrainAppearance * const appearance = m_appearance;

	//-- right button: raise <-> lower, or pick the height under the cursor for flatten / level
	//-- (the tool stays the same)
	if (alternate && (m_tool == T_flatten || m_tool == T_level))
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

	switch (m_tool)
	{
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

	const int numberOfPoints = static_cast<int> (m_strokePoints.size ());
	if (numberOfPoints == m_strokePointsShown)
		return;

	if (!force && (now () - m_lastSwapTime < cs_swapIntervalMs || !isTerrainIdle ()))
		return;

	//-- build the complete replacement first, then swap it in. The live layer is never changed in place.
	TerrainGenerator::Layer * const newLayer = buildStrokeLayer ();
	const Rectangle2d dirty = pointExtent (m_strokePoints, m_strokePointsShown, m_radius);

	if (m_strokeLayer)
	{
		//-- the area already covered gives the same result with the new layer, so only the new part is dirty
		m_appearance->removeLayer (m_strokeLayer);
		retire (m_strokeLayer);
	}

	m_strokeLayer       = newLayer;
	m_strokePointsShown = numberOfPoints;
	m_lastSwapTime      = now ();

	addToTerrain (newLayer, dirty);
	m_lastAction      = "stroke layer swap";
	m_lastActionTime  = now ();
	m_lastActionCount = numberOfPoints;

	//-- baked trees and rocks: clear around the new stroke points (main thread only)
	if (m_strokeKind == SK_flora)
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
		edit.m_extent    = pointExtent (m_strokePoints, 0, m_radius);
		edit.m_points    = m_strokePoints;
		edit.m_kind      = m_strokeKind;
		edit.m_operation = m_strokeOperation;
		edit.m_height    = m_strokeHeight;
		edit.m_familyId  = m_strokeFamilyId;
		edit.m_radius    = m_radius;
		edit.m_feather   = m_feather;
		edit.m_strokeId  = m_nameCounter;
		m_edits.push_back (edit);
		logStroke (edit);
		m_unsaved = true;

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
	const char * const cs_logFileName = "terrain_edit_log.txt";

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
		edit.m_kind == SK_paint ? "paint" : (edit.m_kind == SK_flora ? "flora" : "height"),
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
			edit.m_layer->getName () ? edit.m_layer->getName () : "?", edit.m_kind == SK_paint ? "paint" : "flora",
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
	if (edit.m_kind != SK_height)
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
		TerrainGenerator::Layer * const clone = createLayer (i->m_layer->getName (), i->m_points, i->m_kind, i->m_operation, i->m_height, i->m_familyId, i->m_radius, i->m_feather);
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
	removeFromTerrain (edit.m_layer, edit.m_extent);
	if (edit.m_kind == SK_flora)
		removeFloraClearing (edit.m_strokeId, static_cast<int> (edit.m_points.size ()));
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
	addToTerrain (edit.m_layer, edit.m_extent);
	if (edit.m_kind == SK_flora)
		addFloraClearing (edit.m_strokeId, edit.m_points, 0, edit.m_radius);
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

	Rectangle2d dirty = m_edits.front ().m_extent;
	for (EditList::iterator i = m_edits.begin (); i != m_edits.end (); ++i)
	{
		dirty.expand (i->m_extent);
		m_appearance->removeLayer (i->m_layer);
		retire (i->m_layer);
		if (i->m_kind == SK_flora)
			removeFloraClearing (i->m_strokeId, static_cast<int> (i->m_points.size ()));
	}
	m_edits.clear ();
	clearRedo ();
	m_unsaved = true;

	m_appearance->prepareGenerator ();
	m_lastTerrainChangeTime = now ();
	markDirty (dirty);
	flushDirty ();
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

// ======================================================================
//
// TerrainEditData.cpp
// In-game terrain editing for the God Client. See TerrainEditData.h.
//
// ======================================================================

#include "SwgGodClient/FirstSwgGodClient.h"
#include "TerrainEditData.h"

#include "ActionsEdit.h"
#include "GodClientData.h"
#include "ServerCommander.h"
#include "sharedMath/Transform.h"
#include "sharedObject/World.h"
#include "clientGame/ClientObject.h"
#include "clientGame/Game.h"
#include "clientGraphics/Graphics.h"
#include "sharedMath/VectorArgb.h"
#include "sharedCollision/BoxExtent.h"
#include "sharedCollision/Extent.h"
#include "sharedMath/Sphere.h"
#include "sharedObject/Appearance.h"
#include "sharedObject/CellProperty.h"
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
#include "sharedTerrain/Filter.h"
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
#include <set>
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
		case TerrainEditData::T_ramp:       return "Ramp";
		case TerrainEditData::T_copy:       return "Copy ground";
		case TerrainEditData::T_stamp:      return "Stamp";
		case TerrainEditData::T_scatter:    return "Object scatter";
		case TerrainEditData::T_waterfall:  return "Waterfall";
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
	m_clipboard (),
	m_stampRotation (0.0f),
	m_stampMirror (false),
	m_stampHeightScale (1.0f),
	m_imageSize (128.0f),
	m_imageHeight (20.0f),
	m_scatter (),
	m_strokeScatter (),
	m_scatterSpacing (8.0f),
	m_waterfallShader ("shader/wter_dath_mistyfalls.sht"),
	m_waterfallFlip (false),
	m_strokeWaterfallShader (),
	m_scatterRandomTurn (true),
	m_lastScatterScan (0),
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
	m_strokeOutlineReady (false),
	m_strokeNameOverride (),
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

void TerrainEditData::setLimits (const Limits & limits)
{
	m_limits = limits;
	m_limits.m_slopeMin = std::max (0.0f, std::min (90.0f, m_limits.m_slopeMin));
	m_limits.m_slopeMax = std::max (m_limits.m_slopeMin, std::min (90.0f, m_limits.m_slopeMax));
	if (m_limits.m_heightHigh < m_limits.m_heightLow)
		std::swap (m_limits.m_heightLow, m_limits.m_heightHigh);
}

const TerrainEditData::Limits & TerrainEditData::getLimits () const { return m_limits; }

namespace TerrainEditDataNamespace
{
	//-- the largest border the flora managers accept (just under half a tile)
	inline float maximumFloraBorder (const float tileSize) { return std::max (0.0f, tileSize * 0.5f - 0.05f); }
}

int TerrainEditData::getFloraScatter ()
{
	ClientProceduralTerrainAppearance * const appearance = getAppearance ();
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (appearance->getAppearanceTemplate ()) : 0;
	if (!terrainTemplate)
		return -1;

	const float largest = TerrainEditDataNamespace::maximumFloraBorder (terrainTemplate->getNonCollidableTileSize ());
	if (largest <= 0.0f)
		return 100;
	const float fraction = 1.0f - std::max (0.0f, std::min (largest, terrainTemplate->getNonCollidableTileBorder ())) / largest;
	return static_cast<int> (fraction * 100.0f + 0.5f);
}

void TerrainEditData::setFloraScatter (const int percent)
{
	ClientProceduralTerrainAppearance * const appearance = getAppearance ();
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (appearance->getAppearanceTemplate ()) : 0;
	if (!terrainTemplate)
		return;

	const float keep = 1.0f - static_cast<float> (std::max (0, std::min (100, percent))) / 100.0f;
	const float nonCollidable = TerrainEditDataNamespace::maximumFloraBorder (terrainTemplate->getNonCollidableTileSize ()) * keep;
	const float radial        = TerrainEditDataNamespace::maximumFloraBorder (terrainTemplate->getRadialTileSize ()) * keep;
	const float farRadial     = TerrainEditDataNamespace::maximumFloraBorder (terrainTemplate->getFarRadialTileSize ()) * keep;

	logEvent ("FLORA SCATTER %d%%: bushes/rocks tile %.1f m border %.2f -> %.2f; grass tile %.1f m border %.2f -> %.2f; far grass tile %.1f m border %.2f -> %.2f",
		percent, terrainTemplate->getNonCollidableTileSize (), terrainTemplate->getNonCollidableTileBorder (), nonCollidable,
		terrainTemplate->getRadialTileSize (), terrainTemplate->getRadialTileBorder (), radial,
		terrainTemplate->getFarRadialTileSize (), terrainTemplate->getFarRadialTileBorder (), farRadial);

	const_cast<ProceduralTerrainAppearanceTemplate *> (terrainTemplate)->setFloraTileBorders (nonCollidable, radial, farRadial);
	appearance->setFloraTileBorders (nonCollidable, radial, farRadial);
	m_unsaved = true;
	++m_revision;
}

bool TerrainEditData::toolUsesLimits (const Tool tool)
{
	return tool == T_paint || tool == T_plantFlora || tool == T_clearFlora || tool == T_scatter;
}

bool TerrainEditData::kindUsesLimits (const StrokeKind kind)
{
	return kind == SK_paint || kind == SK_plant || kind == SK_flora || kind == SK_tree || kind == SK_scatter;
}

std::string TerrainEditData::describeLimits (const Limits & limits)
{
	if (!limits.any ())
		return std::string ();

	std::string text;
	char part [96];
	if (limits.m_slopeOn)
	{
		IGNORE_RETURN (_snprintf (part, sizeof (part), "slope %.0f-%.0f deg", limits.m_slopeMin, limits.m_slopeMax));
		part [sizeof (part) - 1] = '\0';
		text += part;
	}
	if (limits.m_heightOn)
	{
		IGNORE_RETURN (_snprintf (part, sizeof (part), "%sheight %.1f-%.1f m", text.empty () ? "" : ", ", limits.m_heightLow, limits.m_heightHigh));
		part [sizeof (part) - 1] = '\0';
		text += part;
	}
	if (limits.m_onTexture > 0)
	{
		IGNORE_RETURN (_snprintf (part, sizeof (part), "%sonly on texture #%d", text.empty () ? "" : ", ", limits.m_onTexture));
		part [sizeof (part) - 1] = '\0';
		text += part;
	}
	return text;
}
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

	case SK_waterfall:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Waterfall %.1f m -> %.1f m (w %.0f)", m_nameCounter,
			m_strokeSamples.waterHeights.empty () ? 0.0f : m_strokeSamples.waterHeights.front (), m_strokeSamples.waterHeights.empty () ? 0.0f : m_strokeSamples.waterHeights.back (), m_radius * 2.0f));
		break;

	case SK_scatter:
		{
			std::string shortName (getScatterTemplate ());
			const size_t slash = shortName.find_last_of ('/');
			if (slash != std::string::npos)
				shortName = shortName.substr (slash + 1);
			IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Scatter %d x %s (every %.0f m, %.0f%%) (r %.0f)", m_nameCounter, static_cast<int> (m_strokeScatter.size ()), shortName.c_str (), m_scatterSpacing, m_strokeParam * 100.0f, m_radius));
		}
		break;

	case SK_stamp:
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Stamp %s (turned %.0f deg%s, height x%.2f)", m_nameCounter, m_clipboard.m_name.c_str (),
			m_stampRotation, m_stampMirror ? ", mirrored" : "", m_stampHeightScale));
		break;

	case SK_ramp:
		{
			const float length = m_strokePoints.size () >= 2 ? m_strokePoints.front ().magnitudeBetween (m_strokePoints.back ()) : 0.0f;
			IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d Ramp %.1f m -> %.1f m, %.0f m long (w %.0f)", m_nameCounter, m_strokeHeight, m_strokeParam, length, m_radius * 2.0f));
		}
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

	//-- strokes made in code (Flatten under selection) bring their own name
	if (!m_strokeNameOverride.empty ())
	{
		IGNORE_RETURN (_snprintf (name, sizeof (name), "godclient_edit_%04d %s", m_nameCounter, m_strokeNameOverride.c_str ()));
		name [sizeof (name) - 1] = '\0';
	}

	//-- shapes: describe the shape instead of the brush size
	if (m_strokeNameOverride.empty () && m_strokeShape != SH_brush && m_strokePoints.size () >= 3)
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

	//-- Smart paint: say which limits the stroke used
	if (m_strokeLimits.any ())
	{
		const size_t used = strlen (name);
		IGNORE_RETURN (_snprintf (name + used, sizeof (name) - used, " [%s]", describeLimits (m_strokeLimits).c_str ()));
		name [sizeof (name) - 1] = '\0';
	}

	return createLayer (name, m_strokePoints, m_strokeKind, m_strokeOperation, m_strokeHeight, m_strokeFamilyId, m_radius, m_feather, m_strokeParam, m_strokeParam2, m_strokeSubKind, m_strokeSamples, (m_strokeKind == SK_waterfall ? m_strokeWaterfallShader : m_waterShader).c_str (), m_strokeShape, m_strokeLimits);
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

bool TerrainEditData::sampleGroundGrid (const Rectangle2d & area, const int margin, float & spacing, int & gx0, int & gz0, int & nx, int & nz, std::vector<float> & ground)
{
	//-- the ground on the terrain's own pole grid over the area (plus a margin in poles), sampled in
	//-- blocks of exactly the client's chunk pole count (see bakeSmooth for why). The spacing starts
	//-- at the finest pole spacing and doubles for very large areas (at most ~6000 poles).
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	const TerrainGenerator * const generator = terrainTemplate ? terrainTemplate->getTerrainGenerator () : 0;
	if (!generator)
		return false;

	spacing = std::max (0.5f, terrainTemplate->getTileWidthInMeters () * 0.5f);
	while (((area.x1 - area.x0) / spacing + 1.0f) * ((area.y1 - area.y0) / spacing + 1.0f) > 6000.0f)
		spacing *= 2.0f;

	gx0 = static_cast<int> (floorf (area.x0 / spacing)) - margin;
	gz0 = static_cast<int> (floorf (area.y0 / spacing)) - margin;
	const int gx1 = static_cast<int> (ceilf (area.x1 / spacing)) + margin;
	const int gz1 = static_cast<int> (ceilf (area.y1 / spacing)) + margin;
	nx = gx1 - gx0 + 1;
	nz = gz1 - gz0 + 1;
	ground.assign (static_cast<size_t> (nx * nz), 0.0f);

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

	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::bakeRamp ()
{
	//-- a straight slope from the ground at the start of the drag to the ground at the end, as wide
	//-- as the brush (size x 2), fading into the ground over the soft edge at the sides. Baked on the
	//-- pole grid like Smooth, so it cuts into hills and fills hollows along the way.
	m_strokeSamples.points.clear ();
	m_strokeSamples.counts.clear ();

	if (m_strokePoints.size () < 2 || m_strokePoints.front ().magnitudeBetween (m_strokePoints.back ()) < 2.0f)
	{
		m_message = "Ramp: drag from one end of the ramp to the other (at least 2 m).";
		return false;
	}

	const Vector2d a = m_strokePoints.front ();
	const Vector2d b = m_strokePoints.back ();
	m_strokePoints.clear ();
	m_strokePoints.push_back (a);
	m_strokePoints.push_back (b);

	const float heightA = m_appearance->generateHeight_expensive (a);
	const float heightB = m_appearance->generateHeight_expensive (b);
	const float length  = a.magnitudeBetween (b);
	const Vector2d direction ((b.x - a.x) / length, (b.y - a.y) / length);

	float spacing = 0.0f;
	int gx0 = 0, gz0 = 0, nx = 0, nz = 0;
	std::vector<float> ground;
	if (!sampleGroundGrid (pointExtent (m_strokePoints, 0, m_radius), 0, spacing, gx0, gz0, nx, nz, ground))
		return false;

	const float inner = m_radius * (1.0f - m_feather);
	float largest = 0.0f;
	for (int z = 0; z < nz; ++z)
		for (int x = 0; x < nx; ++x)
		{
			const Vector2d p (static_cast<float> (gx0 + x) * spacing, static_cast<float> (gz0 + z) * spacing);
			const float rx = p.x - a.x;
			const float rz = p.y - a.y;
			const float along = rx * direction.x + rz * direction.y;
			if (along < 0.0f || along > length)
				continue;
			const float side = fabsf (rx * direction.y - rz * direction.x);
			if (side >= m_radius)
				continue;

			float weight = side <= inner ? 1.0f : 1.0f - (side - inner) / std::max (0.001f, m_radius - inner);
			weight = weight * weight * (3.0f - 2.0f * weight);

			const size_t index = static_cast<size_t> (z * nx + x);
			const float target = heightA + (heightB - heightA) * (along / length);
			const float change = (target - ground [index]) * weight;
			if (!_finite (change) || !_finite (ground [index]) || fabsf (change) < 0.02f)
				continue;
			largest = std::max (largest, fabsf (change));
			m_strokeSamples.points.push_back (Vector (p.x, ground [index] + change, p.y));
		}

	m_strokeHeight = heightA;
	m_strokeParam  = heightB;
	m_strokeParam2 = spacing * 0.5f;

	const float slopeDegrees = atanf (fabsf (heightB - heightA) / length) * (180.0f / PI);
	logEvent ("RAMP %.1f %.1f (%.1f m) -> %.1f %.1f (%.1f m): %.0f m long, slope %.1f deg, %d patches, pole spacing %.1f m, largest change %.2f m",
		a.x, a.y, heightA, b.x, b.y, heightB, length, slopeDegrees, static_cast<int> (m_strokeSamples.points.size ()), spacing, largest);

	if (m_strokeSamples.points.empty ())
	{
		m_message = "Ramp: the ground here already follows that slope (nothing moved more than 2 cm).";
		return false;
	}

	char note [200];
	IGNORE_RETURN (_snprintf (note, sizeof (note), "Ramp: %.0f m long from %.1f m to %.1f m (%.1f deg slope); the ground moved by up to %.1f m.", length, heightA, heightB, slopeDegrees, largest));
	note [sizeof (note) - 1] = '\0';
	m_message = note;
	return true;
}

// ----------------------------------------------------------------------

int TerrainEditData::flattenUnderSelection ()
{
	if (m_strokeActive)
		endStroke ();

	if (getStatus () != S_ok)
	{
		m_message = "Flatten under selection: not on a ground planet, or terrain editing is disabled.";
		return 0;
	}

	GodClientData::ObjectList_t selection;
	GodClientData::getInstance ().getSelection (selection);
	if (selection.empty ())
	{
		m_message = "Flatten under selection: select one or more objects first (untick Terrain brush on to select).";
		return 0;
	}

	const float margin = 1.0f;                     // flat ground beyond the footprint
	const float rim    = std::max (1.0f, m_radius); // the slope back to the old ground: Brush size
	const float savedFeather = m_feather;
	int made = 0;
	std::string names;

	for (GodClientData::ObjectList_t::const_iterator i = selection.begin (); i != selection.end (); ++i)
	{
		const ClientObject * const object = *i;
		if (!object || object->getParentCell () != CellProperty::getWorldCellProperty ())
			continue;   // only objects standing in the world (not inside buildings)

		//-- footprint in the object's own frame: its appearance box, or 70% of its bounding sphere
		float x0 = -2.0f, x1 = 2.0f, z0 = -2.0f, z1 = 2.0f;
		const Appearance * const appearance = object->getAppearance ();
		const Extent * const extent = appearance ? appearance->getExtent () : 0;
		const BoxExtent * const box = dynamic_cast<const BoxExtent *> (extent);
		if (box)
		{
			x0 = box->getMin ().x;
			x1 = box->getMax ().x;
			z0 = box->getMin ().z;
			z1 = box->getMax ().z;
		}
		else if (extent)
		{
			const Sphere & sphere = extent->getSphere ();
			const float r = std::max (1.0f, sphere.getRadius () * 0.7f);
			x0 = sphere.getCenter ().x - r;
			x1 = sphere.getCenter ().x + r;
			z0 = sphere.getCenter ().z - r;
			z1 = sphere.getCenter ().z + r;
		}

		const float grow = margin + rim;
		x0 -= grow;
		x1 += grow;
		z0 -= grow;
		z1 += grow;

		PointList outline;
		const float cornersX [] = { x0, x1, x1, x0 };
		const float cornersZ [] = { z0, z0, z1, z1 };
		for (int c = 0; c < 4; ++c)
		{
			const Vector corner = object->rotateTranslate_o2w (Vector (cornersX [c], 0.0f, cornersZ [c]));
			outline.push_back (Vector2d (corner.x, corner.z));
		}

		const float base     = object->getPosition_w ().y;
		const float halfSize = 0.5f * std::min (x1 - x0, z1 - z0);
		const char * const templateName = object->getObjectTemplateName ();
		std::string shortName (templateName ? templateName : "object");
		const size_t slash = shortName.find_last_of ('/');
		if (slash != std::string::npos)
			shortName = shortName.substr (slash + 1);

		char label [200];
		IGNORE_RETURN (_snprintf (label, sizeof (label), "Flatten under %s to %.1f m (%.0f x %.0f m, rim %.0f m)", shortName.c_str (), base, x1 - x0, z1 - z0, rim));
		label [sizeof (label) - 1] = '\0';

		//-- set up a shape stroke exactly as beginStroke would for Flatten, then finish it
		clearRedo ();
		++m_nameCounter;
		m_strokeInverted   = false;
		m_strokeKind       = SK_height;
		m_strokeFamilyId   = 0;
		m_strokeFloraPointsCleared = 0;
		m_strokeParam      = 0.0f;
		m_strokeParam2     = 0.0f;
		m_strokeSubKind    = 0;
		m_strokeSamples.points.clear ();
		m_strokeSamples.counts.clear ();
		m_strokeFloraPoints.clear ();
		m_strokeFloraRadius = m_radius;
		m_strokeShape      = SH_polygon;
		m_strokeOutlineReady = true;
		m_strokeNameOverride = label;
		m_strokeLimits     = Limits ();
		m_strokeTreeTiles.clear ();
		m_strokeTreeHeights.clear ();
		m_strokeTreeOldHeights.clear ();
		m_strokeOperation  = TGO_replace;
		m_strokeHeight     = base;
		m_strokePoints     = outline;
		m_strokePointsShown = 0;
		m_strokeLayer      = 0;
		m_strokeActive     = true;
		m_feather          = std::max (0.0f, std::min (1.0f, rim / std::max (0.5f, halfSize)));   // soft edge = the rim

		logEvent ("FLATTEN UNDER %s at %.1f %.1f: base %.2f m, footprint %.1f x %.1f m (box %s), rim %.1f m",
			templateName ? templateName : "?", object->getPosition_w ().x, object->getPosition_w ().z, base, x1 - x0 - 2.0f * grow, z1 - z0 - 2.0f * grow, box ? "yes" : "no", rim);

		const size_t before = m_edits.size ();
		endStroke ();
		m_feather = savedFeather;
		m_strokeOutlineReady = false;
		m_strokeNameOverride.clear ();

		if (m_edits.size () > before)
		{
			++made;
			if (names.size () < 120)
				names += (names.empty () ? "" : ", ") + shortName;
		}
	}

	m_feather = savedFeather;

	char note [300];
	if (made > 0)
		IGNORE_RETURN (_snprintf (note, sizeof (note), "Flattened the ground under %d object%s (%s), each to its base height with a %.0f m rim (Brush size).", made, made == 1 ? "" : "s", names.c_str (), rim));
	else
		IGNORE_RETURN (_snprintf (note, sizeof (note), "Flatten under selection: nothing flattened (only objects standing outdoors can be used)."));
	note [sizeof (note) - 1] = '\0';
	m_message = note;
	++m_revision;
	return made;
}

// ----------------------------------------------------------------------

bool  TerrainEditData::hasClipboard () const                 { return m_clipboard.m_valid; }
void  TerrainEditData::setStampRotation (const float d)       { m_stampRotation = d - 360.0f * floorf (d / 360.0f); }
float TerrainEditData::getStampRotation () const              { return m_stampRotation; }
void  TerrainEditData::setStampMirror (const bool mirror)     { m_stampMirror = mirror; }
bool  TerrainEditData::getStampMirror () const                { return m_stampMirror; }
void  TerrainEditData::setStampHeightScale (const float s)    { m_stampHeightScale = std::max (-10.0f, std::min (10.0f, s)); }
float TerrainEditData::getStampHeightScale () const           { return m_stampHeightScale; }
void  TerrainEditData::setImageSize (const float m)           { m_imageSize = std::max (4.0f, std::min (1024.0f, m)); }
float TerrainEditData::getImageSize () const                  { return m_imageSize; }
void  TerrainEditData::setImageHeight (const float m)         { m_imageHeight = std::max (-500.0f, std::min (500.0f, m)); }
float TerrainEditData::getImageHeight () const                { return m_imageHeight; }

std::string TerrainEditData::describeClipboard () const
{
	if (!m_clipboard.m_valid)
		return "Clipboard: empty. Use Copy ground (drag a rectangle) or Load heightmap image.";

	char text [300];
	if (m_clipboard.m_isImage)
		IGNORE_RETURN (_snprintf (text, sizeof (text), "Clipboard: image %s (%d x %d pixels), stamped %.0f m wide, white = %.1f m.",
			m_clipboard.m_name.c_str (), m_clipboard.m_width, m_clipboard.m_height, m_imageSize, m_imageHeight));
	else
	{
		float low = 0.0f;
		float high = 0.0f;
		for (std::vector<float>::const_iterator i = m_clipboard.m_values.begin (); i != m_clipboard.m_values.end (); ++i)
		{
			low  = std::min (low, *i);
			high = std::max (high, *i);
		}
		IGNORE_RETURN (_snprintf (text, sizeof (text), "Clipboard: %s, %.0f x %.0f m, %+.1f to %+.1f m from its edge.",
			m_clipboard.m_name.c_str (), m_clipboard.m_sizeX, m_clipboard.m_sizeZ, low, high));
	}
	text [sizeof (text) - 1] = '\0';
	return text;
}

void TerrainEditData::setClipboardFromImage (const char * const name, const int width, const int height, const std::vector<float> & values01)
{
	if (width < 2 || height < 2 || static_cast<int> (values01.size ()) != width * height)
	{
		m_message = "Load heightmap image: the image is too small or could not be read.";
		return;
	}

	m_clipboard.m_valid   = true;
	m_clipboard.m_isImage = true;
	m_clipboard.m_name    = name ? name : "image";
	m_clipboard.m_width   = width;
	m_clipboard.m_height  = height;
	m_clipboard.m_sizeX   = 0.0f;
	m_clipboard.m_sizeZ   = 0.0f;
	m_clipboard.m_values  = values01;
	logEvent ("CLIPBOARD image %s %d x %d", m_clipboard.m_name.c_str (), width, height);
	m_message = describeClipboard () + " Choose Stamp and click to place it.";
	++m_revision;
}

float TerrainEditData::sampleClipboard (const float u, const float v) const
{
	const int w = m_clipboard.m_width;
	const int h = m_clipboard.m_height;
	const float cu = std::max (0.0f, std::min (static_cast<float> (w - 1), u));
	const float cv = std::max (0.0f, std::min (static_cast<float> (h - 1), v));
	const int x0 = std::min (w - 2, static_cast<int> (cu));
	const int z0 = std::min (h - 2, static_cast<int> (cv));
	const float fx = cu - static_cast<float> (x0);
	const float fz = cv - static_cast<float> (z0);
	const std::vector<float> & d = m_clipboard.m_values;
	const float a = d [static_cast<size_t> (z0 * w + x0)];
	const float b = d [static_cast<size_t> (z0 * w + x0 + 1)];
	const float c = d [static_cast<size_t> ((z0 + 1) * w + x0)];
	const float e = d [static_cast<size_t> ((z0 + 1) * w + x0 + 1)];
	return (a * (1.0f - fx) + b * fx) * (1.0f - fz) + (c * (1.0f - fx) + e * fx) * fz;
}

// ----------------------------------------------------------------------

void TerrainEditData::copyGround ()
{
	//-- the rectangle's ground on the pole grid, stored as metres above (or below) the average ground
	//-- along its edge, so a stamp can sit it on any ground
	const Rectangle2d area = pointExtent (m_strokePoints, 0, 0.0f);
	const float sizeX = area.x1 - area.x0;
	const float sizeZ = area.y1 - area.y0;
	if (sizeX < 4.0f || sizeZ < 4.0f)
	{
		m_message = "Copy ground: drag a rectangle at least 4 m each way.";
		return;
	}
	if (sizeX > 512.0f || sizeZ > 512.0f)
	{
		m_message = "Copy ground: the rectangle is too big (at most 512 m each way).";
		return;
	}

	float spacing = 0.0f;
	int gx0 = 0, gz0 = 0, nx = 0, nz = 0;
	std::vector<float> ground;
	if (!sampleGroundGrid (area, 0, spacing, gx0, gz0, nx, nz, ground) || nx < 2 || nz < 2)
	{
		m_message = "Copy ground: could not read the ground there.";
		return;
	}

	double edgeSum = 0.0;
	int edgeCount = 0;
	for (int z = 0; z < nz; ++z)
		for (int x = 0; x < nx; ++x)
			if (x == 0 || z == 0 || x == nx - 1 || z == nz - 1)
			{
				edgeSum += ground [static_cast<size_t> (z * nx + x)];
				++edgeCount;
			}
	const float edge = edgeCount > 0 ? static_cast<float> (edgeSum / edgeCount) : 0.0f;

	m_clipboard.m_valid   = true;
	m_clipboard.m_isImage = false;
	char name [96];
	IGNORE_RETURN (_snprintf (name, sizeof (name), "ground from %.0f, %.0f", (area.x0 + area.x1) * 0.5f, (area.y0 + area.y1) * 0.5f));
	name [sizeof (name) - 1] = '\0';
	m_clipboard.m_name   = name;
	m_clipboard.m_width  = nx;
	m_clipboard.m_height = nz;
	m_clipboard.m_sizeX  = static_cast<float> (nx - 1) * spacing;
	m_clipboard.m_sizeZ  = static_cast<float> (nz - 1) * spacing;
	m_clipboard.m_values.resize (ground.size ());
	for (size_t i = 0; i < ground.size (); ++i)
		m_clipboard.m_values [i] = _finite (ground [i]) ? ground [i] - edge : 0.0f;

	logEvent ("COPY ground %.1f %.1f .. %.1f %.1f: %d x %d poles at %.1f m, edge average %.2f m", area.x0, area.y0, area.x1, area.y1, nx, nz, spacing, edge);
	m_message = describeClipboard () + " Choose Stamp and click to place it.";
	++m_revision;
}

// ----------------------------------------------------------------------

bool TerrainEditData::bakeStamp ()
{
	//-- the clipboard, turned / mirrored / scaled, centred on the click. It sits on the ground: its
	//-- edge meets the average ground along its outline, and it fades in over the soft edge.
	m_strokeSamples.points.clear ();
	m_strokeSamples.counts.clear ();
	if (!m_clipboard.m_valid || m_strokePoints.empty ())
		return false;

	const Vector2d centre = m_strokePoints.back ();
	const bool  isImage = m_clipboard.m_isImage;
	const float sizeX = isImage ? m_imageSize : m_clipboard.m_sizeX;
	const float sizeZ = isImage ? m_imageSize * static_cast<float> (m_clipboard.m_height - 1) / static_cast<float> (std::max (1, m_clipboard.m_width - 1)) : m_clipboard.m_sizeZ;
	const float halfX = sizeX * 0.5f;
	const float halfZ = sizeZ * 0.5f;
	const float angle = m_stampRotation * (PI / 180.0f);
	const float cosA  = cosf (angle);
	const float sinA  = sinf (angle);

	//-- the four corners in the world (also the edit's outline)
	PointList corners;
	{
		const float cx [] = { -halfX, halfX, halfX, -halfX };
		const float cz [] = { -halfZ, -halfZ, halfZ, halfZ };
		for (int c = 0; c < 4; ++c)
			corners.push_back (Vector2d (centre.x + cx [c] * cosA - cz [c] * sinA, centre.y + cx [c] * sinA + cz [c] * cosA));
	}

	//-- the ground the stamp sits on: the average along its outline
	float base = 0.0f;
	{
		int count = 0;
		for (int c = 0; c < 4; ++c)
		{
			const Vector2d & p0 = corners [static_cast<size_t> (c)];
			const Vector2d & p1 = corners [static_cast<size_t> ((c + 1) % 4)];
			for (int s = 0; s < 8; ++s)
			{
				const float t = static_cast<float> (s) / 8.0f;
				base += m_appearance->generateHeight_expensive (Vector2d (p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t));
				++count;
			}
		}
		base /= static_cast<float> (count);
	}

	float spacing = 0.0f;
	int gx0 = 0, gz0 = 0, nx = 0, nz = 0;
	std::vector<float> ground;
	if (!sampleGroundGrid (pointExtent (corners, 0, 0.0f), 0, spacing, gx0, gz0, nx, nz, ground))
		return false;

	const float featherWidth = std::max (spacing, m_feather * std::min (halfX, halfZ));
	const float heightScale  = isImage ? m_imageHeight * m_stampHeightScale : m_stampHeightScale;
	float largest = 0.0f;
	float highest = -FLT_MAX;
	float lowest  = FLT_MAX;
	for (int z = 0; z < nz; ++z)
		for (int x = 0; x < nx; ++x)
		{
			const Vector2d p (static_cast<float> (gx0 + x) * spacing, static_cast<float> (gz0 + z) * spacing);
			const float dx = p.x - centre.x;
			const float dz = p.y - centre.y;
			float lx =  dx * cosA + dz * sinA;
			const float lz = -dx * sinA + dz * cosA;
			if (m_stampMirror)
				lx = -lx;
			if (fabsf (lx) > halfX || fabsf (lz) > halfZ)
				continue;

			float weight = std::min (1.0f, std::min (halfX - fabsf (lx), halfZ - fabsf (lz)) / featherWidth);
			weight = weight * weight * (3.0f - 2.0f * weight);

			const float u = (lx + halfX) / std::max (0.001f, sizeX) * static_cast<float> (m_clipboard.m_width - 1);
			//-- images: the top row is north (+z); copied ground: row 0 is the south edge
			const float v = (isImage ? (halfZ - lz) : (lz + halfZ)) / std::max (0.001f, sizeZ) * static_cast<float> (m_clipboard.m_height - 1);
			const float desired = base + sampleClipboard (u, v) * heightScale;

			const size_t index = static_cast<size_t> (z * nx + x);
			const float change = (desired - ground [index]) * weight;
			if (!_finite (change) || !_finite (ground [index]) || fabsf (change) < 0.02f)
				continue;
			largest = std::max (largest, fabsf (change));
			highest = std::max (highest, ground [index] + change);
			lowest  = std::min (lowest, ground [index] + change);
			m_strokeSamples.points.push_back (Vector (p.x, ground [index] + change, p.y));
		}

	m_strokePoints = corners;
	m_strokeHeight = base;
	m_strokeParam  = heightScale;
	m_strokeParam2 = spacing * 0.5f;

	logEvent ("STAMP %s at %.1f %.1f: %.0f x %.0f m, turned %.0f deg%s, height scale %.2f, base %.2f m, %d patches at %.1f m, largest change %.2f m",
		m_clipboard.m_name.c_str (), centre.x, centre.y, sizeX, sizeZ, m_stampRotation, m_stampMirror ? " mirrored" : "", heightScale, base,
		static_cast<int> (m_strokeSamples.points.size ()), spacing, largest);

	if (m_strokeSamples.points.empty ())
	{
		m_message = "Stamp: nothing changed (the stamp is flat here, or matches the ground).";
		return false;
	}

	char note [240];
	IGNORE_RETURN (_snprintf (note, sizeof (note), "Stamp: %.0f x %.0f m on ground at %.1f m; the new ground runs from %.1f m to %.1f m (moved by up to %.1f m).",
		sizeX, sizeZ, base, lowest, highest, largest));
	note [sizeof (note) - 1] = '\0';
	m_message = note;
	return true;
}

// ----------------------------------------------------------------------

void  TerrainEditData::setScatterSpacing (const float m)     { m_scatterSpacing = std::max (1.0f, std::min (200.0f, m)); }
float TerrainEditData::getScatterSpacing () const            { return m_scatterSpacing; }
void  TerrainEditData::setScatterRandomTurn (const bool on)  { m_scatterRandomTurn = on; }
bool  TerrainEditData::getScatterRandomTurn () const         { return m_scatterRandomTurn; }

std::string TerrainEditData::getScatterTemplate ()
{
	//-- the Tree Browser gives "static/flora/xyz"; the server wants "object/static/flora/xyz.iff"
	//-- (as GameWidget::createObjectFromTemplate builds it for Ctrl+N)
	std::string name = ActionsEdit::getInstance ().getSelectedServerTemplate ();
	if (name.empty ())
		return name;
	if (name.compare (0, 7, "object/") != 0)
		name = "object/" + name;
	if (name.size () < 4 || _stricmp (name.c_str () + name.size () - 4, ".iff") != 0)
		name += ".iff";
	return name;
}

// ----------------------------------------------------------------------

bool TerrainEditData::bakeScatter ()
{
	//-- one candidate spot per Object spacing cell, at a random place inside it (so no rows), kept
	//-- at Plant density and where the limits pass; snapped to the ground, with a random turn
	m_strokeScatter.clear ();

	const std::string selected = getScatterTemplate ();
	if (selected.empty () || m_strokePoints.empty ())
		return false;

	//-- the server command wants the server template; the client sees the shared one
	std::string serverTemplate (selected);
	std::string sharedTemplate (selected);
	{
		const size_t slash = selected.find_last_of ('/');
		const std::string folder = slash == std::string::npos ? std::string () : selected.substr (0, slash + 1);
		const std::string file   = slash == std::string::npos ? selected : selected.substr (slash + 1);
		if (file.compare (0, 7, "shared_") == 0)
			serverTemplate = folder + file.substr (7);
		else
			sharedTemplate = folder + "shared_" + file;
	}

	const bool isShape = m_strokeShape != SH_brush;
	const PointList & points = m_strokePoints;
	const Rectangle2d area = pointExtent (points, 0, isShape ? 0.0f : m_radius);
	const float radiusSquared = m_radius * m_radius;
	const float spacing = m_scatterSpacing;
	const int   maximum = 250;

	const int gx0 = static_cast<int> (floorf (area.x0 / spacing));
	const int gx1 = static_cast<int> (floorf (area.x1 / spacing));
	const int gz0 = static_cast<int> (floorf (area.y0 / spacing));
	const int gz1 = static_cast<int> (floorf (area.y1 / spacing));

	int limitedOut = 0;
	bool capped = false;
	for (int gz = gz0; gz <= gz1 && !capped; ++gz)
		for (int gx = gx0; gx <= gx1; ++gx)
		{
			RandomGenerator random (static_cast<uint32> (gx * 73856093) ^ static_cast<uint32> (gz * 19349663) ^ (static_cast<uint32> (m_nameCounter) * 83492791u));
			const Vector2d p ((static_cast<float> (gx) + random.randomReal (0.f, 1.f)) * spacing, (static_cast<float> (gz) + random.randomReal (0.f, 1.f)) * spacing);
			const float pick = random.randomReal (0.f, 1.f);
			const float yaw  = m_scatterRandomTurn ? random.randomReal (0.f, PI_TIMES_2) : 0.0f;

			if (isShape ? !isInsidePolygon (p, points) : distanceSquaredToPolyline (p, points) > radiusSquared)
				continue;
			if (pick >= m_strokeParam)
				continue;
			if (m_strokeLimits.any () && !passesLimits (p, m_strokeLimits))
			{
				++limitedOut;
				continue;
			}

			ScatterItem item;
			item.m_serverTemplate = serverTemplate;
			item.m_sharedTemplate = sharedTemplate;
			item.m_position       = Vector (p.x, m_appearance->generateHeight_expensive (p), p.y);
			item.m_yaw            = yaw;
			m_strokeScatter.push_back (item);
			if (static_cast<int> (m_strokeScatter.size ()) >= maximum)
			{
				capped = true;
				break;
			}
		}

	logEvent ("SCATTER %d x %s (server %s): spacing %.1f m, density %.0f%%, %d left out by the limits%s",
		static_cast<int> (m_strokeScatter.size ()), sharedTemplate.c_str (), serverTemplate.c_str (), spacing, m_strokeParam * 100.0f, limitedOut, capped ? ", capped at 250" : "");

	if (m_strokeScatter.empty ())
	{
		m_message = limitedOut > 0 ? "Object scatter: no spot here passes the limits." : "Object scatter: no spot inside the area (try a smaller Object spacing, a bigger brush or a higher density).";
		return false;
	}

	char note [240];
	IGNORE_RETURN (_snprintf (note, sizeof (note), "Object scatter: creating %d objects on the server%s. Undo removes them once the server has sent them back.",
		static_cast<int> (m_strokeScatter.size ()), capped ? " (the most one stroke makes; paint again for more)" : ""));
	note [sizeof (note) - 1] = '\0';
	m_message = note;
	return true;
}

// ----------------------------------------------------------------------

void TerrainEditData::createScatterObjects (const int strokeId)
{
	std::map<int, ScatterList>::iterator found = m_scatter.find (strokeId);
	if (found == m_scatter.end ())
		return;

	int sent = 0;
	for (ScatterList::iterator i = found->second.begin (); i != found->second.end (); ++i)
	{
		if (i->m_live)
			continue;
		Transform transform;
		transform.yaw_l (i->m_yaw);
		transform.setPosition_p (i->m_position);
		IGNORE_RETURN (ServerCommander::getInstance ().createObject ("terrain editor scatter", i->m_serverTemplate, CellProperty::getWorldCellProperty (), transform, false));
		i->m_live           = true;
		i->m_id.clear ();
		i->m_deleteWhenSeen = false;
		i->m_createdAt      = now ();
		++sent;
	}
	logEvent ("SCATTER stroke %d: sent %d create commands", strokeId, sent);
	m_lastScatterScan = 0;
}

// ----------------------------------------------------------------------

void TerrainEditData::removeScatterObjects (const int strokeId)
{
	std::map<int, ScatterList>::iterator found = m_scatter.find (strokeId);
	if (found == m_scatter.end ())
		return;

	int destroyed = 0;
	int waiting = 0;
	for (ScatterList::iterator i = found->second.begin (); i != found->second.end (); ++i)
	{
		if (!i->m_live)
			continue;
		if (!i->m_id.empty ())
		{
			IGNORE_RETURN (ServerCommander::getInstance ().destroyObjectById (i->m_id));
			i->m_id.clear ();
			++destroyed;
		}
		else
		{
			i->m_deleteWhenSeen = true;   // not back from the server yet: destroy it when it arrives
			++waiting;
		}
		i->m_live = false;
	}
	logEvent ("SCATTER stroke %d: destroyed %d objects, %d more will be destroyed when the server sends them", strokeId, destroyed, waiting);
	if (waiting > 0)
	{
		char note [200];
		IGNORE_RETURN (_snprintf (note, sizeof (note), "Object scatter undone: %d objects destroyed; %d not back from the server yet will be destroyed when they arrive.", destroyed, waiting));
		note [sizeof (note) - 1] = '\0';
		m_message = note;
	}
}

// ----------------------------------------------------------------------

void TerrainEditData::updateScatter ()
{
	if (m_scatter.empty () || now () - m_lastScatterScan < 500)
		return;
	m_lastScatterScan = now ();

	//-- anything still waiting for the server (give up after 2 minutes)
	bool waiting = false;
	for (std::map<int, ScatterList>::iterator s = m_scatter.begin (); s != m_scatter.end () && !waiting; ++s)
		for (ScatterList::iterator i = s->second.begin (); i != s->second.end (); ++i)
			if (i->m_id.empty () && (i->m_live || i->m_deleteWhenSeen) && now () - i->m_createdAt < 120000)
			{
				waiting = true;
				break;
			}
	if (!waiting)
		return;

	//-- ids already claimed by a placement
	std::set<std::string> claimed;
	for (std::map<int, ScatterList>::iterator s = m_scatter.begin (); s != m_scatter.end (); ++s)
		for (ScatterList::iterator i = s->second.begin (); i != s->second.end (); ++i)
			if (!i->m_id.empty ())
				claimed.insert (i->m_id);

	int matched = 0;
	//-- static objects (flora trees, rocks) may be in the not-targetable or flora lists
	const int lists [] = { static_cast<int> (WOL_Tangible), static_cast<int> (WOL_TangibleNotTargetable), static_cast<int> (WOL_TangibleFlora) };
	for (int l = 0; l < 3; ++l)
	for (int o = 0; o < World::getNumberOfObjects (lists [l]); ++o)
	{
		const Object * const object = World::getConstObject (lists [l], o);
		if (!object || !object->getNetworkId ().isValid ())
			continue;
		const Vector position = object->getPosition_w ();

		for (std::map<int, ScatterList>::iterator s = m_scatter.begin (); s != m_scatter.end (); ++s)
			for (ScatterList::iterator i = s->second.begin (); i != s->second.end (); ++i)
			{
				if (!i->m_id.empty () || (!i->m_live && !i->m_deleteWhenSeen))
					continue;
				const float dx = position.x - i->m_position.x;
				const float dz = position.z - i->m_position.z;
				if (dx * dx + dz * dz > 0.16f)
					continue;
				const char * const templateName = object->getObjectTemplateName ();
				if (!templateName || _stricmp (templateName, i->m_sharedTemplate.c_str ()) != 0)
					continue;
				const std::string id = object->getNetworkId ().getValueString ();
				if (claimed.find (id) != claimed.end ())
					continue;

				claimed.insert (id);
				++matched;
				if (i->m_deleteWhenSeen)
				{
					IGNORE_RETURN (ServerCommander::getInstance ().destroyObjectById (id));
					i->m_deleteWhenSeen = false;
				}
				else
					i->m_id = id;
				goto nextObject;
			}
nextObject:
		;
	}

	if (matched > 0)
		logEvent ("SCATTER matched %d objects sent back by the server", matched);

	//-- a stroke where the server sent nothing back after 15 s: say so once (wrong template, no
	//-- god rights, or the objects are out of view)
	for (std::map<int, ScatterList>::iterator s = m_scatter.begin (); s != m_scatter.end (); ++s)
	{
		if (s->second.empty () || m_scatterWarned.find (s->first) != m_scatterWarned.end ())
			continue;
		int live = 0;
		int seen = 0;
		unsigned long newest = 0;
		for (ScatterList::const_iterator i = s->second.begin (); i != s->second.end (); ++i)
			if (i->m_live)
			{
				++live;
				if (!i->m_id.empty ())
					++seen;
				newest = std::max (newest, i->m_createdAt);
			}
		if (live == 0 || seen > 0 || now () - newest < 15000)
			continue;

		m_scatterWarned.insert (s->first);
		logEvent ("SCATTER stroke %d: the server has not sent back any of the %d objects after 15 s (template %s)", s->first, live, s->second.front ().m_serverTemplate.c_str ());
		char note [300];
		IGNORE_RETURN (_snprintf (note, sizeof (note), "Object scatter: the server hasn't created any of the %d objects (template %s). Check that Ctrl+N works with this template, and that the account has god rights.",
			live, s->second.front ().m_serverTemplate.c_str ()));
		note [sizeof (note) - 1] = '\0';
		m_message = note;
	}
}

// ----------------------------------------------------------------------

bool TerrainEditData::changesHeight (const StrokeKind kind)
{
	return kind == SK_height || kind == SK_roughen || kind == SK_terrace || kind == SK_road || kind == SK_river || kind == SK_water || kind == SK_smooth || kind == SK_ramp || kind == SK_stamp;
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
	int limitedOut = 0;
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

			//-- Smart paint limits: checked once, at the tree's own spot
			if (m_strokeLimits.any () && !passesLimits (tree, m_strokeLimits))
			{
				++limitedOut;
				continue;
			}

			m_strokeTreeTiles.push_back (tile);
			m_strokeTreeHeights.push_back (m_appearance->generateHeight_expensive (tree));
			m_strokeTreeOldHeights.push_back (m_treeHeightMap [static_cast<size_t> (tile)]);
			if (m_strokeTreeTiles.size () >= 4000)
				break;
		}

	logEvent ("TREES planting %d (family %d, density %.0f%%); %d tiles in the area already had a tree; %d spots left out by the limits%s%s", static_cast<int> (m_strokeTreeTiles.size ()), m_strokeFamilyId, m_strokeParam * 100.0f, occupied, limitedOut,
		m_strokeLimits.any () ? " " : "", describeLimits (m_strokeLimits).c_str ());

	if (m_strokeTreeTiles.empty ())
	{
		m_message = limitedOut > 0 ? "Plant trees: no tree spot here passes the limits (slope / height / texture)."
			: occupied > 0 ? "Plant trees: every 16 m tile here already has a tree (one tree per tile)." : "Plant trees: no tree spot inside the area (trees go one per 16 m tile; try a bigger brush or a higher density).";
		return false;
	}

	char note [200];
	if (limitedOut > 0)
		IGNORE_RETURN (_snprintf (note, sizeof (note), "Planted %d trees (one per 16 m tile at most); %d spots were left out by the limits.", static_cast<int> (m_strokeTreeTiles.size ()), limitedOut));
	else
		IGNORE_RETURN (_snprintf (note, sizeof (note), "Planted %d trees (one per 16 m tile at most).", static_cast<int> (m_strokeTreeTiles.size ())));
	note [sizeof (note) - 1] = '\0';
	m_message = note;
	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::sampleGround (const Vector2d & position_w, float & height, float & slopeDegrees, int & shaderFamily)
{
	//-- runs the generator (every layer, ours included) on a tiny patch at the terrain's own pole
	//-- spacing, so slope and texture come out as the engine's filters see them. The patch is the
	//-- same size generateHeight_expensive uses, which keeps it inside the fractal caches.
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	const TerrainGenerator * const generator = terrainTemplate ? terrainTemplate->getTerrainGenerator () : 0;
	if (!generator)
		return false;

	const int   originOffset  = terrainTemplate->getChunkOriginOffset ();
	const int   upperPad      = terrainTemplate->getChunkUpperPad ();
	const int   numberOfPoles = 2 + originOffset + upperPad;
	const float spacing       = terrainTemplate->getTileWidthInMeters () * 0.5f;

	static TerrainGenerator::CreateChunkBuffer s_buffer;
	static int s_bufferPoles = 0;
	if (s_bufferPoles != numberOfPoles)
	{
		s_buffer.allocate (numberOfPoles);
		s_bufferPoles = numberOfPoles;
	}

	TerrainGenerator::GeneratorChunkData data (terrainTemplate->getLegacyMode ());
	data.shaderGroup                 = &generator->getShaderGroup ();
	data.floraGroup                  = &generator->getFloraGroup ();
	data.radialGroup                 = &generator->getRadialGroup ();
	data.environmentGroup            = &generator->getEnvironmentGroup ();
	data.fractalGroup                = &generator->getFractalGroup ();
	data.bitmapGroup                 = &generator->getBitmapGroup ();
	data.heightMap                   = &s_buffer.heightMap;
	data.colorMap                    = &s_buffer.colorMap;
	data.shaderMap                   = &s_buffer.shaderMap;
	data.floraStaticCollidableMap    = &s_buffer.floraStaticCollidableMap;
	data.floraStaticNonCollidableMap = &s_buffer.floraStaticNonCollidableMap;
	data.floraDynamicNearMap         = &s_buffer.floraDynamicNearMap;
	data.floraDynamicFarMap          = &s_buffer.floraDynamicFarMap;
	data.environmentMap              = &s_buffer.environmentMap;
	data.vertexPositionMap           = &s_buffer.vertexPositionMap;
	data.vertexNormalMap             = &s_buffer.vertexNormalMap;
	data.excludeMap                  = &s_buffer.excludeMap;
	data.passableMap                 = &s_buffer.passableMap;
	data.start                       = Vector (position_w.x - static_cast<float> (originOffset) * spacing, 0.0f, position_w.y - static_cast<float> (originOffset) * spacing);
	data.originOffset                = originOffset;
	data.upperPad                    = upperPad;
	data.numberOfPoles               = numberOfPoles;
	data.distanceBetweenPoles        = spacing;

	generator->generateChunk (data);

	const int c  = originOffset;
	const int x0 = c > 0 ? c - 1 : c;
	const int x1 = c + 1;
	height = s_buffer.heightMap.getData (c, c);
	const float dx = (s_buffer.heightMap.getData (x1, c) - s_buffer.heightMap.getData (x0, c)) / (static_cast<float> (x1 - x0) * spacing);
	const float dz = (s_buffer.heightMap.getData (c, x1) - s_buffer.heightMap.getData (c, x0)) / (static_cast<float> (x1 - x0) * spacing);
	slopeDegrees = atanf (sqrtf (dx * dx + dz * dz)) * (180.0f / PI);
	shaderFamily = s_buffer.shaderMap.getData (c, c).getFamilyId ();
	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::passesLimits (const Vector2d & position_w, const Limits & limits)
{
	if (!limits.any ())
		return true;

	float height = 0.0f;
	float slope  = 0.0f;
	int   shader = 0;
	if (!sampleGround (position_w, height, slope, shader))
		return true;

	if (limits.m_heightOn && (height < limits.m_heightLow || height > limits.m_heightHigh))
		return false;
	if (limits.m_slopeOn && (slope < limits.m_slopeMin || slope > limits.m_slopeMax))
		return false;
	if (limits.m_onTexture > 0 && shader != limits.m_onTexture)
		return false;
	return true;
}

// ----------------------------------------------------------------------

void TerrainEditData::bakeLimitedClear ()
{
	//-- Clear flora with limits: the stroke layer's filters remove generated flora where the ground
	//-- passes the limits, but baked trees are cleared with the engine's clear circles, which know
	//-- nothing of slopes or textures. So each baked tree in the area is checked once, and a small
	//-- clear circle is put on each tree that passes.
	m_strokeFloraPoints.clear ();
	m_strokeFloraRadius = 1.5f;

	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	if (!terrainTemplate || !loadTreeMaps () || m_strokePoints.empty ())
	{
		logEvent ("CLEAR limited: no baked tree map (only terrain-rule flora is cleared) %s", describeLimits (m_strokeLimits).c_str ());
		return;
	}

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

	int kept = 0;
	for (int tz = tz0; tz <= tz1 && m_strokeFloraPoints.size () < 4000; ++tz)
		for (int tx = tx0; tx <= tx1; ++tx)
		{
			const int keyX = tx + centerOffset;
			const int keyZ = tz + centerOffset;
			if (keyX < 0 || keyZ < 0 || keyX >= width || keyZ >= width)
				continue;

			const int tile = keyZ * width + keyX;
			if (m_treeFamilies [static_cast<size_t> (tile)] == 0)
				continue;

			RandomGenerator random (static_cast<uint32> (tile));
			const float xOffset = random.randomReal (0.f, 1.f);
			const float zOffset = random.randomReal (0.f, 1.f);
			const Vector2d tree (static_cast<float> (tx) * tileWidth + tileBorder + xOffset * (tileWidth - 2.0f * tileBorder),
			                     static_cast<float> (tz) * tileWidth + tileBorder + zOffset * (tileWidth - 2.0f * tileBorder));

			if (isShape ? !isInsidePolygon (tree, points) : distanceSquaredToPolyline (tree, points) > radiusSquared)
				continue;

			if (passesLimits (tree, m_strokeLimits))
				m_strokeFloraPoints.push_back (tree);
			else
				++kept;

			if (m_strokeFloraPoints.size () >= 4000)
				break;
		}

	logEvent ("CLEAR limited: %d baked trees pass the limits and are cleared, %d are kept (%s)", static_cast<int> (m_strokeFloraPoints.size ()), kept, describeLimits (m_strokeLimits).c_str ());

	char note [200];
	IGNORE_RETURN (_snprintf (note, sizeof (note), "Clear flora with limits: %d trees cleared, %d kept; bushes and grass follow the limits too.", static_cast<int> (m_strokeFloraPoints.size ()), kept));
	note [sizeof (note) - 1] = '\0';
	m_message = note;
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

TerrainGenerator::Layer * TerrainEditData::addLimitLayers (TerrainGenerator::Layer * const layer, const Limits & limits)
{
	//-- Smart paint. The engine's filters fade in over a band at each end of their range, and they
	//-- leave out the exact ends (flat ground has a slope of exactly 0 degrees), so:
	//--   height and "steeper than" go on the stroke layer itself;
	//--   "flatter than" (slope 0..max) is a sublayer whose filter "max..90" is inverted;
	//--   "only on texture" goes on the innermost sublayer, with the affectors (the engine only
	//--   brings the texture map up to date for a layer that has affectors).
	//-- Sublayers get the parent's strength, so the stroke's own soft edge still applies.
	TerrainGenerator::Layer * target = layer;

	if (limits.m_heightOn)
	{
		FilterHeight * const filter = new FilterHeight ();
		filter->setName ("godclient_limit_height");
		filter->setLowHeight (limits.m_heightLow);
		filter->setHighHeight (limits.m_heightHigh);
		filter->setFeatherFunction (TGFF_easeInOut);
		//-- fade over about 1 m at each end (the feather is a share of half the range)
		const float range = std::max (0.01f, limits.m_heightHigh - limits.m_heightLow);
		filter->setFeatherDistance (std::max (0.0f, std::min (1.0f, 2.0f / range)));
		target->addFilter (filter);
	}

	if (limits.m_slopeOn)
	{
		const float minimum = std::max (0.0f, std::min (90.0f, limits.m_slopeMin));
		const float maximum = std::max (minimum, std::min (90.0f, limits.m_slopeMax));
		const float toRadians = PI / 180.0f;

		if (minimum > 0.25f)
		{
			//-- "between min and max" (max 90 = "steeper than min"); vertical ground never occurs
			FilterSlope * const filter = new FilterSlope ();
			filter->setName ("godclient_limit_slope");
			filter->setMinimumAngle (minimum * toRadians);
			filter->setMaximumAngle (std::min (90.0f, maximum + 0.01f) * toRadians);
			filter->setFeatherFunction (TGFF_easeInOut);
			filter->setFeatherDistance (0.1f);
			target->addFilter (filter);
		}
		else if (maximum < 89.75f)
		{
			//-- "flatter than max": everywhere except slopes from max to 90
			FilterSlope * const filter = new FilterSlope ();
			filter->setName ("godclient_limit_slope_not_steeper");
			filter->setMinimumAngle (maximum * toRadians);
			filter->setMaximumAngle (90.0f * toRadians);
			filter->setFeatherFunction (TGFF_easeInOut);
			filter->setFeatherDistance (0.05f);

			TerrainGenerator::Layer * const sublayer = new TerrainGenerator::Layer ();
			sublayer->setName ("godclient_limit_slope_layer");
			sublayer->addFilter (filter);
			sublayer->setInvertFilters (true);
			target->addLayer (sublayer);
			target = sublayer;
		}
		//-- 0..90 limits nothing
	}

	if (limits.m_onTexture > 0)
	{
		FilterShader * const filter = new FilterShader ();
		filter->setName ("godclient_limit_texture");
		filter->setFamilyId (limits.m_onTexture);

		TerrainGenerator::Layer * const sublayer = new TerrainGenerator::Layer ();
		sublayer->setName ("godclient_limit_texture_layer");
		sublayer->addFilter (filter);
		target->addLayer (sublayer);
		target = sublayer;
	}

	return target;
}

// ----------------------------------------------------------------------

TerrainGenerator::Layer * TerrainEditData::createLayer (const char * const name, const PointList & points, const StrokeKind kind, const TerrainGeneratorOperation operation, const float height, const int familyId, const float radius, const float feather, const float param, const float param2, const int subKind, const PathSamples & samples, const char * const waterShader, const Shape shape, const Limits & limits)
{
	if (kind == SK_road || kind == SK_river)
		return createPathLayer (name, points, kind, familyId, radius, feather, param, samples, waterShader);

	if (kind == SK_waterfall)
		return createWaterfallLayer (name, points, radius, samples, waterShader, param > 0.5f);

	if (kind == SK_smooth || kind == SK_ramp || kind == SK_stamp)
		return createSmoothLayer (name, samples, param2);

	if (kind == SK_tree || kind == SK_scatter)
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

	//-- Smart paint limits: the affectors go into the innermost limit sublayer
	TerrainGenerator::Layer * const target = (kindUsesLimits (kind) && limits.any ()) ? addLimitLayers (layer, limits) : layer;

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
			target->addAffector (basin);
		}
		break;

	case SK_paint:
		{
			AffectorShaderConstant * const affector = new AffectorShaderConstant ();
			affector->setName ("godclient_paint");
			affector->setFamilyId (familyId);
			target->addAffector (affector);

			//-- the planet's colour layers tint every texture (desert colour-ramp fractals give the
			//-- ground its patchy look), which leaves dark/blotchy areas on the painted texture.
			//-- Reset the tint to neutral (white = untinted) inside the stroke, feathered at the edge.
			AffectorColorConstant * const tint = new AffectorColorConstant ();
			tint->setName ("godclient_paint_neutral_tint");
			tint->setOperation (TGO_replace);
			tint->setColor (PackedRgb::solidWhite);
			target->addAffector (tint);
		}
		break;

	case SK_flora:
		{
			//-- remove every kind of flora. On most planets trees/rocks come from the baked flora map
			//-- instead; those are handled by addFloraClearing and buildClearedFloraMap.
			AffectorFloraStaticCollidableConstant * const collidable = new AffectorFloraStaticCollidableConstant ();
			collidable->setName ("godclient_clear_collidable");
			collidable->setRemoveAll (true);
			target->addAffector (collidable);

			AffectorFloraStaticNonCollidableConstant * const nonCollidable = new AffectorFloraStaticNonCollidableConstant ();
			nonCollidable->setName ("godclient_clear_noncollidable");
			nonCollidable->setRemoveAll (true);
			target->addAffector (nonCollidable);

			AffectorFloraDynamicNearConstant * const nearFlora = new AffectorFloraDynamicNearConstant ();
			nearFlora->setName ("godclient_clear_radial_near");
			nearFlora->setRemoveAll (true);
			target->addAffector (nearFlora);

			AffectorFloraDynamicFarConstant * const farFlora = new AffectorFloraDynamicFarConstant ();
			farFlora->setName ("godclient_clear_radial_far");
			farFlora->setRemoveAll (true);
			target->addAffector (farFlora);
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
			target->addAffector (fractal);

			AffectorHeightConstant * const centre = new AffectorHeightConstant ();
			centre->setName ("godclient_roughen_centre");
			centre->setOperation (TGO_subtract);
			centre->setHeight (param * 0.5f);
			target->addAffector (centre);
		}
		break;

	case SK_terrace:
		{
			AffectorHeightTerrace * const terrace = new AffectorHeightTerrace ();
			terrace->setName ("godclient_terrace");
			terrace->setHeight (param);
			terrace->setFraction (param2);
			target->addAffector (terrace);
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
			target->addAffector (grass);
		}
		else
		{
			AffectorFloraStaticNonCollidableConstant * const flora = new AffectorFloraStaticNonCollidableConstant ();
			flora->setName ("godclient_plant_static");
			flora->setFamilyId (familyId);
			flora->setOperation (TGO_add);
			flora->setDensityOverride (true);
			flora->setDensityOverrideDensity (param);
			target->addAffector (flora);
		}
		break;

	case SK_height:
	default:
		{
			AffectorHeightConstant * const affector = new AffectorHeightConstant ();
			affector->setName ("godclient_height");
			affector->setOperation (operation);
			affector->setHeight (height);
			target->addAffector (affector);
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

				//-- Smart paint limits: only the trees that passed (one small clear circle on each)
				bool hit = false;
				if (e->m_limits.any ())
				{
					const float floraRadiusSquared = e->m_floraRadius * e->m_floraRadius;
					for (PointList::const_iterator f = e->m_floraPoints.begin (); f != e->m_floraPoints.end () && !hit; ++f)
						hit = f->magnitudeBetweenSquared (tree) <= floraRadiusSquared;
				}
				else
					hit = e->m_shape != SH_brush ? isInsidePolygon (tree, e->m_points) : distanceSquaredToPolyline (tree, e->m_points) <= radiusSquared;

				if (hit)
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
	m_strokeOutlineReady = false;
	m_strokeNameOverride.clear ();

	//-- roads, rivers and ramps are always paths; water always fills a shape (a brush drag draws its outline)
	if (m_tool == T_road || m_tool == T_river || m_tool == T_ramp || m_tool == T_stamp || m_tool == T_waterfall)
		m_strokeShape = SH_brush;
	else if (m_tool == T_water && m_shape == SH_brush)
		m_strokeShape = SH_polygon;
	else if (m_tool == T_copy)
		m_strokeShape = SH_rectangle;   // copy: always a rectangle, drag corner to corner
	else
		m_strokeShape = m_shape;

	m_strokeScatter.clear ();

	switch (m_tool)
	{
	case T_waterfall:
		m_strokeKind      = SK_waterfall;
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = 0.0f;
		m_strokeParam     = m_waterfallFlip ? 1.0f : 0.0f;
		break;

	case T_scatter:
		if (getScatterTemplate ().empty ())
		{
			logEvent ("STROKE REFUSED scatter: no server template selected");
			m_message = "Object scatter: select a server template first (Tree Browser, ServerTemplates tab; the same one Ctrl+N creates).";
			return false;
		}
		m_strokeKind      = SK_scatter;
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = 0.0f;
		m_strokeParam     = m_plantDensity;
		break;

	case T_copy:
		m_strokeKind      = SK_copy;
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = 0.0f;
		break;

	case T_stamp:
		if (!m_clipboard.m_valid)
		{
			logEvent ("STROKE REFUSED stamp: the clipboard is empty");
			m_message = "Stamp: nothing to stamp yet. Use Copy ground (drag a rectangle) or Load heightmap image first.";
			return false;
		}
		m_strokeKind      = SK_stamp;
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = 0.0f;
		break;

	case T_ramp:
		m_strokeKind      = SK_ramp;
		m_strokeOperation = TGO_replace;
		m_strokeHeight    = 0.0f;
		break;

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

	m_strokeLimits = kindUsesLimits (m_strokeKind) ? m_limits : Limits ();
	if (m_strokeLimits.any ())
		logEvent ("STROKE LIMITS %s", describeLimits (m_strokeLimits).c_str ());

	//-- painting a texture only where the ground already has that texture changes nothing
	if (m_strokeKind == SK_paint && m_strokeLimits.m_onTexture > 0 && m_strokeLimits.m_onTexture == m_strokeFamilyId)
	{
		logEvent ("STROKE REFUSED paint: the paint texture and the 'only where the ground is already' texture are the same (#%d)", m_strokeFamilyId);
		m_message = "Nothing to do: Paint texture and 'Only where the ground is already' are the same texture. The second list is the texture to paint OVER; set it to (any texture) or to the texture you want to replace.";
		return false;
	}

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

	//-- stamp: one point, the centre (dragging moves it); built when the button is released
	if (m_strokeKind == SK_stamp)
	{
		m_strokePoints.back () = position_w;
		return;
	}

	//-- ramp: only the two ends matter (start and the current point); built when the drag ends
	if (m_strokeKind == SK_ramp || m_strokeKind == SK_waterfall)
	{
		if (m_strokePoints.size () < 2)
			m_strokePoints.push_back (position_w);
		else
			m_strokePoints.back () = position_w;
		return;
	}

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

		const PointList outline = m_strokeOutlineReady ? m_strokePoints : buildOutline (m_strokeShape, m_strokePoints);
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

		//-- the water surface only exists inside the outline, so it must stay below the shore all the
		//-- way round, or it shows as a raised sheet standing above the bank. Highest allowed: the
		//-- lowest ground on the outline minus a little. A height under all the ground inside
		//-- (e.g. the box still at 0 m) means "fill it": just below the shore.
		if (m_strokeKind == SK_water)
		{
			//-- the shore: dry outline points only. Where the outline crosses water that is already
			//-- there (a river running in), that water is the level to join, and its bed is not shore.
			float lowestShore = FLT_MAX;
			float lowestAll   = FLT_MAX;
			float joinHeight  = FLT_MAX;
			int   wetPoints   = 0;
			const int step = std::max (1, numberOfPoints / 64);
			for (int i = 0; i < numberOfPoints; i += step)
			{
				const Vector2d & p = m_strokePoints [static_cast<size_t> (i)];
				const float ground = m_appearance->generateHeight_expensive (p);
				lowestAll = std::min (lowestAll, ground);
				float existing = 0.0f;
				if (existingWaterHeight (p, ground, existing))
				{
					joinHeight = std::min (joinHeight, existing);
					++wetPoints;
				}
				else
					lowestShore = std::min (lowestShore, ground);
			}
			if (lowestShore == FLT_MAX)
				lowestShore = lowestAll;   // the whole outline is in water

			float lowestInside = FLT_MAX;
			float gridRadius = 0.0f;
			const PointList inner = coverPoints (m_strokePoints, gridRadius);
			const int innerStep = std::max (1, static_cast<int> (inner.size ()) / 48);
			for (size_t i = 0; i < inner.size (); i += static_cast<size_t> (innerStep))
				if (isInsidePolygon (inner [i], m_strokePoints))
				{
					const float ground = m_appearance->generateHeight_expensive (inner [i]);
					lowestInside = std::min (lowestInside, ground);

					//-- a lake drawn over (part of) another lake or a river: inside, that water stands
					//-- above its dug bed, so the new lake joins it there too
					float existing = 0.0f;
					if (existingWaterHeight (inner [i], ground, existing))
					{
						joinHeight = std::min (joinHeight, existing);
						++wetPoints;
					}
				}
			lowestInside = std::min (lowestInside, lowestShore);

			const float requested = m_strokeHeight;
			const float shoreGap  = 0.3f;
			const float highest   = lowestShore - shoreGap;
			const char * reason   = 0;

			if (joinHeight < FLT_MAX && joinHeight <= highest + shoreGap)
			{
				//-- touching existing water: take its level so the lake and the river meet
				m_strokeHeight = joinHeight;
				reason = "the outline touches water that is already there (a river), so the lake takes its level";
				logEvent ("WATER joins existing water at %.2f m (%d outline points in water)", joinHeight, wetPoints);
			}
			else if (lowestShore < FLT_MAX)
			{
				if (requested <= lowestInside)
				{
					m_strokeHeight = highest;
					reason = "it was under all the ground there, so the lake was filled to just below the shore";
				}
				else if (requested > highest)
				{
					m_strokeHeight = highest;
					reason = "it was above the lowest point of the shore, so the water would have stood above the bank";
				}
			}

			const bool hidden = m_strokeParam <= 0.0f && m_strokeHeight <= lowestInside;
			logEvent ("WATER requested %.1f m, shore lowest %.1f m, inside lowest %.1f m: using %.1f m (basin %.1f m deep)%s",
				requested, lowestShore, lowestInside, m_strokeHeight, m_strokeParam, hidden ? " - no dip inside and no depth: the water is under the ground" : "");

			char note [260];
			note [0] = '\0';
			if (reason && joinHeight < FLT_MAX && m_strokeHeight == joinHeight)
				IGNORE_RETURN (_snprintf (note, sizeof (note), "Water at %.1f m: the lake joins the water already there (a river), so their levels match.", m_strokeHeight));
			else if (reason)
				IGNORE_RETURN (_snprintf (note, sizeof (note), "Water at %.1f m, %.1f m below the lowest point of the shore (you asked for %.1f m; %s).", m_strokeHeight, shoreGap, requested, reason));
			if (hidden)
				IGNORE_RETURN (_snprintf (note, sizeof (note), "The ground inside is not lower than the shore, so the water is hidden under it. Set River/Lake depth above 0 to dig a basin."));
			note [sizeof (note) - 1] = '\0';
			if (note [0])
				m_message = note;
			if (m_strokeHeight != requested)
			{
				m_levelHeight = m_strokeHeight;
				++m_revision;
			}
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

		//-- rivers: where the path runs into water that is already there (a lake, another river),
		//-- the river's surface takes that water's height so the two meet level
		if (m_strokeKind == SK_river)
		{
			m_strokeSamples.waterHeights.clear ();
			int joined = 0;
			size_t first = 0;
			for (int p = 0; p < numberOfPoints; ++p)
			{
				const size_t sampleIndex = std::min (first, m_strokeSamples.points.size () - 1);
				const float bank = m_strokeSamples.points [sampleIndex].y;
				float water = bank - m_strokeParam * 0.3f;
				float existing = 0.0f;
				if (existingWaterHeight (m_strokePoints [static_cast<size_t> (p)], bank, existing))
				{
					water = existing;
					++joined;
				}
				m_strokeSamples.waterHeights.push_back (water);
				if (static_cast<size_t> (p) < m_strokeSamples.counts.size ())
					first += static_cast<size_t> (m_strokeSamples.counts [static_cast<size_t> (p)]);
				else
					first = m_strokeSamples.points.size () - 1;
			}
			if (joined > 0)
			{
				logEvent ("RIVER joins existing water at %d of %d path points", joined, numberOfPoints);
				char note [200];
				IGNORE_RETURN (_snprintf (note, sizeof (note), "River: joined the water already there at %d of its %d path points, so the levels match.", joined, numberOfPoints));
				note [sizeof (note) - 1] = '\0';
				m_message = note;
			}
		}
	}

	//-- smooth: built once, when the drag ends, from the ground as it is before the stroke
	if (m_strokeKind == SK_smooth)
	{
		if (!force || m_strokeLayer)
			return;
		if (!bakeSmooth ())
			return;
	}

	//-- copy ground: the rectangle's heights go to the clipboard; no layer, so no edit
	if (m_strokeKind == SK_copy)
	{
		if (force && !m_strokeLayer)
			copyGround ();
		return;
	}

	//-- stamp: built once, when the button is released, centred on the last point
	if (m_strokeKind == SK_stamp)
	{
		if (!force || m_strokeLayer)
			return;
		if (!bakeStamp ())
			return;
		numberOfPoints = static_cast<int> (m_strokePoints.size ());   // now the stamp's four corners
	}

	//-- ramp: built once, when the drag ends, from the ground at the two ends
	if (m_strokeKind == SK_ramp)
	{
		if (!force || m_strokeLayer)
			return;
		if (!bakeRamp ())
			return;
	}

	//-- waterfall: built once, when the drag ends, following the ground from the top to the bottom
	if (m_strokeKind == SK_waterfall)
	{
		if (!force || m_strokeLayer)
			return;
		if (m_waterShader.empty ())
			m_waterShader = findWaterShader ();
		if (!bakeWaterfall ())
			return;
		numberOfPoints = static_cast<int> (m_strokePoints.size ());
	}

	//-- Clear flora with limits: built once, when the drag ends (each baked tree is checked)
	const bool limitedFlora = m_strokeKind == SK_flora && m_strokeLimits.any ();
	if (limitedFlora)
	{
		if (!force || m_strokeLayer)
			return;
		bakeLimitedClear ();
	}

	//-- object scatter: spots chosen once, when the drag ends; the objects are created in endStroke
	if (m_strokeKind == SK_scatter)
	{
		if (!force || m_strokeLayer)
			return;
		if (!bakeScatter ())
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
	if (m_strokeKind == SK_river || m_strokeKind == SK_water || m_strokeKind == SK_waterfall)
	{
		m_appearance->addRunTimeWater (newLayer, m_nameCounter);
		refreshSwimWater ();
	}
	m_lastAction      = "stroke layer swap";
	m_lastActionTime  = now ();
	m_lastActionCount = numberOfPoints;

	//-- baked trees and rocks: clear around the new stroke points (main thread only)
	if (m_strokeKind == SK_flora && (isShape || limitedFlora))
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
		edit.m_waterShader = m_strokeKind == SK_waterfall ? m_strokeWaterfallShader : m_waterShader;
		edit.m_shape       = m_strokeShape;
		const bool floraFromList = m_strokeShape != SH_brush || (m_strokeKind == SK_flora && m_strokeLimits.any ());
		edit.m_floraPoints = floraFromList ? m_strokeFloraPoints : m_strokePoints;
		edit.m_floraRadius = floraFromList ? m_strokeFloraRadius : m_radius;
		edit.m_limits      = m_strokeLimits;
		edit.m_treeTiles      = m_strokeTreeTiles;
		edit.m_treeHeights    = m_strokeTreeHeights;
		edit.m_treeOldHeights = m_strokeTreeOldHeights;
		m_edits.push_back (edit);
		logStroke (edit);
		m_unsaved = true;

		//-- object scatter: send the create commands now that the edit exists
		if (edit.m_kind == SK_scatter)
		{
			m_scatter [edit.m_strokeId] = m_strokeScatter;
			m_strokeScatter.clear ();
			createScatterObjects (edit.m_strokeId);
		}

		//-- Smart paint: say how much of the stroke passed the limits, and what the ground there is,
		//-- so a stroke that did nothing explains itself (trees and Clear flora report their own counts)
		if (edit.m_limits.any () && (edit.m_kind == SK_paint || edit.m_kind == SK_plant))
		{
			PointList spots;
			if (edit.m_shape != SH_brush)
			{
				float gridRadius = 0.0f;
				const PointList grid = coverPoints (edit.m_points, gridRadius);
				for (PointList::const_iterator g = grid.begin (); g != grid.end (); ++g)
					if (isInsidePolygon (*g, edit.m_points))
						spots.push_back (*g);
			}
			else
				spots = edit.m_points;

			const int total = static_cast<int> (spots.size ());
			const int checks = std::min (total, 16);
			int passed = 0;
			int passedAndPainted = 0;
			int failedSlope = 0;
			int failedHeight = 0;
			int failedTexture = 0;
			float firstHeight = 0.0f;
			float firstSlope  = 0.0f;
			int   firstShader = 0;
			for (int c = 0; c < checks; ++c)
			{
				const int index = checks > 1 ? (c * (total - 1)) / (checks - 1) : 0;
				float h = 0.0f;
				float s = 0.0f;
				int   t = 0;
				if (!sampleGround (spots [static_cast<size_t> (index)], h, s, t))
					continue;
				if (c == 0)
				{
					firstHeight = h;
					firstSlope  = s;
					firstShader = t;
				}
				const bool slopeOk   = !edit.m_limits.m_slopeOn || (s >= edit.m_limits.m_slopeMin && s <= edit.m_limits.m_slopeMax);
				const bool heightOk  = !edit.m_limits.m_heightOn || (h >= edit.m_limits.m_heightLow && h <= edit.m_limits.m_heightHigh);
				const bool painted   = edit.m_kind == SK_paint && t == edit.m_familyId;   // sampled after painting: a passing spot already shows the new texture
				const bool textureOk = edit.m_limits.m_onTexture <= 0 || t == edit.m_limits.m_onTexture || painted;
				if (!slopeOk)   ++failedSlope;
				if (!heightOk)  ++failedHeight;
				if (!textureOk) ++failedTexture;
				const bool ok = slopeOk && heightOk && textureOk;
				if (ok)
					++passed;
				if (ok && painted)
					++passedAndPainted;
				logEvent ("  spot %.1f %.1f: height %.1f slope %.0f texture #%d -> %s%s", spots [static_cast<size_t> (index)].x, spots [static_cast<size_t> (index)].y, h, s, t,
					ok ? "passes" : "outside", edit.m_kind == SK_paint ? (painted ? ", painted" : ", not painted") : "");
			}

			const char * const shaderName = getShaderFamilyName (firstShader);
			logEvent ("LIMITS %d of %d checked spots pass (%s); paint landed on %d of the passing spots; failed: slope %d, height %d, texture %d; first spot: height %.1f m, slope %.0f deg, texture %s (#%d)",
				passed, checks, describeLimits (edit.m_limits).c_str (), passedAndPainted, failedSlope, failedHeight, failedTexture, firstHeight, firstSlope, shaderName && *shaderName ? shaderName : "?", firstShader);

			char note [400];
			if (checks > 0 && passed == 0)
			{
				const char * const blocker = failedTexture >= checks ? "the 'Only where the ground is already' texture"
					: failedHeight >= checks ? "the height band" : failedSlope >= checks ? "the slope range" : "the limits together";
				IGNORE_RETURN (_snprintf (note, sizeof (note), "Nothing changed: %s left out the whole stroke (%s). The ground here is %.1f m high, slope %.0f deg, texture %s (#%d).",
					blocker, describeLimits (edit.m_limits).c_str (), firstHeight, firstSlope, shaderName && *shaderName ? shaderName : "?", firstShader));
			}
			else
				IGNORE_RETURN (_snprintf (note, sizeof (note), "Limits: %d of %d checked spots in the stroke pass (%s).", passed, checks, describeLimits (edit.m_limits).c_str ()));
			note [sizeof (note) - 1] = '\0';
			m_message = note;
		}

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
		//-- path points that joined existing water carry their own surface height
		if (samples.waterHeights.size () == points.size ())
		{
			heights.clear ();
			for (size_t w = 0; w < samples.waterHeights.size (); ++w)
				heights.add (samples.waterHeights [w]);
		}
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

void  TerrainEditData::setWaterfallShader (const std::string & shaderTemplate) { m_waterfallShader = shaderTemplate; }
const std::string & TerrainEditData::getWaterfallShader () const          { return m_waterfallShader; }
void  TerrainEditData::setWaterfallFlip (const bool flip)                  { m_waterfallFlip = flip; }
bool  TerrainEditData::getWaterfallFlip () const                           { return m_waterfallFlip; }

// ----------------------------------------------------------------------

bool TerrainEditData::existingWaterHeight (const Vector2d & position_w, const float ground, float & waterHeight) const
{
	//-- water from the planet or from earlier edits (lakes, river ribbons) at this spot, counted only
	//-- when it is at or above the ground there (so a deep global water table under the land is ignored)
	const ProceduralTerrainAppearanceTemplate * const terrainTemplate = m_appearance ? dynamic_cast<const ProceduralTerrainAppearanceTemplate *> (m_appearance->getAppearanceTemplate ()) : 0;
	if (!terrainTemplate)
		return false;
	float height = 0.0f;
	if (!terrainTemplate->getWaterHeight (Vector (position_w.x, 0.0f, position_w.y), height))
		return false;
	if (height < ground - 0.2f)
		return false;
	waterHeight = height;
	return true;
}

// ----------------------------------------------------------------------

bool TerrainEditData::bakeWaterfall ()
{
	//-- a sheet of water from the top of the drag to the bottom, just above the ground all the way
	//-- (so it hugs a cliff or runs down steps). Where either end is in water that is already there
	//-- (a river above, a pool below), it joins that water's height.
	m_strokeSamples.points.clear ();
	m_strokeSamples.counts.clear ();
	m_strokeSamples.waterHeights.clear ();

	if (m_strokePoints.size () < 2 || m_strokePoints.front ().magnitudeBetween (m_strokePoints.back ()) < 1.0f)
	{
		m_message = "Waterfall: drag from the top of the fall to the bottom (at least 1 m).";
		return false;
	}
	//-- the chosen waterfall look (the game's falling-water shaders); the planet's own water if none
	//-- is chosen or the file is missing
	m_strokeWaterfallShader = (!m_waterfallShader.empty () && TreeFile::exists (m_waterfallShader.c_str ())) ? m_waterfallShader : m_waterShader;
	if (m_strokeWaterfallShader.empty ())
	{
		m_message = "Waterfall: no water look found on this planet.";
		return false;
	}

	const Vector2d top    = m_strokePoints.front ();
	const Vector2d bottom = m_strokePoints.back ();
	const float length = top.magnitudeBetween (bottom);
	//-- the ground every 0.5 m along the drag
	const float spacing = 0.5f;
	const int pieces = std::max (2, std::min (160, static_cast<int> (ceilf (length / spacing))));
	const float step = length / static_cast<float> (pieces);

	PointList points;
	std::vector<float> ground;
	for (int i = 0; i <= pieces; ++i)
	{
		const float t = static_cast<float> (i) / static_cast<float> (pieces);
		const Vector2d p (top.x + (bottom.x - top.x) * t, top.y + (bottom.y - top.y) * t);
		points.push_back (p);
		ground.push_back (m_appearance->generateHeight_expensive (p));
	}

	//-- real falls shoot out over the edge: the water keeps the height of the ground up to 3 m
	//-- behind it (back up the drag), so it carries on 3 m past the top of the cliff face and then
	//-- drops nearly straight down in front of it, instead of lying on (and cutting into) the rock
	const int lookback = std::max (1, static_cast<int> (3.0f / step + 0.5f));
	for (int i = 0; i <= pieces; ++i)
	{
		float water = ground [static_cast<size_t> (i)] + 0.3f;
		for (int j = std::max (0, i - lookback); j < i; ++j)
			water = std::max (water, ground [static_cast<size_t> (j)] + 0.3f);
		if (i > 0)
			water = std::min (water, m_strokeSamples.waterHeights.back ());   // it never climbs
		m_strokeSamples.waterHeights.push_back (water);
	}

	//-- the two ends join water that is already there (a river above, a pool below)
	{
		float existing = 0.0f;
		if (existingWaterHeight (points.front (), ground.front (), existing))
			m_strokeSamples.waterHeights.front () = existing;
		if (existingWaterHeight (points.back (), ground.back (), existing))
			m_strokeSamples.waterHeights.back () = existing;
		for (size_t i = 1; i < m_strokeSamples.waterHeights.size (); ++i)
			m_strokeSamples.waterHeights [i] = std::min (m_strokeSamples.waterHeights [i], m_strokeSamples.waterHeights [i - 1]);
	}

	float highest = m_strokeSamples.waterHeights.front ();
	float lowest  = m_strokeSamples.waterHeights.back ();

	m_strokePoints = points;
	logEvent ("WATERFALL %.1f %.1f -> %.1f %.1f: %.0f m long, water %.1f m -> %.1f m, %d points, width %.0f m",
		top.x, top.y, bottom.x, bottom.y, length, m_strokeSamples.waterHeights.front (), m_strokeSamples.waterHeights.back (), static_cast<int> (points.size ()), m_radius * 2.0f);

	char note [200];
	IGNORE_RETURN (_snprintf (note, sizeof (note), "Waterfall: %.0f m long, falling from %.1f m to %.1f m.", length, m_strokeSamples.waterHeights.front (), m_strokeSamples.waterHeights.back ()));
	note [sizeof (note) - 1] = '\0';
	m_message = note;
	UNREF (highest);
	UNREF (lowest);
	return true;
}

// ----------------------------------------------------------------------

TerrainGenerator::Layer * TerrainEditData::createWaterfallLayer (const char * const name, const PointList & points, const float radius, const PathSamples & samples, const char * const waterShader, const bool flipFlow)
{
	TerrainGenerator::Layer * const layer = new TerrainGenerator::Layer ();
	layer->setName (name ? name : "godclient_edit");
	if (points.size () < 2 || samples.waterHeights.size () != points.size () || !waterShader || !*waterShader)
		return layer;

	AffectorRibbon * const ribbon = new AffectorRibbon ();
	ribbon->setName ("godclient_waterfall");
	//-- the shader's animation scrolls along the ribbon; which way looks "down" depends on the shader,
	//-- so the user can flip it. Default: built from the top down (the drag order).
	if (flipFlow)
	{
		for (PointList::const_reverse_iterator i = points.rbegin (); i != points.rend (); ++i)
			ribbon->addPoint (*i);
	}
	else
	{
		for (PointList::const_iterator i = points.begin (); i != points.end (); ++i)
			ribbon->addPoint (*i);
	}
	ribbon->setWidth (radius * 2.0f);
	ribbon->setFeatherFunction (TGFF_linear);
	ribbon->setFeatherDistance (0.0f);
	ribbon->setRibbonWaterShaderTemplateName (waterShader);
	ribbon->setWaterShaderSize (4.0f);
	ribbon->setVelocity (4.0f);    // fast water
	ribbon->setCapWidth (0.0f);
	ribbon->setWaterType (TGWT_water);
	ribbon->setTerrainShaderFamilyId (0);

	ArrayList<float> heights;
	if (flipFlow)
	{
		for (size_t i = samples.waterHeights.size (); i > 0; --i)
			heights.add (samples.waterHeights [i - 1]);
	}
	else
	{
		for (size_t i = 0; i < samples.waterHeights.size (); ++i)
			heights.add (samples.waterHeights [i]);
	}
	ribbon->copyHeightList (heights);

	layer->addAffector (ribbon);
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

TerrainEditData::PointList TerrainEditData::stampCorners (const Vector2d & centre) const
{
	PointList corners;
	if (!m_clipboard.m_valid)
		return corners;

	const bool  isImage = m_clipboard.m_isImage;
	const float halfX = 0.5f * (isImage ? m_imageSize : m_clipboard.m_sizeX);
	const float halfZ = 0.5f * (isImage ? m_imageSize * static_cast<float> (m_clipboard.m_height - 1) / static_cast<float> (std::max (1, m_clipboard.m_width - 1)) : m_clipboard.m_sizeZ);
	const float angle = m_stampRotation * (PI / 180.0f);
	const float cosA = cosf (angle);
	const float sinA = sinf (angle);
	const float cx [] = { -halfX, halfX, halfX, -halfX };
	const float cz [] = { -halfZ, -halfZ, halfZ, halfZ };
	for (int c = 0; c < 4; ++c)
		corners.push_back (Vector2d (centre.x + cx [c] * cosA - cz [c] * sinA, centre.y + cx [c] * sinA + cz [c] * cosA));
	return corners;
}

// ----------------------------------------------------------------------

void TerrainEditData::drawGroundLine (const PointList & points, const bool closed, const VectorArgb & colour)
{
	//-- lines a little above the ground, split every ~2 m so they follow it (heights from the
	//-- terrain already on screen: cheap)
	const int n = static_cast<int> (points.size ());
	if (n < 2 || !m_appearance)
		return;

	float lastHeight = 0.0f;
	bool  haveHeight = false;
	const int segments = closed ? n : n - 1;
	for (int s = 0; s < segments; ++s)
	{
		const Vector2d & a = points [static_cast<size_t> (s)];
		const Vector2d & b = points [static_cast<size_t> ((s + 1) % n)];
		const int pieces = std::max (1, std::min (48, static_cast<int> (ceilf (a.magnitudeBetween (b) / 2.0f))));
		Vector previous;
		for (int p = 0; p <= pieces; ++p)
		{
			const float t = static_cast<float> (p) / static_cast<float> (pieces);
			const float x = a.x + (b.x - a.x) * t;
			const float z = a.y + (b.y - a.y) * t;
			float height = 0.0f;
			if (m_appearance->getHeight (Vector (x, 0.0f, z), height))
			{
				lastHeight = height;
				haveHeight = true;
			}
			const Vector current (x, (haveHeight ? lastHeight : 0.0f) + 0.3f, z);
			if (p > 0)
				Graphics::drawLine (previous, current, colour);
			previous = current;
		}
	}
}

// ----------------------------------------------------------------------

void TerrainEditData::drawOverlay ()
{
	if (!m_brushMode || getStatus () != S_ok)
		return;

	PointList circle;
	const int circlePoints = 48;

	if (m_strokeActive && !m_strokePoints.empty ())
	{
		if (m_strokeKind == SK_ramp || m_strokeKind == SK_waterfall)
		{
			if (m_strokePoints.size () >= 2 && m_strokePoints.front ().magnitudeBetween (m_strokePoints.back ()) > 0.1f)
			{
				const Vector2d a = m_strokePoints.front ();
				const Vector2d b = m_strokePoints.back ();
				const float length = a.magnitudeBetween (b);
				const Vector2d side (-(b.y - a.y) / length * m_radius, (b.x - a.x) / length * m_radius);
				PointList corridor;
				corridor.push_back (Vector2d (a.x + side.x, a.y + side.y));
				corridor.push_back (Vector2d (b.x + side.x, b.y + side.y));
				corridor.push_back (Vector2d (b.x - side.x, b.y - side.y));
				corridor.push_back (Vector2d (a.x - side.x, a.y - side.y));
				drawGroundLine (corridor, true, m_strokeKind == SK_waterfall ? VectorArgb::solidCyan : VectorArgb::solidGreen);
				PointList middle;
				middle.push_back (a);
				middle.push_back (b);
				drawGroundLine (middle, false, VectorArgb::solidYellow);
			}
			return;
		}

		if (m_strokeKind == SK_stamp)
		{
			drawGroundLine (stampCorners (m_strokePoints.back ()), true, VectorArgb::solidCyan);
			return;
		}

		if (m_strokeShape == SH_rectangle || m_strokeShape == SH_circle)
		{
			if (m_strokePoints.size () >= 2)
			{
				const PointList outline = buildOutline (m_strokeShape, m_strokePoints);
				drawGroundLine (outline, true, m_strokeKind == SK_copy ? VectorArgb::solidYellow : VectorArgb::solidWhite);
			}
			return;
		}

		if (m_strokeShape == SH_polygon)
		{
			drawGroundLine (m_strokePoints, true, m_strokeKind == SK_water ? VectorArgb::solidCyan : VectorArgb::solidWhite);
			return;
		}

		//-- brush strokes: the circle follows the last point
		for (int i = 0; i < circlePoints; ++i)
		{
			const float a = PI_TIMES_2 * static_cast<float> (i) / static_cast<float> (circlePoints);
			circle.push_back (Vector2d (m_strokePoints.back ().x + cosf (a) * m_radius, m_strokePoints.back ().y + sinf (a) * m_radius));
		}
		drawGroundLine (circle, true, VectorArgb::solidGreen);
		return;
	}

	if (!m_hasCursor)
		return;

	//-- not dragging: show where the next stroke would go
	if (m_tool == T_stamp)
	{
		if (m_clipboard.m_valid)
			drawGroundLine (stampCorners (m_cursor), true, VectorArgb::solidCyan);
		return;
	}

	const bool brushShaped = m_tool == T_road || m_tool == T_river || m_tool == T_ramp || m_tool == T_waterfall || (m_shape == SH_brush && m_tool != T_copy && m_tool != T_water);
	if (brushShaped)
	{
		for (int i = 0; i < circlePoints; ++i)
		{
			const float a = PI_TIMES_2 * static_cast<float> (i) / static_cast<float> (circlePoints);
			circle.push_back (Vector2d (m_cursor.x + cosf (a) * m_radius, m_cursor.y + sinf (a) * m_radius));
		}
		drawGroundLine (circle, true, VectorArgb::solidGreen);
	}
	else
	{
		//-- shape tools: a small cross where the drag would start
		PointList line;
		line.push_back (Vector2d (m_cursor.x - 2.0f, m_cursor.y));
		line.push_back (Vector2d (m_cursor.x + 2.0f, m_cursor.y));
		drawGroundLine (line, false, m_tool == T_copy ? VectorArgb::solidYellow : VectorArgb::solidWhite);
		line [0] = Vector2d (m_cursor.x, m_cursor.y - 2.0f);
		line [1] = Vector2d (m_cursor.x, m_cursor.y + 2.0f);
		drawGroundLine (line, false, m_tool == T_copy ? VectorArgb::solidYellow : VectorArgb::solidWhite);
	}
}

// ----------------------------------------------------------------------

bool TerrainEditData::sampleCursorGround (float & height, float & slopeDegrees, int & shaderFamily)
{
	if (!m_hasCursor || getStatus () == S_noTerrain)
		return false;

	return sampleGround (m_cursor, height, slopeDegrees, shaderFamily);
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
		case TerrainEditData::SK_ramp:    return "ramp";
		case TerrainEditData::SK_copy:    return "copy";
		case TerrainEditData::SK_stamp:   return "stamp";
		case TerrainEditData::SK_scatter: return "scatter";
		case TerrainEditData::SK_waterfall: return "waterfall";
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

	if (edit.m_limits.any ())
		fprintf (file, "  limits: %s\n", describeLimits (edit.m_limits).c_str ());

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
	if ((edit.m_shape != SH_brush || edit.m_kind == SK_water) && edit.m_kind != SK_tree && edit.m_kind != SK_smooth && edit.m_kind != SK_ramp && edit.m_kind != SK_stamp && edit.m_kind != SK_scatter)
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

	if (edit.m_kind == SK_scatter)
	{
		int live = 0;
		int seen = 0;
		const std::map<int, ScatterList>::const_iterator found = m_scatter.find (edit.m_strokeId);
		if (found != m_scatter.end ())
			for (ScatterList::const_iterator i = found->second.begin (); i != found->second.end (); ++i)
				if (i->m_live)
				{
					++live;
					if (!i->m_id.empty ())
						++seen;
				}
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize), "Object scatter: %d objects sent to the server, %d seen back (Undo can only remove the ones seen; the rest are removed when they arrive).", live, seen));
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

	if (edit.m_kind == SK_smooth || edit.m_kind == SK_ramp || edit.m_kind == SK_stamp)
	{
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize), "%s stroke: %d ground points baked (patches %.1f m wide).", edit.m_kind == SK_ramp ? "Ramp" : edit.m_kind == SK_stamp ? "Stamp" : "Smooth", static_cast<int> (edit.m_samples.points.size ()), edit.m_param2 * 2.0f));
		summary [summarySize - 1] = '\0';
		return;
	}

	if (edit.m_kind == SK_waterfall)
	{
		IGNORE_RETURN (_snprintf (summary, static_cast<size_t> (summarySize), "Waterfall: %d points, water from %.1f m down to %.1f m.", static_cast<int> (edit.m_points.size ()),
			edit.m_samples.waterHeights.empty () ? 0.0f : edit.m_samples.waterHeights.front (), edit.m_samples.waterHeights.empty () ? 0.0f : edit.m_samples.waterHeights.back ()));
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
		if (i->m_kind == SK_tree || i->m_kind == SK_scatter)
			continue;   // trees are in the baked tree map, written below as the template holds it now

		TerrainGenerator::Layer * const clone = createLayer (i->m_layer->getName (), i->m_points, i->m_kind, i->m_operation, i->m_height, i->m_familyId, i->m_radius, i->m_feather, i->m_param, i->m_param2, i->m_subKind, i->m_samples, i->m_waterShader.c_str (), i->m_shape, i->m_limits);
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

	//-- the storage folder and the server folder are shared: every save keeps the file it replaces
	//-- in terrain/_terrain_backup/<name>_<date_time>.trn
	std::string backup;
	if (::GetFileAttributesA (path.c_str ()) != INVALID_FILE_ATTRIBUTES)
	{
		SYSTEMTIME now;
		::GetLocalTime (&now);
		char stamp [32];
		setMessage (stamp, sizeof (stamp), "%04d%02d%02d_%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
		const std::string backupFolder (directoryOf (path) + "/_terrain_backup");
		IGNORE_RETURN (Os::createDirectories (backupFolder.c_str ()));
		backup = backupFolder + "/" + baseNameOf (path) + "_" + stamp + ".trn";
		if (!::CopyFileA (path.c_str (), backup.c_str (), TRUE))
		{
			setMessage (message, messageSize, "Not saved: could not back up %s to %s.", path.c_str (), backup.c_str ());
			return false;
		}
	}

	if (!writeTerrain (path.c_str (), message, messageSize))
		return false;

	logEvent ("SAVE TO FOLDER %s (%d strokes)%s%s", path.c_str (), getNumberOfEdits (), backup.empty () ? "" : ", previous file kept as ", backup.c_str ());
	if (backup.empty ())
		setMessage (message, messageSize, "Saved %d stroke(s) to %s", getNumberOfEdits (), path.c_str ());
	else
		setMessage (message, messageSize, "Saved %d stroke(s) to %s\nThe previous file was kept as %s", getNumberOfEdits (), path.c_str (), backup.c_str ());
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
	if (edit.m_kind == SK_scatter)
		removeScatterObjects (edit.m_strokeId);
	if (edit.m_kind == SK_river || edit.m_kind == SK_water || edit.m_kind == SK_waterfall)
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
	if (edit.m_kind == SK_scatter)
		createScatterObjects (edit.m_strokeId);
	if (edit.m_kind == SK_river || edit.m_kind == SK_water || edit.m_kind == SK_waterfall)
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
		if (i->m_kind == SK_scatter)
			removeScatterObjects (i->m_strokeId);
		if (i->m_kind == SK_river || i->m_kind == SK_water || i->m_kind == SK_waterfall)
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

	//-- object scatter: learn the ids of objects the server has sent back
	updateScatter ();

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

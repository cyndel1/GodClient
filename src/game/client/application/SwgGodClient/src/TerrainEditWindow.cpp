// ======================================================================
//
// TerrainEditWindow.cpp
// Dockable "Terrain Editor" panel for the God Client.
//
// ======================================================================

#include "SwgGodClient/FirstSwgGodClient.h"
#include "TerrainEditWindow.h"
#include "TerrainEditWindow.moc"

#include "TerrainEditData.h"

#include <qapplication.h>
#include <qcheckbox.h>
#include <qcombobox.h>
#include <qgroupbox.h>
#include <qimage.h>
#include <qfiledialog.h>
#include <qmessagebox.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qlineedit.h>
#include <qlistbox.h>
#include <qpushbutton.h>
#include <qradiobutton.h>
#include <qspinbox.h>
#include <qtimer.h>
#include <qvalidator.h>
#include <qbuttongroup.h>
#include <qscrollview.h>
#include <qvbuttongroup.h>

#include <algorithm>
#include <cstdio>

// ======================================================================

namespace TerrainEditWindowNamespace
{
	const int cs_timerMs            = 100;
	const int cs_heightSampleTicks  = 3;    // sample the cursor height every 300 ms
}

using namespace TerrainEditWindowNamespace;

// ======================================================================

TerrainEditWindow::TerrainEditWindow (QWidget * const parent, const char * const name) :
	QWidget (parent, name),
	m_brushMode (0),
	m_tools (0),
	m_radius (0),
	m_feather (0),
	m_strength (0),
	m_levelHeight (0),
	m_paintFamily (0),
	m_familyIds (),
	m_familyPlanet (),
	m_fractal (0),
	m_fractalIds (),
	m_roughness (0),
	m_terraceStep (0),
	m_terraceFlat (0),
	m_plant (0),
	m_plantKinds (),
	m_plantIds (),
	m_plantDensity (0),
	m_riverDepth (0),
	m_shape (0),
	m_smoothStrength (0),
	m_floraScatter (0),
	m_flattenSelection (0),
	m_clipboardLabel (0),
	m_loadImage (0),
	m_stampRotation (0),
	m_stampMirror (0),
	m_stampScale (0),
	m_imageSize (0),
	m_imageHeight (0),
	m_scatterTemplate (0),
	m_scatterSpacing (0),
	m_scatterTurn (0),
	m_slopeOn (0),
	m_slopeMin (0),
	m_slopeMax (0),
	m_heightOn (0),
	m_heightLow (0),
	m_heightHigh (0),
	m_onTexture (0),
	m_onTextureIds (),
	m_limitsNote (0),
	m_status (0),
	m_editList (0),
	m_undo (0),
	m_redo (0),
	m_removeAll (0),
	m_check (0),
	m_diagnostic (0),
	m_save (0),
	m_saveCopy (0),
	m_revert (0),
	m_fileStatus (0),
	m_timer (0),
	m_shownRevision (-1),
	m_heightCountdown (0),
	m_updating (false)
{
	TerrainEditData & data = TerrainEditData::getInstance ();

	//-- everything sits in a scroll area so the panel works at any dock height
	QVBoxLayout * const outer = new QVBoxLayout (this, 0, 0, "terrainEditOuter");
	QScrollView * const scroll = new QScrollView (this, "terrainEditScroll");
	scroll->setResizePolicy (QScrollView::AutoOneFit);
	scroll->setHScrollBarMode (QScrollView::AlwaysOff);
	scroll->setFrameShape (QFrame::NoFrame);
	outer->addWidget (scroll);

	QWidget * const content = new QWidget (scroll->viewport (), "terrainEditContent");
	scroll->addChild (content);

	//-- a gap on the right keeps the spin-box arrows clear of the panel's scroll bar
	QHBoxLayout * const sides = new QHBoxLayout (content, 6, 0, "terrainEditSides");
	QVBoxLayout * const layout = new QVBoxLayout (4, "terrainEditLayout");
	sides->addLayout (layout, 1);
	sides->addSpacing (2);

	m_brushMode = new QCheckBox ("Terrain brush on (left drag = paint,\nright = alternate)", content, "brushMode");
	layout->addWidget (m_brushMode);

	//-- two columns of tools (button ids follow TerrainEditData::Tool)
	m_tools = new QButtonGroup (2, Qt::Horizontal, "Tool", content, "tools");
	m_tools->setRadioButtonExclusive (true);
	IGNORE_RETURN (new QRadioButton ("Raise (right: lower)",  m_tools, "raise"));
	IGNORE_RETURN (new QRadioButton ("Lower (right: raise)",  m_tools, "lower"));
	IGNORE_RETURN (new QRadioButton ("Flatten",               m_tools, "flatten"));
	IGNORE_RETURN (new QRadioButton ("Level to height box",   m_tools, "level"));
	IGNORE_RETURN (new QRadioButton ("Paint texture",         m_tools, "paint"));
	IGNORE_RETURN (new QRadioButton ("Clear flora",           m_tools, "clearFlora"));
	IGNORE_RETURN (new QRadioButton ("Roughen",               m_tools, "roughen"));
	IGNORE_RETURN (new QRadioButton ("Terrace",               m_tools, "terrace"));
	IGNORE_RETURN (new QRadioButton ("Plant flora",           m_tools, "plantFlora"));
	IGNORE_RETURN (new QRadioButton ("Road (draw a path)",    m_tools, "road"));
	IGNORE_RETURN (new QRadioButton ("River (draw a path)",   m_tools, "river"));
	IGNORE_RETURN (new QRadioButton ("Water level (lake)",    m_tools, "water"));
	IGNORE_RETURN (new QRadioButton ("Smooth",                m_tools, "smooth"));
	IGNORE_RETURN (new QRadioButton ("Ramp (drag end to end)", m_tools, "ramp"));
	IGNORE_RETURN (new QRadioButton ("Copy ground (rectangle)", m_tools, "copy"));
	IGNORE_RETURN (new QRadioButton ("Stamp (click to place)", m_tools, "stamp"));
	IGNORE_RETURN (new QRadioButton ("Object scatter", m_tools, "scatter"));
	layout->addWidget (m_tools);

	m_flattenSelection = new QPushButton ("Flatten under selected objects", content, "flattenSelection");
	layout->addWidget (m_flattenSelection);

	//-- shape fills (ids follow TerrainEditData::Shape)
	QHBoxLayout * const shapeRow = new QHBoxLayout (4, "terrainEditShape");
	layout->addLayout (shapeRow);
	shapeRow->addWidget (new QLabel ("Shape", content));
	m_shape = new QComboBox (false, content, "shape");
	m_shape->insertItem ("Brush (along the drag)");
	m_shape->insertItem ("Rectangle (drag corner to corner)");
	m_shape->insertItem ("Circle (drag from the centre)");
	m_shape->insertItem ("Polygon (drag around the outline)");
	shapeRow->addWidget (m_shape, 1);

	QLabel * const toolHint = new QLabel ("Flatten: the first click sets the height, kept for later strokes (click Flatten again to pick a new one). Flatten/Level/Water: right-click picks the ground height. Paint, Roughen and Plant use the choices below. Road and River: drag along the path; they appear when you let go, use the Paint texture, and are as wide as the brush (size x 2). Textures with a higher priority are drawn over lower ones where they meet, so narrow strokes look cleanest with a high-priority texture (lower in the list). "
		"Shapes: every tool except Road and River can fill a rectangle, circle or polygon instead; the fill appears when you let go, and Soft edge % is how far in from the outline towards the middle it fades in (0% = hard edge). Water level: a flat water surface at the Flatten/Level/Water height inside the shape (Brush shape = polygon); the ground inside is dug down River/Lake depth below the water, with banks as wide as the soft edge. The water is always kept just below the lowest point of the shore (height 0 = fill to just below the shore). Plant flora with a tree: entry plants trees (one per 16 m tile, into empty tiles only), shown when you let go. Smooth: softens bumps along the drag or inside the shape (built when you let go); strokes add up, so go over an area again to smooth it more. "
		"Ramp: drag from one end to the other; when you let go, the ground between becomes a straight slope from the ground height at the start to the ground height at the end, as wide as the brush (size x 2), fading out over the soft edge. Flatten under selected objects: select buildings or objects (untick Terrain brush on to select), then press the button; the ground under each is levelled to its base height, 1 m past its footprint, with a slope back to the old ground as wide as Brush size.", content, "toolHint");
	toolHint->setAlignment (Qt::WordBreak | Qt::AlignLeft);
	layout->addWidget (toolHint);

	QGridLayout * const grid = new QGridLayout (14, 2, 4, "terrainEditGrid");
	layout->addLayout (grid);

	grid->addWidget (new QLabel ("Brush size (m)", content), 0, 0);
	m_radius = new QSpinBox (1, 2048, 1, content, "radius");
	grid->addWidget (m_radius, 0, 1);

	grid->addWidget (new QLabel ("Soft edge (%)", content), 1, 0);
	m_feather = new QSpinBox (0, 100, 5, content, "feather");
	grid->addWidget (m_feather, 1, 1);

	grid->addWidget (new QLabel ("Raise/lower by (m)", content), 2, 0);
	m_strength = new QLineEdit (content, "strength");
	m_strength->setValidator (new QDoubleValidator (0.0, 1000.0, 2, m_strength));
	grid->addWidget (m_strength, 2, 1);

	grid->addWidget (new QLabel ("Flatten/Level/Water height (m)", content), 3, 0);
	m_levelHeight = new QLineEdit (content, "levelHeight");
	m_levelHeight->setValidator (new QDoubleValidator (-10000.0, 10000.0, 2, m_levelHeight));
	grid->addWidget (m_levelHeight, 3, 1);

	grid->addWidget (new QLabel ("Paint texture (also Road/River)", content), 4, 0);
	m_paintFamily = new QComboBox (false, content, "paintFamily");
	grid->addWidget (m_paintFamily, 4, 1);

	grid->addWidget (new QLabel ("Roughen pattern", content), 5, 0);
	m_fractal = new QComboBox (false, content, "roughenPattern");
	grid->addWidget (m_fractal, 5, 1);

	grid->addWidget (new QLabel ("Roughness (m of bumps)", content), 6, 0);
	m_roughness = new QLineEdit (content, "roughness");
	m_roughness->setValidator (new QDoubleValidator (0.0, 500.0, 2, m_roughness));
	grid->addWidget (m_roughness, 6, 1);

	grid->addWidget (new QLabel ("Terrace step (m)", content), 7, 0);
	m_terraceStep = new QLineEdit (content, "terraceStep");
	m_terraceStep->setValidator (new QDoubleValidator (0.1, 500.0, 2, m_terraceStep));
	grid->addWidget (m_terraceStep, 7, 1);

	grid->addWidget (new QLabel ("Terrace flat part (%)", content), 8, 0);
	m_terraceFlat = new QSpinBox (0, 95, 5, content, "terraceFlat");
	grid->addWidget (m_terraceFlat, 8, 1);

	grid->addWidget (new QLabel ("Plant", content), 9, 0);
	m_plant = new QComboBox (false, content, "plant");
	grid->addWidget (m_plant, 9, 1);

	grid->addWidget (new QLabel ("Plant density (%)", content), 10, 0);
	m_plantDensity = new QSpinBox (1, 100, 5, content, "plantDensity");
	grid->addWidget (m_plantDensity, 10, 1);

	grid->addWidget (new QLabel ("River/Lake depth (m)", content), 11, 0);
	m_riverDepth = new QLineEdit (content, "riverDepth");
	m_riverDepth->setValidator (new QDoubleValidator (0.5, 100.0, 2, m_riverDepth));
	grid->addWidget (m_riverDepth, 11, 1);

	grid->addWidget (new QLabel ("Smooth strength (%)", content), 12, 0);
	m_smoothStrength = new QSpinBox (5, 100, 5, content, "smoothStrength");
	grid->addWidget (m_smoothStrength, 12, 1);

	grid->addWidget (new QLabel ("Flora scatter, whole planet (%)", content), 13, 0);
	m_floraScatter = new QSpinBox (0, 100, 10, content, "floraScatter");
	grid->addWidget (m_floraScatter, 13, 1);

	//-- Object scatter
	{
		QGroupBox * const scatter = new QGroupBox ("Object scatter (server objects)", content, "scatter");
		scatter->setColumnLayout (0, Qt::Vertical);
		scatter->layout ()->setSpacing (4);
		scatter->layout ()->setMargin (6);
		QGridLayout * const scatterGrid = new QGridLayout (scatter->layout ());
		scatterGrid->setAlignment (Qt::AlignTop);

		m_scatterTemplate = new QLabel ("", scatter, "scatterTemplate");
		m_scatterTemplate->setAlignment (Qt::WordBreak | Qt::AlignLeft);
		scatterGrid->addMultiCellWidget (m_scatterTemplate, 0, 0, 0, 1);

		scatterGrid->addWidget (new QLabel ("Object spacing (m)", scatter), 1, 0);
		m_scatterSpacing = new QSpinBox (1, 200, 1, scatter, "scatterSpacing");
		scatterGrid->addWidget (m_scatterSpacing, 1, 1);

		m_scatterTurn = new QCheckBox ("Random turn", scatter, "scatterTurn");
		scatterGrid->addMultiCellWidget (m_scatterTurn, 2, 2, 0, 1);

		QLabel * const scatterNote = new QLabel ("Paint with the brush or fill a shape: one object per spacing cell, at a random spot in it, kept at Plant density and where the Limits pass, at most 250 per stroke. They are real server objects: everyone sees them, they are not saved in the .trn, and Undo destroys them.", scatter, "scatterNote");
		scatterNote->setAlignment (Qt::WordBreak | Qt::AlignLeft);
		scatterGrid->addMultiCellWidget (scatterNote, 3, 3, 0, 1);

		layout->addWidget (scatter);
	}

	//-- Copy ground / Stamp / heightmap images
	{
		QGroupBox * const stamp = new QGroupBox ("Stamp (copied ground or heightmap image)", content, "stamp");
		stamp->setColumnLayout (0, Qt::Vertical);
		stamp->layout ()->setSpacing (4);
		stamp->layout ()->setMargin (6);
		QGridLayout * const stampGrid = new QGridLayout (stamp->layout ());
		stampGrid->setAlignment (Qt::AlignTop);

		m_clipboardLabel = new QLabel ("", stamp, "clipboardLabel");
		m_clipboardLabel->setAlignment (Qt::WordBreak | Qt::AlignLeft);
		stampGrid->addMultiCellWidget (m_clipboardLabel, 0, 0, 0, 1);

		m_loadImage = new QPushButton ("Load heightmap image...", stamp, "loadImage");
		stampGrid->addMultiCellWidget (m_loadImage, 1, 1, 0, 1);

		stampGrid->addWidget (new QLabel ("Turn (deg)", stamp), 2, 0);
		m_stampRotation = new QSpinBox (0, 359, 15, stamp, "stampRotation");
		m_stampRotation->setWrapping (true);
		stampGrid->addWidget (m_stampRotation, 2, 1);

		m_stampMirror = new QCheckBox ("Mirror", stamp, "stampMirror");
		stampGrid->addMultiCellWidget (m_stampMirror, 3, 3, 0, 1);

		stampGrid->addWidget (new QLabel ("Height (%; minus = upside down)", stamp), 4, 0);
		m_stampScale = new QSpinBox (-500, 500, 10, stamp, "stampScale");
		stampGrid->addWidget (m_stampScale, 4, 1);

		stampGrid->addWidget (new QLabel ("Image size (m)", stamp), 5, 0);
		m_imageSize = new QSpinBox (4, 1024, 16, stamp, "imageSize");
		stampGrid->addWidget (m_imageSize, 5, 1);

		stampGrid->addWidget (new QLabel ("Image height of white (m)", stamp), 6, 0);
		m_imageHeight = new QLineEdit (stamp, "imageHeight");
		m_imageHeight->setValidator (new QDoubleValidator (-500.0, 500.0, 1, m_imageHeight));
		stampGrid->addWidget (m_imageHeight, 6, 1);

		QLabel * const stampNote = new QLabel ("Copy ground: drag a rectangle; its shape goes to the clipboard (no edit). Stamp: click where the middle should go (drag to move it); it is built when you let go, sitting on the ground around it and fading in over Soft edge. Heightmap images (PNG, BMP, TGA, JPG): black = no change, white = the image height.", stamp, "stampNote");
		stampNote->setAlignment (Qt::WordBreak | Qt::AlignLeft);
		stampGrid->addMultiCellWidget (stampNote, 7, 7, 0, 1);

		layout->addWidget (stamp);
	}

	//-- Smart paint limits
	{
		QGroupBox * const limits = new QGroupBox ("Limits (Paint, Plant flora, Clear flora)", content, "limits");
		limits->setColumnLayout (0, Qt::Vertical);
		limits->layout ()->setSpacing (4);
		limits->layout ()->setMargin (6);
		QGridLayout * const limitGrid = new QGridLayout (limits->layout ());
		limitGrid->setAlignment (Qt::AlignTop);

		m_slopeOn = new QCheckBox ("Slope between (deg)", limits, "slopeOn");
		limitGrid->addWidget (m_slopeOn, 0, 0);
		m_slopeMin = new QSpinBox (0, 90, 5, limits, "slopeMin");
		limitGrid->addWidget (m_slopeMin, 0, 1);
		limitGrid->addWidget (new QLabel ("to", limits), 0, 2);
		m_slopeMax = new QSpinBox (0, 90, 5, limits, "slopeMax");
		limitGrid->addWidget (m_slopeMax, 0, 3);

		m_heightOn = new QCheckBox ("Height between (m)", limits, "heightOn");
		limitGrid->addWidget (m_heightOn, 1, 0);
		m_heightLow = new QLineEdit (limits, "heightLow");
		m_heightLow->setValidator (new QDoubleValidator (-10000.0, 10000.0, 1, m_heightLow));
		limitGrid->addWidget (m_heightLow, 1, 1);
		limitGrid->addWidget (new QLabel ("to", limits), 1, 2);
		m_heightHigh = new QLineEdit (limits, "heightHigh");
		m_heightHigh->setValidator (new QDoubleValidator (-10000.0, 10000.0, 1, m_heightHigh));
		limitGrid->addWidget (m_heightHigh, 1, 3);

		QLabel * const onTextureLabel = new QLabel ("Only where the ground is already", limits);
		onTextureLabel->setAlignment (Qt::WordBreak | Qt::AlignLeft | Qt::AlignVCenter);
		limitGrid->addWidget (onTextureLabel, 2, 0);
		m_onTexture = new QComboBox (false, limits, "onTexture");
		limitGrid->addMultiCellWidget (m_onTexture, 2, 2, 1, 3);

		m_limitsNote = new QLabel ("The stroke only acts where the ground passes every limit that is on. Slope: 0 = flat, 90 = a cliff; 30 to 90 picks steep slopes, 0 to 10 flat ground. 'Only where the ground is already' is NOT the texture to paint (that is Paint texture above): it is the existing texture to paint over or plant on. Leave it at (any texture) unless you want that. The status line shows the height, slope and texture under the cursor.", limits, "limitsNote");
		m_limitsNote->setAlignment (Qt::WordBreak | Qt::AlignLeft);
		limitGrid->addMultiCellWidget (m_limitsNote, 3, 3, 0, 3);

		layout->addWidget (limits);
	}

	m_status = new QLabel ("", content, "status");
	m_status->setTextFormat (Qt::PlainText);
	layout->addWidget (m_status);

	layout->addWidget (new QLabel ("Edits (newest last):", content));
	m_editList = new QListBox (content, "editList");
	m_editList->setMinimumHeight (110);
	layout->addWidget (m_editList, 1);

	QHBoxLayout * const buttons = new QHBoxLayout (4, "terrainEditButtons");
	layout->addLayout (buttons);
	m_undo      = new QPushButton ("Undo", content, "undo");
	m_redo      = new QPushButton ("Redo", content, "redo");
	m_removeAll = new QPushButton ("Remove all", content, "removeAll");
	buttons->addWidget (m_undo);
	buttons->addWidget (m_redo);
	buttons->addWidget (m_removeAll);

	m_check = new QPushButton ("Check last stroke (writes terrain_edit_log.txt)", content, "checkLastStroke");
	layout->addWidget (m_check);
	m_diagnostic = new QLabel ("", content, "diagnostic");
	m_diagnostic->setAlignment (Qt::WordBreak | Qt::AlignLeft);
	layout->addWidget (m_diagnostic);

	//-- saving
	m_fileStatus = new QLabel ("", content, "fileStatus");
	m_fileStatus->setAlignment (Qt::WordBreak | Qt::AlignLeft);
	layout->addWidget (m_fileStatus);

	QHBoxLayout * const fileButtons = new QHBoxLayout (4, "terrainEditFileButtons");
	layout->addLayout (fileButtons);
	m_save     = new QPushButton ("Save", content, "save");
	m_saveCopy = new QPushButton ("Save copy to folder...", content, "saveCopy");
	m_revert   = new QPushButton ("Revert to saved", content, "revert");
	fileButtons->addWidget (m_save);
	fileButtons->addWidget (m_saveCopy);
	fileButtons->addWidget (m_revert);

	QLabel * const note = new QLabel ("Save writes the planet's .trn into the client's loose data folder (backed up first). The server only sees edits after you copy the saved .trn into its data and restart it.", content, "note");
	note->setAlignment (Qt::WordBreak | Qt::AlignLeft);
	layout->addWidget (note);

	//-- drop-down lists hold long names (textures with "#id, priority n"); without this their width
	//-- would push the panel wider than the dock, under the scroll bar. They shrink instead, and the
	//-- full name shows when the list is opened.
	{
		QComboBox * const combos [] = { m_shape, m_paintFamily, m_fractal, m_plant, m_onTexture };
		for (size_t i = 0; i < sizeof (combos) / sizeof (combos [0]); ++i)
		{
			combos [i]->setSizePolicy (QSizePolicy (QSizePolicy::Ignored, QSizePolicy::Fixed));
			combos [i]->setMinimumWidth (80);
		}
	}

	//-- initial values
	m_updating = true;
	m_radius->setValue (static_cast<int> (data.getRadius ()));
	m_feather->setValue (static_cast<int> (data.getFeather () * 100.0f + 0.5f));
	m_strength->setText (QString::number (data.getStrength (), 'f', 1));
	m_levelHeight->setText (QString::number (data.getLevelHeight (), 'f', 1));
	m_roughness->setText (QString::number (data.getRoughness (), 'f', 1));
	m_terraceStep->setText (QString::number (data.getTerraceStep (), 'f', 1));
	m_terraceFlat->setValue (static_cast<int> (data.getTerraceFlat () * 100.0f + 0.5f));
	m_plantDensity->setValue (static_cast<int> (data.getPlantDensity () * 100.0f + 0.5f));
	m_riverDepth->setText (QString::number (data.getRiverDepth (), 'f', 1));
	m_smoothStrength->setValue (static_cast<int> (data.getSmoothStrength () * 100.0f + 0.5f));
	m_stampRotation->setValue (static_cast<int> (data.getStampRotation () + 0.5f));
	m_stampMirror->setChecked (data.getStampMirror ());
	m_stampScale->setValue (static_cast<int> (floorf (data.getStampHeightScale () * 100.0f + 0.5f)));
	m_imageSize->setValue (static_cast<int> (data.getImageSize () + 0.5f));
	m_imageHeight->setText (QString::number (data.getImageHeight (), 'f', 1));
	m_clipboardLabel->setText (QString (data.describeClipboard ().c_str ()));
	m_scatterSpacing->setValue (static_cast<int> (data.getScatterSpacing () + 0.5f));
	m_scatterTurn->setChecked (data.getScatterRandomTurn ());
	{
		const TerrainEditData::Limits & limits = data.getLimits ();
		m_slopeOn->setChecked (limits.m_slopeOn);
		m_slopeMin->setValue (static_cast<int> (limits.m_slopeMin + 0.5f));
		m_slopeMax->setValue (static_cast<int> (limits.m_slopeMax + 0.5f));
		m_heightOn->setChecked (limits.m_heightOn);
		m_heightLow->setText (QString::number (limits.m_heightLow, 'f', 1));
		m_heightHigh->setText (QString::number (limits.m_heightHigh, 'f', 1));
	}
	m_tools->setButton (static_cast<int> (data.getTool ()));
	m_shape->setCurrentItem (static_cast<int> (data.getShape ()));
	m_brushMode->setChecked (data.isBrushMode ());
	m_updating = false;

	//-- connections
	IGNORE_RETURN (connect (m_scatterSpacing, SIGNAL (valueChanged (int)),         this, SLOT (onScatterChangedInt (int))));
	IGNORE_RETURN (connect (m_scatterTurn, SIGNAL (toggled (bool)),                this, SLOT (onScatterChangedBool (bool))));
	IGNORE_RETURN (connect (m_brushMode,   SIGNAL (toggled (bool)),                this, SLOT (onBrushModeToggled (bool))));
	IGNORE_RETURN (connect (m_tools,       SIGNAL (clicked (int)),                 this, SLOT (onToolClicked (int))));
	IGNORE_RETURN (connect (m_radius,      SIGNAL (valueChanged (int)),            this, SLOT (onRadiusChanged (int))));
	IGNORE_RETURN (connect (m_feather,     SIGNAL (valueChanged (int)),            this, SLOT (onFeatherChanged (int))));
	IGNORE_RETURN (connect (m_strength,    SIGNAL (textChanged (const QString &)), this, SLOT (onStrengthChanged (const QString &))));
	IGNORE_RETURN (connect (m_levelHeight, SIGNAL (textChanged (const QString &)), this, SLOT (onLevelHeightChanged (const QString &))));
	IGNORE_RETURN (connect (m_paintFamily, SIGNAL (activated (int)),               this, SLOT (onPaintFamilyActivated (int))));
	IGNORE_RETURN (connect (m_fractal,     SIGNAL (activated (int)),               this, SLOT (onFractalActivated (int))));
	IGNORE_RETURN (connect (m_roughness,   SIGNAL (textChanged (const QString &)), this, SLOT (onRoughnessChanged (const QString &))));
	IGNORE_RETURN (connect (m_terraceStep, SIGNAL (textChanged (const QString &)), this, SLOT (onTerraceStepChanged (const QString &))));
	IGNORE_RETURN (connect (m_terraceFlat, SIGNAL (valueChanged (int)),            this, SLOT (onTerraceFlatChanged (int))));
	IGNORE_RETURN (connect (m_plant,       SIGNAL (activated (int)),               this, SLOT (onPlantActivated (int))));
	IGNORE_RETURN (connect (m_plantDensity, SIGNAL (valueChanged (int)),           this, SLOT (onPlantDensityChanged (int))));
	IGNORE_RETURN (connect (m_riverDepth,  SIGNAL (textChanged (const QString &)), this, SLOT (onRiverDepthChanged (const QString &))));
	IGNORE_RETURN (connect (m_shape,       SIGNAL (activated (int)),               this, SLOT (onShapeActivated (int))));
	IGNORE_RETURN (connect (m_smoothStrength, SIGNAL (valueChanged (int)),         this, SLOT (onSmoothStrengthChanged (int))));
	IGNORE_RETURN (connect (m_floraScatter, SIGNAL (valueChanged (int)),           this, SLOT (onFloraScatterChanged (int))));
	IGNORE_RETURN (connect (m_flattenSelection, SIGNAL (clicked ()),               this, SLOT (onFlattenSelection ())));
	IGNORE_RETURN (connect (m_loadImage,   SIGNAL (clicked ()),                    this, SLOT (onLoadImage ())));
	IGNORE_RETURN (connect (m_stampRotation, SIGNAL (valueChanged (int)),          this, SLOT (onStampChangedInt (int))));
	IGNORE_RETURN (connect (m_stampMirror, SIGNAL (toggled (bool)),                this, SLOT (onStampChangedBool (bool))));
	IGNORE_RETURN (connect (m_stampScale,  SIGNAL (valueChanged (int)),            this, SLOT (onStampChangedInt (int))));
	IGNORE_RETURN (connect (m_imageSize,   SIGNAL (valueChanged (int)),            this, SLOT (onStampChangedInt (int))));
	IGNORE_RETURN (connect (m_imageHeight, SIGNAL (textChanged (const QString &)), this, SLOT (onStampChangedText (const QString &))));
	IGNORE_RETURN (connect (m_slopeOn,     SIGNAL (toggled (bool)),                this, SLOT (onLimitsChangedBool (bool))));
	IGNORE_RETURN (connect (m_slopeMin,    SIGNAL (valueChanged (int)),            this, SLOT (onLimitsChangedInt (int))));
	IGNORE_RETURN (connect (m_slopeMax,    SIGNAL (valueChanged (int)),            this, SLOT (onLimitsChangedInt (int))));
	IGNORE_RETURN (connect (m_heightOn,    SIGNAL (toggled (bool)),                this, SLOT (onHeightLimitToggled (bool))));
	IGNORE_RETURN (connect (m_heightLow,   SIGNAL (textChanged (const QString &)), this, SLOT (onLimitsChangedText (const QString &))));
	IGNORE_RETURN (connect (m_heightHigh,  SIGNAL (textChanged (const QString &)), this, SLOT (onLimitsChangedText (const QString &))));
	IGNORE_RETURN (connect (m_onTexture,   SIGNAL (activated (int)),               this, SLOT (onLimitsChangedInt (int))));
	IGNORE_RETURN (connect (m_undo,        SIGNAL (clicked ()),                    this, SLOT (onUndo ())));
	IGNORE_RETURN (connect (m_redo,        SIGNAL (clicked ()),                    this, SLOT (onRedo ())));
	IGNORE_RETURN (connect (m_removeAll,   SIGNAL (clicked ()),                    this, SLOT (onRemoveAll ())));
	IGNORE_RETURN (connect (m_check,       SIGNAL (clicked ()),                    this, SLOT (onCheckLastStroke ())));
	IGNORE_RETURN (connect (m_save,        SIGNAL (clicked ()),                    this, SLOT (onSave ())));
	IGNORE_RETURN (connect (m_saveCopy,    SIGNAL (clicked ()),                    this, SLOT (onSaveCopy ())));
	IGNORE_RETURN (connect (m_revert,      SIGNAL (clicked ()),                    this, SLOT (onRevert ())));

	m_timer = new QTimer (this, "terrainEditTimer");
	IGNORE_RETURN (connect (m_timer, SIGNAL (timeout ()), this, SLOT (onTimer ())));
	IGNORE_RETURN (m_timer->start (cs_timerMs));

	refreshFromData ();
}

// ----------------------------------------------------------------------

TerrainEditWindow::~TerrainEditWindow ()
{
	m_timer = 0;   // owned by Qt
}

// ======================================================================

void TerrainEditWindow::onBrushModeToggled (const bool on)
{
	if (!m_updating)
		TerrainEditData::getInstance ().setBrushMode (on);
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onToolClicked (const int id)
{
	if (!m_updating && id >= 0 && id <= static_cast<int> (TerrainEditData::T_scatter))
		TerrainEditData::getInstance ().setTool (static_cast<TerrainEditData::Tool> (id));
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onRadiusChanged (const int value)
{
	if (!m_updating)
		TerrainEditData::getInstance ().setRadius (static_cast<float> (value));
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onFeatherChanged (const int value)
{
	if (!m_updating)
		TerrainEditData::getInstance ().setFeather (static_cast<float> (value) / 100.0f);
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onStrengthChanged (const QString & text)
{
	bool ok = false;
	const float value = text.toFloat (&ok);
	if (!m_updating && ok)
		TerrainEditData::getInstance ().setStrength (value);
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onLevelHeightChanged (const QString & text)
{
	bool ok = false;
	const float value = text.toFloat (&ok);
	if (!m_updating && ok)
		TerrainEditData::getInstance ().setLevelHeight (value);
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onPaintFamilyActivated (const int index)
{
	if (m_updating || index < 0 || index >= static_cast<int> (m_familyIds.size ()))
		return;

	TerrainEditData & data = TerrainEditData::getInstance ();
	data.setPaintFamily (m_familyIds [static_cast<size_t> (index)]);

	//-- choosing a texture means you want to paint with it (Road and River use it too)
	if (data.getTool () != TerrainEditData::T_paint && data.getTool () != TerrainEditData::T_road && data.getTool () != TerrainEditData::T_river)
	{
		data.setTool (TerrainEditData::T_paint);
		m_updating = true;
		m_tools->setButton (static_cast<int> (TerrainEditData::T_paint));
		m_updating = false;
	}
}

// ----------------------------------------------------------------------

namespace
{
	void selectTool (TerrainEditData & data, QButtonGroup * const tools, bool & updating, const TerrainEditData::Tool tool)
	{
		if (data.getTool () == tool)
			return;
		data.setTool (tool);
		updating = true;
		tools->setButton (static_cast<int> (tool));
		updating = false;
	}

	bool parseFloat (const QString & text, float & value)
	{
		bool ok = false;
		value = text.toFloat (&ok);
		return ok;
	}
}

void TerrainEditWindow::onFractalActivated (const int index)
{
	if (m_updating || index < 0 || index >= static_cast<int> (m_fractalIds.size ()))
		return;

	TerrainEditData & data = TerrainEditData::getInstance ();
	data.setRoughenFamily (m_fractalIds [static_cast<size_t> (index)]);
	selectTool (data, m_tools, m_updating, TerrainEditData::T_roughen);
}

void TerrainEditWindow::onRoughnessChanged (const QString & text)
{
	float value = 0.0f;
	if (!m_updating && parseFloat (text, value))
		TerrainEditData::getInstance ().setRoughness (value);
}

void TerrainEditWindow::onTerraceStepChanged (const QString & text)
{
	float value = 0.0f;
	if (!m_updating && parseFloat (text, value))
		TerrainEditData::getInstance ().setTerraceStep (value);
}

void TerrainEditWindow::onTerraceFlatChanged (const int value)
{
	if (!m_updating)
		TerrainEditData::getInstance ().setTerraceFlat (static_cast<float> (value) / 100.0f);
}

void TerrainEditWindow::onPlantActivated (const int index)
{
	if (m_updating || index < 0 || index >= static_cast<int> (m_plantIds.size ()))
		return;

	TerrainEditData & data = TerrainEditData::getInstance ();
	data.setPlant (static_cast<TerrainEditData::PlantKind> (m_plantKinds [static_cast<size_t> (index)]), m_plantIds [static_cast<size_t> (index)]);
	selectTool (data, m_tools, m_updating, TerrainEditData::T_plantFlora);
}

void TerrainEditWindow::onSmoothStrengthChanged (const int value)
{
	if (!m_updating)
		TerrainEditData::getInstance ().setSmoothStrength (static_cast<float> (value) / 100.0f);
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onShapeActivated (const int index)
{
	if (m_updating)
		return;

	if (index >= 0 && index <= static_cast<int> (TerrainEditData::SH_polygon))
		TerrainEditData::getInstance ().setShape (static_cast<TerrainEditData::Shape> (index));
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onRiverDepthChanged (const QString & text)
{
	float value = 0.0f;
	if (!m_updating && parseFloat (text, value))
		TerrainEditData::getInstance ().setRiverDepth (value);
}

void TerrainEditWindow::onPlantDensityChanged (const int value)
{
	if (!m_updating)
		TerrainEditData::getInstance ().setPlantDensity (static_cast<float> (value) / 100.0f);
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onUndo ()
{
	IGNORE_RETURN (TerrainEditData::getInstance ().undo ());
	refreshFromData ();
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onRedo ()
{
	IGNORE_RETURN (TerrainEditData::getInstance ().redo ());
	refreshFromData ();
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onRemoveAll ()
{
	TerrainEditData::getInstance ().removeAll ();
	refreshFromData ();
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onLimitsChanged ()
{
	if (m_updating)
		return;

	TerrainEditData::Limits limits;
	limits.m_slopeOn  = m_slopeOn->isChecked ();
	limits.m_slopeMin = static_cast<float> (std::min (m_slopeMin->value (), m_slopeMax->value ()));
	limits.m_slopeMax = static_cast<float> (std::max (m_slopeMin->value (), m_slopeMax->value ()));
	limits.m_heightOn = m_heightOn->isChecked ();

	bool okLow = false;
	bool okHigh = false;
	const float low  = m_heightLow->text ().toFloat (&okLow);
	const float high = m_heightHigh->text ().toFloat (&okHigh);
	const TerrainEditData::Limits & current = TerrainEditData::getInstance ().getLimits ();
	limits.m_heightLow  = okLow ? low : current.m_heightLow;
	limits.m_heightHigh = okHigh ? high : current.m_heightHigh;

	const int textureIndex = m_onTexture->currentItem ();
	limits.m_onTexture = (textureIndex >= 0 && textureIndex < static_cast<int> (m_onTextureIds.size ())) ? m_onTextureIds [static_cast<size_t> (textureIndex)] : 0;

	TerrainEditData::getInstance ().setLimits (limits);
}

void TerrainEditWindow::onLimitsChangedInt (int)                  { onLimitsChanged (); }
void TerrainEditWindow::onLimitsChangedText (const QString &)     { onLimitsChanged (); }
void TerrainEditWindow::onLimitsChangedBool (bool)                { onLimitsChanged (); }

namespace TerrainEditWindowNamespace
{
	//-- minimal TGA reader (8-bit grey, 24/32-bit colour; uncompressed or RLE) -> grey 0..1, row 0 = top
	bool readTarga (const char * const path, int & width, int & height, std::vector<float> & grey)
	{
		FILE * const file = fopen (path, "rb");
		if (!file)
			return false;
		std::vector<unsigned char> bytes;
		{
			unsigned char buffer [65536];
			size_t n;
			while ((n = fread (buffer, 1, sizeof (buffer), file)) > 0)
				bytes.insert (bytes.end (), buffer, buffer + n);
			fclose (file);
		}
		if (bytes.size () < 18)
			return false;

		const int idLength  = bytes [0];
		const int mapType   = bytes [1];
		const int imageType = bytes [2];
		const int mapLength = bytes [5] | (bytes [6] << 8);
		const int mapDepth  = bytes [7];
		width  = bytes [12] | (bytes [13] << 8);
		height = bytes [14] | (bytes [15] << 8);
		const int bpp = bytes [16];
		const bool topDown = (bytes [17] & 0x20) != 0;
		if (width <= 1 || height <= 1 || (imageType != 2 && imageType != 3 && imageType != 10 && imageType != 11) || (bpp != 8 && bpp != 24 && bpp != 32))
			return false;

		size_t pos = 18 + static_cast<size_t> (idLength) + (mapType ? static_cast<size_t> (mapLength * ((mapDepth + 7) / 8)) : 0);
		const int pixelBytes = bpp / 8;
		const size_t count = static_cast<size_t> (width) * static_cast<size_t> (height);
		std::vector<float> pixels (count, 0.0f);

		const bool rle = imageType == 10 || imageType == 11;
		size_t i = 0;
		while (i < count)
		{
			//-- a packet: uncompressed files are one-pixel raw packets
			int run = 1;
			bool repeat = false;
			if (rle)
			{
				if (pos >= bytes.size ())
					return false;
				const unsigned char header = bytes [pos++];
				run = (header & 0x7f) + 1;
				repeat = (header & 0x80) != 0;
			}

			for (int r = 0; r < run && i < count; ++r)
			{
				if (pos + static_cast<size_t> (pixelBytes) > bytes.size ())
					return false;
				const unsigned char * const p = &bytes [pos];
				pixels [i++] = pixelBytes == 1 ? p [0] / 255.0f : (0.114f * p [0] + 0.587f * p [1] + 0.299f * p [2]) / 255.0f;   // BGR
				if (!repeat)
					pos += static_cast<size_t> (pixelBytes);
			}
			if (repeat)
				pos += static_cast<size_t> (pixelBytes);
		}

		grey.resize (count);
		for (int y = 0; y < height; ++y)
		{
			const int sourceRow = topDown ? y : height - 1 - y;
			for (int x = 0; x < width; ++x)
				grey [static_cast<size_t> (y * width + x)] = pixels [static_cast<size_t> (sourceRow * width + x)];
		}
		return true;
	}
}

void TerrainEditWindow::onLoadImage ()
{
	static QString s_lastFile;
	const QString path = QFileDialog::getOpenFileName (s_lastFile, "Heightmap images (*.png *.bmp *.tga *.jpg *.jpeg);;All files (*.*)", this, "loadHeightmap", "Load heightmap image");
	if (path.isEmpty ())
		return;
	s_lastFile = path;

	int width = 0;
	int height = 0;
	std::vector<float> grey;
	bool ok = false;
	if (path.lower ().endsWith (".tga"))
		ok = readTarga (path.latin1 (), width, height, grey);
	else
	{
		QImage image;
		if (image.load (path))
		{
			width  = image.width ();
			height = image.height ();
			grey.resize (static_cast<size_t> (width * height));
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					grey [static_cast<size_t> (y * width + x)] = static_cast<float> (qGray (image.pixel (x, y))) / 255.0f;
			ok = width > 1 && height > 1;
		}
	}

	if (!ok)
	{
		m_diagnostic->setText (QString ("Could not read %1 (PNG, BMP, JPG, or 8/24/32-bit TGA).").arg (path));
		return;
	}

	//-- very large images: keep at most 1024 points a side (the stamp is sampled smoothly anyway)
	const int step = std::max (1, std::max (width, height) / 1024 + (std::max (width, height) % 1024 ? 1 : 0));
	if (step > 1)
	{
		const int w = (width + step - 1) / step;
		const int h = (height + step - 1) / step;
		std::vector<float> reduced (static_cast<size_t> (w * h));
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				reduced [static_cast<size_t> (y * w + x)] = grey [static_cast<size_t> (std::min (height - 1, y * step) * width + std::min (width - 1, x * step))];
		grey.swap (reduced);
		width = w;
		height = h;
	}

	QString name = path;
	const int slash = std::max (name.findRev ('/'), name.findRev ('\\'));
	if (slash >= 0)
		name = name.mid (slash + 1);

	TerrainEditData::getInstance ().setClipboardFromImage (name.latin1 (), width, height, grey);
	refreshFromData ();
}

void TerrainEditWindow::onStampChanged ()
{
	if (m_updating)
		return;
	TerrainEditData & data = TerrainEditData::getInstance ();
	data.setStampRotation (static_cast<float> (m_stampRotation->value ()));
	data.setStampMirror (m_stampMirror->isChecked ());
	data.setStampHeightScale (static_cast<float> (m_stampScale->value ()) / 100.0f);
	data.setImageSize (static_cast<float> (m_imageSize->value ()));
	bool ok = false;
	const float imageHeight = m_imageHeight->text ().toFloat (&ok);
	if (ok)
		data.setImageHeight (imageHeight);
	m_clipboardLabel->setText (QString (data.describeClipboard ().c_str ()));
}

void TerrainEditWindow::onScatterChangedInt (int)
{
	if (!m_updating)
		TerrainEditData::getInstance ().setScatterSpacing (static_cast<float> (m_scatterSpacing->value ()));
}

void TerrainEditWindow::onScatterChangedBool (bool)
{
	if (!m_updating)
		TerrainEditData::getInstance ().setScatterRandomTurn (m_scatterTurn->isChecked ());
}

void TerrainEditWindow::onStampChangedInt (int)                { onStampChanged (); }
void TerrainEditWindow::onStampChangedBool (bool)              { onStampChanged (); }
void TerrainEditWindow::onStampChangedText (const QString &)   { onStampChanged (); }

// ----------------------------------------------------------------------

void TerrainEditWindow::onFlattenSelection ()
{
	m_diagnostic->setText ("Flattening under the selected objects...");
	QApplication::flush ();
	IGNORE_RETURN (TerrainEditData::getInstance ().flattenUnderSelection ());
	refreshFromData ();
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onFloraScatterChanged (const int value)
{
	if (m_updating)
		return;
	TerrainEditData::getInstance ().setFloraScatter (value);
	m_diagnostic->setText (QString ("Flora scatter %1%: every bush, rock and grass patch on the planet was re-placed (0% = the grid look, 100% = anywhere in its cell). Saved with the planet.").arg (value));
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onHeightLimitToggled (const bool on)
{
	//-- turning the height band on: start it around the ground under the cursor (10 m either way),
	//-- so it isn't left at a range that misses the ground here
	if (on && !m_updating)
	{
		float height = 0.0f;
		if (TerrainEditData::getInstance ().sampleCursorHeight (height))
		{
			m_updating = true;
			m_heightLow->setText (QString::number (floorf (height - 10.0f), 'f', 1));
			m_heightHigh->setText (QString::number (ceilf (height + 10.0f), 'f', 1));
			m_updating = false;
			m_diagnostic->setText (QString ("Height band set to 10 m either side of the ground under the cursor (%1 m). Change the two boxes as needed.").arg (QString::number (height, 'f', 1)));
		}
	}
	onLimitsChanged ();
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onCheckLastStroke ()
{
	m_diagnostic->setText ("Measuring...");
	QApplication::flush ();

	char summary [512];
	TerrainEditData::getInstance ().diagnoseLastEdit (summary, static_cast<int> (sizeof (summary)));
	m_diagnostic->setText (QString (summary));
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onSave ()
{
	m_diagnostic->setText ("Saving...");
	QApplication::flush ();

	char message [1536];
	const bool ok = TerrainEditData::getInstance ().save (message, static_cast<int> (sizeof (message)));
	m_diagnostic->setText (QString (ok ? "" : "NOT SAVED: ") + message);
	refreshFromData ();
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onSaveCopy ()
{
	static QString s_lastFolder;

	const QString folder = QFileDialog::getExistingDirectory (s_lastFolder, this, "saveCopyFolder",
		"Folder to save into (the terrain/ subfolder is created inside it)");
	if (folder.isEmpty ())
		return;

	s_lastFolder = folder;
	m_diagnostic->setText ("Saving copy...");
	QApplication::flush ();

	char message [1536];
	const bool ok = TerrainEditData::getInstance ().saveCopyTo (folder.latin1 (), message, static_cast<int> (sizeof (message)));
	m_diagnostic->setText (QString (ok ? "" : "NOT SAVED: ") + message);
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onRevert ()
{
	TerrainEditData & data = TerrainEditData::getInstance ();
	if (data.hasUnsavedEdits ())
	{
		const int answer = QMessageBox::warning (this, "Revert to saved",
			"Throw away the strokes made since the last save and reload the planet from disk?",
			QMessageBox::Yes, QMessageBox::No | QMessageBox::Default | QMessageBox::Escape);
		if (answer != QMessageBox::Yes)
			return;
	}

	m_diagnostic->setText ("Reloading the planet from disk...");
	QApplication::flush ();
	m_diagnostic->setText (data.revertToSaved () ? "Reloaded the planet from disk." : "Could not reload: not on a ground planet.");
	refreshFromData ();
}

// ----------------------------------------------------------------------

void TerrainEditWindow::onTimer ()
{
	TerrainEditData & data = TerrainEditData::getInstance ();

	//-- the timer runs every 100 ms; a much later tick means the main thread was blocked
	{
		static unsigned long s_lastTick = 0;
		const unsigned long tickNow = ::GetTickCount ();
		if (s_lastTick != 0 && tickNow - s_lastTick > 400 && data.isBrushMode ())
			data.logStall (tickNow - s_lastTick);
		s_lastTick = tickNow;
	}

	data.tick ();

	if (data.getRevision () != m_shownRevision)
		refreshFromData ();

	//-- messages from the tool (e.g. a shape that was too small to build)
	{
		std::string message;
		if (data.takeMessage (message))
			m_diagnostic->setText (QString (message.c_str ()));
	}

	//-- object scatter: the template selected in the Tree Browser
	{
		const std::string selected = TerrainEditData::getScatterTemplate ();
		const size_t slash = selected.find_last_of ('/');
		const std::string fileName = slash == std::string::npos ? selected : selected.substr (slash + 1);   // a path can't wrap
		const QString text = selected.empty () ? QString ("Object: none. Select a server template in the Tree Browser (ServerTemplates tab).") : QString ("Object: %1").arg (fileName.c_str ());
		if (m_scatterTemplate->text () != text)
			m_scatterTemplate->setText (text);
	}

	//-- status line
	if (--m_heightCountdown <= 0)
	{
		m_heightCountdown = cs_heightSampleTicks;

		QString text;
		switch (data.getStatus ())
		{
		case TerrainEditData::S_noTerrain:
			text = "Not on a ground planet: log in and load a planet first.";
			break;

		case TerrainEditData::S_singleThreaded:
			text = "Brush disabled: set [ClientTerrain] terrainMultiThreaded=true (the default) and restart.";
			break;

		case TerrainEditData::S_ok:
		default:
			{
				float height = 0.0f;
				if (data.isBrushMode () && !data.isStrokeActive () && data.sampleCursorHeight (height))
				{
					text = QString ("Ground height under cursor: %1 m").arg (QString::number (height, 'f', 1));
					if (data.getTool () == TerrainEditData::T_water)
						text += QString ("\nWater height: %1 m (%2 m above the ground here)").arg (QString::number (data.getLevelHeight (), 'f', 1)).arg (QString::number (data.getLevelHeight () - height, 'f', 1));
					if (TerrainEditData::toolUsesLimits (data.getTool ()))
					{
						float groundHeight = 0.0f;
						float slope = 0.0f;
						int shader = 0;
						if (data.sampleCursorGround (groundHeight, slope, shader))
						{
							const char * const shaderName = data.getShaderFamilyName (shader);
							text += QString ("\nSlope: %1 deg, texture: %2 (#%3)").arg (QString::number (slope, 'f', 0)).arg (shaderName && *shaderName ? shaderName : "?").arg (shader);
							if (data.getLimits ().any ())
							{
								const TerrainEditData::Limits & limits = data.getLimits ();
								const bool passes = (!limits.m_slopeOn || (slope >= limits.m_slopeMin && slope <= limits.m_slopeMax))
									&& (!limits.m_heightOn || (groundHeight >= limits.m_heightLow && groundHeight <= limits.m_heightHigh))
									&& (limits.m_onTexture <= 0 || shader == limits.m_onTexture);
								text += passes ? QString ("  - inside the limits") : QString ("  - outside the limits");
							}
						}
					}
					if (data.getTool () == TerrainEditData::T_flatten)
						text += data.isFlattenHeightSet ()
							? QString ("\nFlatten height: %1 m").arg (QString::number (data.getLevelHeight (), 'f', 1))
							: QString ("\nFlatten: the next stroke picks its height");
				}
				else if (data.isStrokeActive ())
					text = "Painting...";
				else
					text = data.isBrushMode () ? "Move the mouse over the ground." : "Tick 'Terrain brush on' to start editing.";
			}
			break;
		}

		if (m_status->text () != text)
			m_status->setText (text);
	}
}

// ----------------------------------------------------------------------

void TerrainEditWindow::refreshFromData ()
{
	TerrainEditData & data = TerrainEditData::getInstance ();

	m_updating = true;
	{
		//-- the right button can switch the tool to Level and pick a height
		if (m_tools->selectedId () != static_cast<int> (data.getTool ()))
			m_tools->setButton (static_cast<int> (data.getTool ()));

		const QString level = QString::number (data.getLevelHeight (), 'f', 1);
		if (!m_levelHeight->hasFocus () && m_levelHeight->text () != level)
			m_levelHeight->setText (level);

		if (m_brushMode->isChecked () != data.isBrushMode ())
			m_brushMode->setChecked (data.isBrushMode ());

		m_editList->clear ();
		const int n = data.getNumberOfEdits ();
		for (int i = 0; i < n; ++i)
			m_editList->insertItem (QString (data.getEditName (i)));
		if (n > 0)
			m_editList->setBottomItem (n - 1);

		m_undo->setEnabled (n > 0);
		m_redo->setEnabled (data.getNumberOfRedos () > 0);
		m_removeAll->setEnabled (n > 0);

		//-- ground textures of the current planet (refilled when the planet changes)
		{
			char planet [512];
			const QString planetName = data.getTerrainFileName (planet, sizeof (planet)) ? QString (planet) : QString ();
			if (planetName != m_familyPlanet)
			{
				m_familyPlanet = planetName;
				m_paintFamily->clear ();
				m_familyIds.clear ();

				const int count = data.getNumberOfShaderFamilies ();
				int selected = -1;
				for (int i = 0; i < count; ++i)
				{
					const int familyId = data.getShaderFamilyId (i);
					const char * const familyName = data.getShaderFamilyName (familyId);
					m_paintFamily->insertItem (QString ("%1  (#%2, priority %3)").arg (familyName && *familyName ? familyName : "unnamed").arg (familyId).arg (i));
					m_familyIds.push_back (familyId);
					if (familyId == data.getPaintFamily ())
						selected = i;
				}

				//-- default to the first texture so Paint works straight away
				if (selected < 0 && !m_familyIds.empty ())
				{
					selected = 0;
					data.setPaintFamily (m_familyIds [0]);
				}
				if (selected >= 0)
					m_paintFamily->setCurrentItem (selected);
				m_paintFamily->setEnabled (!m_familyIds.empty ());

				//-- flora scatter of this planet
				{
					const int scatter = data.getFloraScatter ();
					m_floraScatter->setEnabled (scatter >= 0);
					if (scatter >= 0)
						m_floraScatter->setValue (scatter);
				}

				//-- Smart paint "only on texture": any, then the planet's textures
				{
					m_onTexture->clear ();
					m_onTextureIds.clear ();
					m_onTexture->insertItem ("(any texture)");
					m_onTextureIds.push_back (0);
					int chosen = 0;
					for (int i = 0; i < count; ++i)
					{
						const int familyId = data.getShaderFamilyId (i);
						const char * const familyName = data.getShaderFamilyName (familyId);
						m_onTexture->insertItem (QString ("%1  (#%2)").arg (familyName && *familyName ? familyName : "unnamed").arg (familyId));
						m_onTextureIds.push_back (familyId);
						if (familyId == data.getLimits ().m_onTexture)
							chosen = static_cast<int> (m_onTextureIds.size ()) - 1;
					}
					m_onTexture->setCurrentItem (chosen);
					if (chosen == 0 && data.getLimits ().m_onTexture != 0)
					{
						TerrainEditData::Limits limits = data.getLimits ();
						limits.m_onTexture = 0;   // that texture isn't on this planet
						data.setLimits (limits);
					}
				}

				//-- roughen patterns (the planet's fractals)
				m_fractal->clear ();
				m_fractalIds.clear ();
				{
					const int fractals = data.getNumberOfFractalFamilies ();
					int chosen = -1;
					for (int i = 0; i < fractals; ++i)
					{
						const int familyId = data.getFractalFamilyId (i);
						const char * const familyName = data.getFractalFamilyName (familyId);
						m_fractal->insertItem (QString ("%1  (#%2)").arg (familyName && *familyName ? familyName : "unnamed").arg (familyId));
						m_fractalIds.push_back (familyId);
						if (familyId == data.getRoughenFamily ())
							chosen = i;
					}
					if (chosen < 0 && !m_fractalIds.empty ())
					{
						chosen = 0;
						data.setRoughenFamily (m_fractalIds [0]);
					}
					if (chosen >= 0)
						m_fractal->setCurrentItem (chosen);
					m_fractal->setEnabled (!m_fractalIds.empty ());
				}

				//-- plants: bushes/rocks (static flora) then grass (radial flora)
				m_plant->clear ();
				m_plantKinds.clear ();
				m_plantIds.clear ();
				{
					const int plants = data.getNumberOfPlantChoices ();
					int chosen = -1;
					for (int i = 0; i < plants; ++i)
					{
						TerrainEditData::PlantKind kind = TerrainEditData::PK_static;
						int familyId = 0;
						if (!data.getPlantChoice (i, kind, familyId))
							continue;
						const char * const plantName = data.getPlantName (kind, familyId);
						m_plant->insertItem (QString ("%1: %2  (#%3)").arg (kind == TerrainEditData::PK_radial ? "grass" : kind == TerrainEditData::PK_tree ? "tree" : "flora").arg (plantName && *plantName ? plantName : "unnamed").arg (familyId));
						m_plantKinds.push_back (static_cast<int> (kind));
						m_plantIds.push_back (familyId);
						if (kind == data.getPlantKind () && familyId == data.getPlantFamily ())
							chosen = static_cast<int> (m_plantIds.size ()) - 1;
					}
					if (chosen < 0 && !m_plantIds.empty ())
					{
						chosen = 0;
						data.setPlant (static_cast<TerrainEditData::PlantKind> (m_plantKinds [0]), m_plantIds [0]);
					}
					if (chosen >= 0)
						m_plant->setCurrentItem (chosen);
					m_plant->setEnabled (!m_plantIds.empty ());
				}
			}
		}

		//-- file status
		char fileName [512];
		char path [1024];
		bool existsOnDisk = false;
		if (data.getTerrainFileName (fileName, sizeof (fileName)) && data.getDefaultSavePath (path, sizeof (path), existsOnDisk))
		{
			m_fileStatus->setText (QString ("Planet file: %1  %2\nSave writes: %3")
				.arg (fileName)
				.arg (data.hasUnsavedEdits () ? "(UNSAVED changes)" : "(no unsaved changes)")
				.arg (path));
			m_save->setEnabled (true);
			m_saveCopy->setEnabled (true);
			m_revert->setEnabled (true);
		}
		else
		{
			m_fileStatus->setText ("Planet file: (not on a ground planet)");
			m_save->setEnabled (false);
			m_saveCopy->setEnabled (false);
			m_revert->setEnabled (false);
		}
	}
	m_updating = false;

	m_clipboardLabel->setText (QString (data.describeClipboard ().c_str ()));
	m_shownRevision = data.getRevision ();
}

// ======================================================================

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

	QVBoxLayout * const layout = new QVBoxLayout (content, 6, 4, "terrainEditLayout");

	m_brushMode = new QCheckBox ("Terrain brush on (left drag = paint, right = alternate)", content, "brushMode");
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
	layout->addWidget (m_tools);

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
		"Shapes: every tool except Road and River can fill a rectangle, circle or polygon instead; the fill appears when you let go, and Soft edge % is how far in from the outline towards the middle it fades in (0% = hard edge). Water level: a flat water surface at the Flatten/Level/Water height inside the shape (Brush shape = polygon); the ground inside is dug down River/Lake depth below the water, with banks as wide as the soft edge. Right-click the ground, then add a metre or two so the water shows above it. Plant flora with a tree: entry plants trees (one per 16 m tile, into empty tiles only), shown when you let go. Smooth: softens bumps along the drag or inside the shape (built when you let go); strokes add up, so go over an area again to smooth it more.", content, "toolHint");
	toolHint->setAlignment (Qt::WordBreak | Qt::AlignLeft);
	layout->addWidget (toolHint);

	QGridLayout * const grid = new QGridLayout (13, 2, 4, "terrainEditGrid");
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
	m_tools->setButton (static_cast<int> (data.getTool ()));
	m_shape->setCurrentItem (static_cast<int> (data.getShape ()));
	m_brushMode->setChecked (data.isBrushMode ());
	m_updating = false;

	//-- connections
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
	if (!m_updating && id >= 0 && id <= static_cast<int> (TerrainEditData::T_smooth))
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

	m_shownRevision = data.getRevision ();
}

// ======================================================================

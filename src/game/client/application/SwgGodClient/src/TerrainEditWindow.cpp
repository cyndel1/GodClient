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

	QVBoxLayout * const layout = new QVBoxLayout (this, 6, 4, "terrainEditLayout");

	m_brushMode = new QCheckBox ("Terrain brush on (left drag = paint, right = alternate)", this, "brushMode");
	layout->addWidget (m_brushMode);

	m_tools = new QVButtonGroup ("Tool", this, "tools");
	m_tools->setRadioButtonExclusive (true);
	IGNORE_RETURN (new QRadioButton ("Raise  (right drag lowers)",            m_tools, "raise"));
	IGNORE_RETURN (new QRadioButton ("Lower  (right drag raises)",            m_tools, "lower"));
	IGNORE_RETURN (new QRadioButton ("Flatten  (first click sets the height, kept for later strokes)", m_tools, "flatten"));
	IGNORE_RETURN (new QRadioButton ("Level to the height box",                                      m_tools, "level"));
	IGNORE_RETURN (new QRadioButton ("Paint ground texture (choose it below)",                       m_tools, "paint"));
	IGNORE_RETURN (new QRadioButton ("Clear flora (trees, rocks, bushes, grass)",                     m_tools, "clearFlora"));
	QLabel * const toolHint = new QLabel ("Flatten/Level: right-click the ground to pick a new height. Click Flatten again to let the next stroke pick one.", m_tools, "toolHint");
	toolHint->setAlignment (Qt::WordBreak | Qt::AlignLeft);
	layout->addWidget (m_tools);

	QGridLayout * const grid = new QGridLayout (5, 2, 4, "terrainEditGrid");
	layout->addLayout (grid);

	grid->addWidget (new QLabel ("Brush size (m)", this), 0, 0);
	m_radius = new QSpinBox (1, 2048, 1, this, "radius");
	grid->addWidget (m_radius, 0, 1);

	grid->addWidget (new QLabel ("Soft edge (%)", this), 1, 0);
	m_feather = new QSpinBox (0, 100, 5, this, "feather");
	grid->addWidget (m_feather, 1, 1);

	grid->addWidget (new QLabel ("Raise/lower by (m)", this), 2, 0);
	m_strength = new QLineEdit (this, "strength");
	m_strength->setValidator (new QDoubleValidator (0.0, 1000.0, 2, m_strength));
	grid->addWidget (m_strength, 2, 1);

	grid->addWidget (new QLabel ("Flatten/Level height (m)", this), 3, 0);
	m_levelHeight = new QLineEdit (this, "levelHeight");
	m_levelHeight->setValidator (new QDoubleValidator (-10000.0, 10000.0, 2, m_levelHeight));
	grid->addWidget (m_levelHeight, 3, 1);

	grid->addWidget (new QLabel ("Paint texture", this), 4, 0);
	m_paintFamily = new QComboBox (false, this, "paintFamily");
	grid->addWidget (m_paintFamily, 4, 1);

	m_status = new QLabel ("", this, "status");
	m_status->setTextFormat (Qt::PlainText);
	layout->addWidget (m_status);

	layout->addWidget (new QLabel ("Edits (newest last):", this));
	m_editList = new QListBox (this, "editList");
	layout->addWidget (m_editList, 1);

	QHBoxLayout * const buttons = new QHBoxLayout (4, "terrainEditButtons");
	layout->addLayout (buttons);
	m_undo      = new QPushButton ("Undo",       this, "undo");
	m_redo      = new QPushButton ("Redo",       this, "redo");
	m_removeAll = new QPushButton ("Remove all", this, "removeAll");
	buttons->addWidget (m_undo);
	buttons->addWidget (m_redo);
	buttons->addWidget (m_removeAll);

	m_check = new QPushButton ("Check last stroke (writes terrain_edit_log.txt)", this, "checkLastStroke");
	layout->addWidget (m_check);
	m_diagnostic = new QLabel ("", this, "diagnostic");
	m_diagnostic->setAlignment (Qt::WordBreak | Qt::AlignLeft);
	layout->addWidget (m_diagnostic);

	//-- saving
	m_fileStatus = new QLabel ("", this, "fileStatus");
	m_fileStatus->setAlignment (Qt::WordBreak | Qt::AlignLeft);
	layout->addWidget (m_fileStatus);

	QHBoxLayout * const fileButtons = new QHBoxLayout (4, "terrainEditFileButtons");
	layout->addLayout (fileButtons);
	m_save     = new QPushButton ("Save",                   this, "save");
	m_saveCopy = new QPushButton ("Save copy to folder...", this, "saveCopy");
	m_revert   = new QPushButton ("Revert to saved",        this, "revert");
	fileButtons->addWidget (m_save);
	fileButtons->addWidget (m_saveCopy);
	fileButtons->addWidget (m_revert);

	QLabel * const note = new QLabel ("Save writes the planet's .trn into the client's loose data folder (backed up first). The server only sees edits after you copy the saved .trn into its data and restart it.", this, "note");
	note->setAlignment (Qt::WordBreak | Qt::AlignLeft);
	layout->addWidget (note);

	//-- initial values
	m_updating = true;
	m_radius->setValue (static_cast<int> (data.getRadius ()));
	m_feather->setValue (static_cast<int> (data.getFeather () * 100.0f + 0.5f));
	m_strength->setText (QString::number (data.getStrength (), 'f', 1));
	m_levelHeight->setText (QString::number (data.getLevelHeight (), 'f', 1));
	m_tools->setButton (static_cast<int> (data.getTool ()));
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
	if (!m_updating && id >= 0 && id <= static_cast<int> (TerrainEditData::T_clearFlora))
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

	//-- choosing a texture means you want to paint with it
	if (data.getTool () != TerrainEditData::T_paint)
	{
		data.setTool (TerrainEditData::T_paint);
		m_updating = true;
		m_tools->setButton (static_cast<int> (TerrainEditData::T_paint));
		m_updating = false;
	}
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
					m_paintFamily->insertItem (QString ("%1  (#%2)").arg (familyName && *familyName ? familyName : "unnamed").arg (familyId));
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

// ======================================================================
//
// TerrainEditWindow.h
// Dockable "Terrain Editor" panel for the God Client.
//
// ======================================================================

#ifndef INCLUDED_TerrainEditWindow_H
#define INCLUDED_TerrainEditWindow_H

// ======================================================================

#include <qstring.h>
#include <qwidget.h>

#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListBox;
class QPushButton;
class QSpinBox;
class QTimer;

// ======================================================================

class TerrainEditWindow : public QWidget
{
	Q_OBJECT; //lint !e1516 !e19 !e1924 !e1762

public:

	TerrainEditWindow (QWidget * parent, const char * name);
	virtual ~TerrainEditWindow ();

public slots:

	void onBrushModeToggled (bool on);
	void onToolClicked (int id);
	void onRadiusChanged (int value);
	void onFeatherChanged (int value);
	void onStrengthChanged (const QString & text);
	void onLevelHeightChanged (const QString & text);
	void onPaintFamilyActivated (int index);
	void onFractalActivated (int index);
	void onRoughnessChanged (const QString & text);
	void onTerraceStepChanged (const QString & text);
	void onTerraceFlatChanged (int value);
	void onPlantActivated (int index);
	void onPlantDensityChanged (int value);
	void onRiverDepthChanged (const QString & text);
	void onShapeActivated (int index);
	void onSmoothStrengthChanged (int value);
	void onUndo ();
	void onRedo ();
	void onRemoveAll ();
	void onCheckLastStroke ();
	void onSave ();
	void onSaveCopy ();
	void onRevert ();
	void onTimer ();

private:

	TerrainEditWindow (const TerrainEditWindow &);
	TerrainEditWindow & operator= (const TerrainEditWindow &);

	void refreshFromData ();

private:

	QCheckBox *    m_brushMode;
	QButtonGroup * m_tools;
	QSpinBox *     m_radius;
	QSpinBox *     m_feather;
	QLineEdit *    m_strength;
	QLineEdit *    m_levelHeight;
	QComboBox *    m_paintFamily;
	std::vector<int> m_familyIds;      // shader family id for each entry in m_paintFamily
	QString        m_familyPlanet;     // planet the list was built for
	QComboBox *    m_fractal;
	std::vector<int> m_fractalIds;
	QLineEdit *    m_roughness;
	QLineEdit *    m_terraceStep;
	QSpinBox *     m_terraceFlat;
	QComboBox *    m_plant;
	std::vector<int> m_plantKinds;
	std::vector<int> m_plantIds;
	QSpinBox *     m_plantDensity;
	QLineEdit *    m_riverDepth;
	QComboBox *    m_shape;
	QSpinBox *     m_smoothStrength;
	QLabel *       m_status;
	QListBox *     m_editList;
	QPushButton *  m_undo;
	QPushButton *  m_redo;
	QPushButton *  m_removeAll;
	QPushButton *  m_check;
	QLabel *       m_diagnostic;
	QPushButton *  m_save;
	QPushButton *  m_saveCopy;
	QPushButton *  m_revert;
	QLabel *       m_fileStatus;
	QTimer *       m_timer;

	int            m_shownRevision;
	int            m_heightCountdown;
	bool           m_updating;
};

// ======================================================================

#endif

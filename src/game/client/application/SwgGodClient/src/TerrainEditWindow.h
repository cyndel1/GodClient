// ======================================================================
//
// TerrainEditWindow.h
// Dockable "Terrain Editor" panel for the God Client.
//
// ======================================================================

#ifndef INCLUDED_TerrainEditWindow_H
#define INCLUDED_TerrainEditWindow_H

// ======================================================================

#include <qwidget.h>

class QButtonGroup;
class QCheckBox;
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

/*
 * Isolated Record - "mission control" dock.
 *
 * A dockable panel (added to OBS like the Audio Mixer / Scenes docks) that
 * lists every source with isolated recording, shows live status (REC, elapsed
 * time, file size), and offers per-source toggles plus global Record All /
 * Stop All and Add Source actions. All OBS state is read through the
 * thread-safe ir:: API (recorder-api.hpp); the dock never touches engine
 * internals directly.
 */
#pragma once

#include <QFrame>

class QTableWidget;
class QTimer;
class QPushButton;
class QLabel;

class IsolatedRecordDock : public QFrame {
	Q_OBJECT

public:
	explicit IsolatedRecordDock(QWidget *parent = nullptr);

private slots:
	void refresh();
	void onRecordAll();
	void onStopAll();
	void onAddSource();
	void onRowToggle();
	void onRowSettings();
	void onRowFolder();
	void onSettings();

private:
	QTableWidget *table_;
	QTimer *timer_;
	QPushButton *recordAll_;
	QPushButton *stopAll_;
	QPushButton *addSource_;
	QPushButton *settings_;
	QLabel *summary_;
};

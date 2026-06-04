#include "dock.hpp"
#include "recorder-api.hpp"
#include "audio-recorder.hpp"

#include <obs.h>
#include <obs-module.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QMenu>
#include <QVariant>
#include <QDialog>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QRadioButton>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFileDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QByteArray>
#include <vector>

/* Columns in the table. */
enum {
	COL_STATUS = 0,
	COL_SOURCE,
	COL_TIME,
	COL_SIZE,
	COL_FOLDER,
	COL_RECORD,
	COL_SETTINGS,
	COL_COUNT,
};

/* We stash the opaque recorder handle on the per-row buttons as a property so
 * slot handlers know which recorder was clicked. Handles are validated inside
 * the ir:: API before use, so a stale handle is harmless. */
static const char *kHandleProp = "ir_handle";
static const char *kAudioProp = "ir_audio";

/* Unified row for the dock table (video filter recorders + audio recorders). */
struct DockRow {
	void *handle;
	QString name;
	bool active;
	int elapsed;
	uint64_t bytes;
	QString file;
	QString folder;
	bool audio;
};

static QString human_size(uint64_t bytes)
{
	if (bytes == 0)
		return QStringLiteral("--");
	const char *units[] = {"B", "KB", "MB", "GB", "TB"};
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	return QString::asprintf("%.1f %s", v, units[u]);
}

static QString human_time(int sec)
{
	if (sec <= 0)
		return QStringLiteral("--");
	return QString::asprintf("%02d:%02d:%02d", sec / 3600, (sec % 3600) / 60, sec % 60);
}

IsolatedRecordDock::IsolatedRecordDock(QWidget *parent) : QFrame(parent)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);

	/* Toolbar. */
	auto *bar = new QHBoxLayout();
	recordAll_ = new QPushButton(obs_module_text("RecordAll"), this);
	stopAll_ = new QPushButton(obs_module_text("StopAll"), this);
	addSource_ = new QPushButton(obs_module_text("AddSource"), this);
	settings_ = new QPushButton(obs_module_text("Settings"), this);
	bar->addWidget(recordAll_);
	bar->addWidget(stopAll_);
	bar->addStretch();
	bar->addWidget(addSource_);
	bar->addWidget(settings_);
	root->addLayout(bar);

	/* Table. */
	table_ = new QTableWidget(0, COL_COUNT, this);
	QStringList headers;
	headers << "" << obs_module_text("Col.Source") << obs_module_text("Col.Time") << obs_module_text("Col.Size")
		<< "" << "" << "";
	table_->setHorizontalHeaderLabels(headers);
	table_->verticalHeader()->setVisible(false);
	table_->setSelectionMode(QAbstractItemView::NoSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->horizontalHeader()->setSectionResizeMode(COL_SOURCE, QHeaderView::Stretch);
	table_->horizontalHeader()->setSectionResizeMode(COL_STATUS, QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(COL_FOLDER, QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(COL_RECORD, QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(COL_SETTINGS, QHeaderView::ResizeToContents);

	/* Let the user drag columns left/right (e.g. move Record to the left).
	 * Restore any saved order and persist changes. */
	auto *hh = table_->horizontalHeader();
	hh->setSectionsMovable(true);
	const std::string saved = ir::dock_column_state();
	if (!saved.empty())
		hh->restoreState(QByteArray::fromBase64(QByteArray::fromStdString(saved)));
	connect(hh, &QHeaderView::sectionMoved, this, [hh](int, int, int) {
		ir::set_dock_column_state(hh->saveState().toBase64().toStdString().c_str());
	});

	root->addWidget(table_);

	summary_ = new QLabel(this);
	summary_->setStyleSheet("color: #888;");
	root->addWidget(summary_);

	connect(recordAll_, &QPushButton::clicked, this, &IsolatedRecordDock::onRecordAll);
	connect(stopAll_, &QPushButton::clicked, this, &IsolatedRecordDock::onStopAll);
	connect(addSource_, &QPushButton::clicked, this, &IsolatedRecordDock::onAddSource);
	connect(settings_, &QPushButton::clicked, this, &IsolatedRecordDock::onSettings);

	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, &IsolatedRecordDock::refresh);
	timer_->start(500);
	refresh();
}

void IsolatedRecordDock::refresh()
{
	/* Audio recorders are reconciled from this UI-thread timer (no graphics
	 * work involved, unlike the video filters which reconcile in video_tick). */
	air::reconcile_all();

	std::vector<DockRow> rows;
	for (const auto &v : ir::snapshot())
		rows.push_back({v.handle, QString::fromStdString(v.name), v.active, v.elapsed_sec, v.bytes,
				QString::fromStdString(v.file), QString::fromStdString(v.folder), false});
	for (const auto &a : air::snapshot())
		rows.push_back({a.handle, QString::fromStdString(a.name), a.active, a.elapsed_sec, a.bytes,
				QString::fromStdString(a.file), QString::fromStdString(a.folder), true});

	table_->setRowCount((int)rows.size());
	int recording = 0;

	for (int i = 0; i < (int)rows.size(); i++) {
		const auto &r = rows[i];
		if (r.active)
			recording++;

		auto *status = new QTableWidgetItem(r.active ? "●" : "○");
		status->setForeground(r.active ? QColor(220, 60, 60) : QColor(120, 120, 120));
		status->setTextAlignment(Qt::AlignCenter);
		table_->setItem(i, COL_STATUS, status);

		/* Prefix audio rows with a note glyph to distinguish them. */
		auto *name = new QTableWidgetItem((r.audio ? QStringLiteral("🎵 ") : QString()) + r.name);
		if (!r.file.isEmpty())
			name->setToolTip(r.file);
		table_->setItem(i, COL_SOURCE, name);

		table_->setItem(i, COL_TIME, new QTableWidgetItem(human_time(r.elapsed)));
		table_->setItem(i, COL_SIZE, new QTableWidgetItem(human_size(r.bytes)));

		auto *btn = new QPushButton(r.active ? obs_module_text("Stop") : obs_module_text("Record"), table_);
		btn->setProperty(kHandleProp, QVariant::fromValue<quintptr>((quintptr)r.handle));
		btn->setProperty(kAudioProp, r.audio);
		connect(btn, &QPushButton::clicked, this, &IsolatedRecordDock::onRowToggle);
		table_->setCellWidget(i, COL_RECORD, btn);

		auto *folderBtn = new QPushButton("📁", table_);
		folderBtn->setProperty("ir_folder", r.folder);
		folderBtn->setFixedWidth(28);
		folderBtn->setToolTip(r.folder.isEmpty() ? QString() : ("Open " + r.folder));
		folderBtn->setEnabled(!r.folder.isEmpty());
		connect(folderBtn, &QPushButton::clicked, this, &IsolatedRecordDock::onRowFolder);
		table_->setCellWidget(i, COL_FOLDER, folderBtn);

		auto *gear = new QPushButton("⚙", table_);
		gear->setProperty(kHandleProp, QVariant::fromValue<quintptr>((quintptr)r.handle));
		gear->setProperty(kAudioProp, r.audio);
		gear->setFixedWidth(28);
		connect(gear, &QPushButton::clicked, this, &IsolatedRecordDock::onRowSettings);
		table_->setCellWidget(i, COL_SETTINGS, gear);
	}

	summary_->setText(QString::asprintf(obs_module_text("Summary.Format"), recording, (int)rows.size()));
}

void IsolatedRecordDock::onRecordAll()
{
	const std::string session = ir::make_session_token();
	ir::record_all(true, session.c_str());
	air::record_all(true, session.c_str());
	refresh();
}

void IsolatedRecordDock::onStopAll()
{
	ir::record_all(false, "");
	air::record_all(false, "");
	refresh();
}

void IsolatedRecordDock::onRowToggle()
{
	auto *btn = qobject_cast<QPushButton *>(sender());
	if (!btn)
		return;
	void *handle = (void *)btn->property(kHandleProp).value<quintptr>();
	const bool audio = btn->property(kAudioProp).toBool();
	const bool currentlyRecording = btn->text() == QString(obs_module_text("Stop"));
	if (audio)
		air::set_record(handle, !currentlyRecording);
	else
		ir::set_record(handle, !currentlyRecording);
	refresh();
}

void IsolatedRecordDock::onRowSettings()
{
	auto *btn = qobject_cast<QPushButton *>(sender());
	if (!btn)
		return;
	void *handle = (void *)btn->property(kHandleProp).value<quintptr>();
	if (!btn->property(kAudioProp).toBool()) {
		ir::open_settings(handle); /* video: OBS's per-source filter dialog */
		return;
	}

	/* Audio recorder settings: destination folder and Remove. (Audio
	 * recorders write 16-bit PCM WAV.) */
	QDialog dlg(this);
	dlg.setWindowTitle(obs_module_text("AudioSettings"));
	auto *form = new QFormLayout(&dlg);

	auto *pathEdit = new QLineEdit(QString::fromStdString(air::get_path(handle)), &dlg);
	auto *browse = new QPushButton("…", &dlg);
	browse->setFixedWidth(32);
	connect(browse, &QPushButton::clicked, &dlg, [&]() {
		QString d = QFileDialog::getExistingDirectory(&dlg, obs_module_text("Path"), pathEdit->text());
		if (!d.isEmpty())
			pathEdit->setText(d);
	});
	auto *pathRow = new QHBoxLayout();
	pathRow->addWidget(pathEdit);
	pathRow->addWidget(browse);
	form->addRow(obs_module_text("Path"), pathRow);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	auto *removeBtn = buttons->addButton(obs_module_text("RemoveAudio"), QDialogButtonBox::DestructiveRole);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	bool removed = false;
	connect(removeBtn, &QPushButton::clicked, &dlg, [&]() {
		removed = true;
		dlg.reject();
	});
	form->addRow(buttons);

	if (dlg.exec() == QDialog::Accepted)
		air::set_path(handle, pathEdit->text().toUtf8().constData());
	else if (removed)
		air::remove(handle);
	refresh();
}

static bool dock_output_exists(const char *id)
{
	const char *out_id;
	size_t i = 0;
	while (obs_enum_output_types(i++, &out_id))
		if (strcmp(out_id, id) == 0)
			return true;
	return false;
}

void IsolatedRecordDock::onRowFolder()
{
	auto *btn = qobject_cast<QPushButton *>(sender());
	if (!btn)
		return;
	QString folder = btn->property("ir_folder").toString();
	if (folder.isEmpty())
		return;
	/* Create it if it doesn't exist yet so opening never fails. */
	QDir().mkpath(folder);
	QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void IsolatedRecordDock::onSettings()
{
	QDialog dlg(this);
	dlg.setWindowTitle(obs_module_text("Settings"));
	auto *lay = new QVBoxLayout(&dlg);

	auto *form = new QFormLayout();

	/* Default destination folder for newly added sources. */
	auto *folderEdit = new QLineEdit(QString::fromStdString(ir::default_folder()), &dlg);
	auto *folderBrowse = new QPushButton("…", &dlg);
	folderBrowse->setFixedWidth(32);
	connect(folderBrowse, &QPushButton::clicked, &dlg, [&]() {
		QString d = QFileDialog::getExistingDirectory(&dlg, obs_module_text("Path"), folderEdit->text());
		if (!d.isEmpty())
			folderEdit->setText(d);
	});
	auto *folderRow = new QHBoxLayout();
	folderRow->addWidget(folderEdit);
	folderRow->addWidget(folderBrowse);
	form->addRow(obs_module_text("Settings.DefaultFolder"), folderRow);

	/* Default recording format for newly added sources. */
	auto *fmtCombo = new QComboBox(&dlg);
	fmtCombo->addItem("Flash Video (.flv)", "flv");
	fmtCombo->addItem("Matroska Video (.mkv)", "mkv");
	fmtCombo->addItem("MPEG-4 (.mp4)", "mp4");
	fmtCombo->addItem("QuickTime (.mov)", "mov");
	if (dock_output_exists("mp4_output"))
		fmtCombo->addItem("Hybrid MP4 (.mp4)", "hybrid_mp4");
	if (dock_output_exists("mov_output"))
		fmtCombo->addItem("Hybrid MOV (.mov)", "hybrid_mov");
	fmtCombo->addItem("Fragmented MP4 (.mp4)", "fragmented_mp4");
	fmtCombo->addItem("Fragmented MOV (.mov)", "fragmented_mov");
	fmtCombo->addItem("MPEG-TS (.ts)", "mpegts");
	{
		const QString cur = QString::fromStdString(ir::default_format());
		int idx = fmtCombo->findData(cur);
		if (idx >= 0)
			fmtCombo->setCurrentIndex(idx);
	}
	form->addRow(obs_module_text("Settings.DefaultFormat"), fmtCombo);

	/* Default recording quality for newly added sources. */
	auto *qCombo = new QComboBox(&dlg);
	qCombo->addItem(obs_module_text("Quality.Stream"), "Stream");
	qCombo->addItem(obs_module_text("Quality.Small"), "Small");
	qCombo->addItem(obs_module_text("Quality.HQ"), "HQ");
	qCombo->addItem(obs_module_text("Quality.Lossless"), "Lossless");
	{
		int idx = qCombo->findData(QString::fromStdString(ir::default_quality()));
		if (idx >= 0)
			qCombo->setCurrentIndex(idx);
	}
	form->addRow(obs_module_text("Settings.DefaultQuality"), qCombo);
	lay->addLayout(form);

	auto *group = new QGroupBox(obs_module_text("Settings.RecordAllGroup"), &dlg);
	auto *gl = new QVBoxLayout(group);
	auto *rbSub = new QRadioButton(obs_module_text("Settings.GroupSubfolder"), group);
	auto *rbSep = new QRadioButton(obs_module_text("Settings.KeepSeparate"), group);
	if (ir::group_record_all())
		rbSub->setChecked(true);
	else
		rbSep->setChecked(true);
	gl->addWidget(rbSub);
	gl->addWidget(rbSep);
	lay->addWidget(group);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	lay->addWidget(buttons);

	if (dlg.exec() == QDialog::Accepted) {
		ir::set_group_record_all(rbSub->isChecked());
		ir::set_default_format(fmtCombo->currentData().toString().toUtf8().constData());
		ir::set_default_quality(qCombo->currentData().toString().toUtf8().constData());
		ir::set_default_folder(folderEdit->text().toUtf8().constData());
	}
}

/* Add a menu action that records an audio source (by weak ref). */
static void add_audio_action(obs_source_t *source, QMenu *menu, QObject *receiver)
{
	const char *name = obs_source_get_name(source);
	QAction *act = menu->addAction(QString::fromUtf8(name ? name : ""));
	obs_weak_source_t *weak = obs_source_get_weak_source(source);
	QObject::connect(act, &QAction::triggered, receiver, [weak]() {
		obs_source_t *s = obs_weak_source_get_source(weak);
		if (s) {
			air::add_source(s);
			obs_source_release(s);
		}
	});
	QObject::connect(act, &QObject::destroyed, [weak]() { obs_weak_source_release(weak); });
}

/* Add a menu action that attaches the video filter to a source (by weak ref). */
static void add_video_action(obs_source_t *source, QMenu *menu, QObject *receiver)
{
	const char *name = obs_source_get_name(source);
	obs_source_t *existing = obs_source_get_filter_by_name(source, "Isolated Record");
	if (existing) {
		obs_source_release(existing); /* already added */
		return;
	}
	QAction *act = menu->addAction(QString::fromUtf8(name ? name : ""));
	obs_weak_source_t *weak = obs_source_get_weak_source(source);
	QObject::connect(act, &QAction::triggered, receiver, [weak]() {
		obs_source_t *s = obs_weak_source_get_source(weak);
		if (s) {
			ir::add_to_source(s);
			obs_source_release(s);
		}
	});
	QObject::connect(act, &QObject::destroyed, [weak]() { obs_weak_source_release(weak); });
}

void IsolatedRecordDock::onAddSource()
{
	QMenu menu(this);

	auto *videoMenu = menu.addMenu(obs_module_text("Add.VideoSource"));
	auto *audioMenu = menu.addMenu(obs_module_text("Add.AudioSource"));

	struct Ctx {
		QMenu *video;
		QMenu *audio;
		QObject *receiver;
	} ctx{videoMenu, audioMenu, this};

	auto cb = [](void *param, obs_source_t *source) -> bool {
		auto *c = (Ctx *)param;
		const uint32_t flags = obs_source_get_output_flags(source);
		if (flags & OBS_SOURCE_VIDEO)
			add_video_action(source, c->video, c->receiver);
		else if (flags & OBS_SOURCE_AUDIO)
			add_audio_action(source, c->audio, c->receiver); /* pure-audio source */
		return true;
	};
	obs_enum_sources(cb, &ctx);
	obs_enum_scenes(cb, &ctx);

	if (videoMenu->isEmpty())
		videoMenu->addAction(obs_module_text("NoSources"))->setEnabled(false);
	if (audioMenu->isEmpty())
		audioMenu->addAction(obs_module_text("NoSources"))->setEnabled(false);

	/* Standalone global mix tracks (1–6). */
	auto *trackMenu = menu.addMenu(obs_module_text("Add.AudioTrack"));
	for (int t = 1; t <= 6; t++) {
		QAction *act = trackMenu->addAction(QString::asprintf(obs_module_text("Add.TrackN"), t));
		QObject::connect(act, &QAction::triggered, this, [t]() { air::add_track(t); });
	}

	menu.exec(addSource_->mapToGlobal(QPoint(0, addSource_->height())));
	refresh();
}

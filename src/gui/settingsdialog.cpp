#include "settingsdialog.h"

#include <QSettings>
#include <QCheckBox>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QDir>
#include <QLineEdit>
#include <QStandardPaths>

namespace {
constexpr int kMaxRecentDirs = 20;
const char* kReopenKey     = "options/reopenLastDir";
const char* kLastDirKey    = "options/lastDir";
const char* kRecentKey     = "recent/dirs";
const char* kBackupDirKey  = "options/backupDir";
const char* kAutoBackupKey = "options/autoBackup";
}

// ── appsettings ──────────────────────────────────────────────────────────

namespace appsettings {

bool reopenLastDir() {
    return QSettings().value(kReopenKey, true).toBool();
}

void setReopenLastDir(bool on) {
    QSettings().setValue(kReopenKey, on);
}

QString lastDir() {
    return QSettings().value(kLastDirKey).toString();
}

void setLastDir(const QString& dir) {
    QSettings().setValue(kLastDirKey, dir);
}

QStringList recentDirs() {
    return QSettings().value(kRecentKey).toStringList();
}

void setRecentDirs(const QStringList& dirs) {
    QSettings().setValue(kRecentKey, dirs);
}

void addRecentDir(const QString& dir) {
    if (dir.isEmpty()) return;
    QString clean = QDir(dir).absolutePath();
    QStringList dirs = recentDirs();
    dirs.removeAll(clean);          // dedupe
    dirs.prepend(clean);            // most-recent first
    while (dirs.size() > kMaxRecentDirs) dirs.removeLast();
    setRecentDirs(dirs);
}

void removeRecentDir(const QString& dir) {
    QStringList dirs = recentDirs();
    dirs.removeAll(dir);
    setRecentDirs(dirs);
}

QString backupDir() {
    QString def = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/backups";
    return QSettings().value(kBackupDirKey, def).toString();
}
void setBackupDir(const QString& dir) { QSettings().setValue(kBackupDirKey, dir); }

bool autoBackup() { return QSettings().value(kAutoBackupKey, false).toBool(); }
void setAutoBackup(bool on) { QSettings().setValue(kAutoBackupKey, on); }

} // namespace appsettings

// ── SettingsDialog ─────────────────────────────────────────────────────────

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Settings");
    setMinimumWidth(480);

    auto* layout = new QVBoxLayout(this);

    // General preferences.
    auto* generalGroup = new QGroupBox("General");
    auto* generalLayout = new QVBoxLayout(generalGroup);
    m_reopenChk = new QCheckBox("Reopen last working directory on startup");
    connect(m_reopenChk, &QCheckBox::toggled, this, &SettingsDialog::onReopenToggled);
    generalLayout->addWidget(m_reopenChk);

    m_autoBackupChk = new QCheckBox("Back up settings before bulk changes (clone / import / regenerate)");
    connect(m_autoBackupChk, &QCheckBox::toggled, this,
            [](bool on) { appsettings::setAutoBackup(on); });
    generalLayout->addWidget(m_autoBackupChk);

    auto* backupRow = new QHBoxLayout();
    backupRow->addWidget(new QLabel("Backup folder:"));
    m_backupDirEdit = new QLineEdit();
    connect(m_backupDirEdit, &QLineEdit::editingFinished, this,
            [this]() { appsettings::setBackupDir(m_backupDirEdit->text()); });
    backupRow->addWidget(m_backupDirEdit, 1);
    auto* browseBtn = new QPushButton("Browse…");
    connect(browseBtn, &QPushButton::clicked, this, &SettingsDialog::onBrowseBackup);
    backupRow->addWidget(browseBtn);
    generalLayout->addLayout(backupRow);
    layout->addWidget(generalGroup);

    // Cached working directories.
    auto* dirGroup = new QGroupBox("NeoGeo ROMset Folders");
    auto* dirLayout = new QVBoxLayout(dirGroup);
    dirLayout->addWidget(new QLabel("Saved folders (double-click to open):"));

    m_dirList = new QListWidget();
    m_dirList->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_dirList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { onOpenSelected(); });
    dirLayout->addWidget(m_dirList);

    auto* btnRow = new QHBoxLayout();
    m_addBtn = new QPushButton("Add…");
    connect(m_addBtn, &QPushButton::clicked, this, &SettingsDialog::onAdd);
    btnRow->addWidget(m_addBtn);

    m_removeBtn = new QPushButton("Remove");
    connect(m_removeBtn, &QPushButton::clicked, this, &SettingsDialog::onRemove);
    btnRow->addWidget(m_removeBtn);

    m_openBtn = new QPushButton("Open");
    connect(m_openBtn, &QPushButton::clicked, this, &SettingsDialog::onOpenSelected);
    btnRow->addWidget(m_openBtn);

    btnRow->addStretch(1);
    dirLayout->addLayout(btnRow);
    layout->addWidget(dirGroup, 1);

    auto* closeRow = new QHBoxLayout();
    closeRow->addStretch(1);
    auto* closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    closeRow->addWidget(closeBtn);
    layout->addLayout(closeRow);

    reload();
}

void SettingsDialog::reload() {
    QSignalBlocker block(m_reopenChk);  // don't re-persist while loading
    m_reopenChk->setChecked(appsettings::reopenLastDir());
    QSignalBlocker block2(m_autoBackupChk);
    m_autoBackupChk->setChecked(appsettings::autoBackup());
    m_backupDirEdit->setText(appsettings::backupDir());

    QString previouslySelected = selectedDir();
    m_dirList->clear();
    m_dirList->addItems(appsettings::recentDirs());
    if (!previouslySelected.isEmpty()) {
        auto matches = m_dirList->findItems(previouslySelected, Qt::MatchExactly);
        if (!matches.isEmpty()) m_dirList->setCurrentItem(matches.first());
    }
}

QString SettingsDialog::selectedDir() const {
    auto* item = m_dirList->currentItem();
    return item ? item->text() : QString();
}

void SettingsDialog::onReopenToggled(bool on) {
    appsettings::setReopenLastDir(on);
}

void SettingsDialog::onBrowseBackup() {
    QString dir = QFileDialog::getExistingDirectory(this, "Choose Backup Folder",
                                                    appsettings::backupDir());
    if (dir.isEmpty()) return;
    m_backupDirEdit->setText(dir);
    appsettings::setBackupDir(dir);
}

void SettingsDialog::onAdd() {
    QString dir = QFileDialog::getExistingDirectory(this, "Add NeoGeo ROMset Folder");
    if (dir.isEmpty()) return;
    appsettings::addRecentDir(dir);
    reload();

    emit openDirectoryRequested(dir);
}

void SettingsDialog::onRemove() {
    QString dir = selectedDir();
    if (dir.isEmpty()) return;
    appsettings::removeRecentDir(dir);
    reload();
}

void SettingsDialog::onOpenSelected() {
    QString dir = selectedDir();
    if (!dir.isEmpty()) emit openDirectoryRequested(dir);
}

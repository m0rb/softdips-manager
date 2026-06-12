#pragma once

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QListWidget;
class QPushButton;

// Thin wrapper over QSettings so keys live in one place and both the main
// window and the settings dialog read/write the same persisted state.
namespace appsettings {
    bool        reopenLastDir();
    void        setReopenLastDir(bool on);

    QString     lastDir();
    void        setLastDir(const QString& dir);

    QStringList recentDirs();
    void        addRecentDir(const QString& dir);     // dedupes, most-recent first
    void        removeRecentDir(const QString& dir);
    void        setRecentDirs(const QStringList& dirs);

    QString     backupDir();                          // defaults under AppData
    void        setBackupDir(const QString& dir);
    bool        autoBackup();                          // back up before bulk changes
    void        setAutoBackup(bool on);
}

// Child window for application settings: general preferences plus management
// of the cached list of working directories (add / remove / open).
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    // Re-read state from QSettings (e.g. after the main window opens a dir).
    void reload();

signals:
    // Emitted when the user asks to open one of the cached directories.
    void openDirectoryRequested(const QString& dir);

private slots:
    void onAdd();
    void onRemove();
    void onOpenSelected();
    void onReopenToggled(bool on);
    void onBrowseBackup();

private:
    QString selectedDir() const;

    QCheckBox*   m_reopenChk;
    QListWidget* m_dirList;
    QPushButton* m_addBtn;
    QPushButton* m_removeBtn;
    QPushButton* m_openBtn;
    QCheckBox*   m_autoBackupChk;
    class QLineEdit* m_backupDirEdit;
};

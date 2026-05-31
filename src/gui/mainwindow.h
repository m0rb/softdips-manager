#pragma once

#include <QMainWindow>
#include <QTableView>
#include <QStandardItemModel>
#include <QItemDelegate>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QGroupBox>
#include <QSplitter>
#include <QStatusBar>
#include <QListWidget>
#include <QTextEdit>
#include <QCheckBox>
#include <QGridLayout>
#include <memory>
#include <map>

#include "softdips.h"

class SettingsDialog;

// Delegate that renders a QComboBox for selecting dip switch options
class DipSwitchDelegate : public QItemDelegate {
    Q_OBJECT
public:
    using QItemDelegate::QItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex&) const override {
        auto* cb = new QComboBox(parent);
        // Commit the moment the user picks a value, so "Save Changes" enables
        // immediately instead of waiting for the editor to lose focus.
        connect(cb, QOverload<int>::of(&QComboBox::activated),
                this, &DipSwitchDelegate::commitAndCloseEditor);
        return cb;
    }

private slots:
    void commitAndCloseEditor() {
        auto* editor = qobject_cast<QComboBox*>(sender());
        if (!editor) return;
        emit commitData(editor);
        emit closeEditor(editor);
    }

public:

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        auto* cb = qobject_cast<QComboBox*>(editor);
        if (!cb) return;
        QVariant data = index.data(Qt::UserRole + 1);
        if (!data.isValid()) return;
        QStringList options = data.toStringList();
        cb->clear();
        cb->addItems(options);
        QString currentValue = index.data(Qt::DisplayRole).toString();
        int idx = options.indexOf(currentValue);
        if (idx >= 0) cb->setCurrentIndex(idx);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        auto* cb = qobject_cast<QComboBox*>(editor);
        if (!cb) return;
        model->setData(index, cb->currentText(), Qt::DisplayRole);
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex&) const override {
        editor->setGeometry(option.rect);
    }
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    // Open a path given on the command line: a directory is loaded as a game
    // collection, anything else as a single .softdips file.
    void openPath(const std::string& path);

private slots:
    void openFile();
    void openDirectory();
    void saveFile();
    void resetToDefaults();
    void openSettings();
    void about();
    void onGameClicked(int row);
    void cloneSettings();
    void createFromRom();
    void auditTitles();
    void generateAllSoftdips();
    void generateSelectedSoftdips();

private:
    void setupUI();
    void setupMenuBar();
    void loadFile(const std::string& filePath);
    void loadDirectory(const std::string& dirPath);
    void selectGame(int index);
    void updateTable();
    void syncTableToSource();
    // Composite "MM [min] SS [sec]" editor for a split Time setting.
    QWidget* makeTimeWidget(softdips::DipSwitch* minSw, softdips::DipSwitch* secSw);
    void updateStatusBar();
    void updateTitlesSelectAllState();
    void logMessage(const QString& msg);

    // One source setting (name + chosen value) to clone onto other titles.
    struct CloneItem { std::string name; std::string newValue; };
    // Resolve + apply the chosen settings to the chosen titles (preview, then
    // per-title confirm for ambiguous matches), and save each modified title.
    void executeClone(const std::vector<CloneItem>& toApply,
                      const std::vector<int>& titleIndices);
    // Generate .softdips (from each P-ROM) for the given titles that lack one.
    void generateSoftdipsFor(const std::vector<int>& indices, const QString& noun);

    // UI
    QSplitter* m_splitter;

    // Left: game list
    QWidget* m_leftPanel;
    QListWidget* m_gameList;
    QCheckBox* m_selectAllTitles;
    QPushButton* m_createFromRomBtn;

    // Right: editor
    QWidget* m_rightPanel;
    QTableView* m_tableView;
    QStandardItemModel* m_model;
    DipSwitchDelegate* m_delegate;
    QLabel* m_gameLabel;
    QPushButton* m_saveButton;
    QPushButton* m_resetButton;
    // Maps each table row to its switch (nullptr for Time rows, which are
    // edited live by their composite widget rather than the model text).
    QVector<softdips::DipSwitch*> m_rowSwitch;

    // Log
    QTextEdit* m_logView;

    // Settings child window (created lazily)
    SettingsDialog* m_settingsDialog = nullptr;

    // Data
    std::string m_currentFilePath;
    std::optional<softdips::SoftDipsFile> m_softDipsFile;
    int m_currentGameIndex = -1;

    struct GameEntry {
        std::string dirName;
        std::string filePath;
        bool hasSoftDips;
        std::string dirPath;
        std::optional<softdips::SoftDipsFile> softDips;
    };
    std::vector<GameEntry> m_games;
};
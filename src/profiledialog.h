// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>

class QListWidget;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QSpinBox;
class ProfileStore;
class OneDriveController;

/** Native editor for isolated DriveBeacon synchronization profiles. */
class ProfileDialog final : public QDialog
{
    Q_OBJECT

public:
    /** Creates an editor bound to the persistent profile store and active controller. */
    explicit ProfileDialog(ProfileStore *store, OneDriveController *controller,
                           QWidget *parent = nullptr);

Q_SIGNALS:
    /** Requests relaunching DriveBeacon with the selected profile. */
    void useProfileRequested(const QString &profileName);

private Q_SLOTS:
    /** Loads the profile represented by the selected list row. */
    void selectProfile(int row);
    /** Resets the editor to a new Graph-oriented profile template. */
    void createProfile();
    /** Loads the selected profile without changing the active process. */
    void loadSelectedProfile();
    /** Validates and persists the values currently shown by the editor. */
    void saveProfile();
    /** Saves the profile and requests a relaunch using its isolated state. */
    void useProfile();
    /** Starts authentication for the active Graph profile. */
    void connectGraph();
    /** Opens the browser URL emitted by the OAuth helper. */
    void openVerificationPage();
    /** Recomputes authentication controls from controller state. */
    void updateGraphStatus();
    /** Requests a fresh first-level folder listing from Graph. */
    void refreshRemoteFolders();
    /** Rebuilds the tree while preserving the profile's include/exclude policy. */
    void populateRemoteFolders();
    /** Keeps include and exclude checkboxes mutually exclusive. */
    void updateFolderSelection(QTreeWidgetItem *item, int column);

private:
    /** Refreshes the profile list and optionally selects a named profile. */
    void refreshProfileList(const QString &selectName = {});
    /** Copies persisted profile values into the editor controls. */
    void loadProfile(const QString &name);

    ProfileStore *m_store;
    OneDriveController *m_controller;
    QListWidget *m_profileList;
    QLineEdit *m_nameEdit;
    QComboBox *m_backendCombo;
    QLineEdit *m_directoryEdit;
    QComboBox *m_availabilityCombo;
    QCheckBox *m_syncEnabledCheck;
    QCheckBox *m_globalSyncEnabledCheck;
    QTreeWidget *m_folderTree;
    QPushButton *m_refreshFoldersButton;
    QSpinBox *m_remoteIntervalSpin;
    QSpinBox *m_concurrentDownloadsSpin;
    QSpinBox *m_concurrentUploadsSpin;
    QSpinBox *m_concurrentLargeTransfersSpin;
    QLineEdit *m_clientIdEdit;
    QPushButton *m_clientIdHelpButton;
    QLineEdit *m_driveIdEdit;
    QLabel *m_graphStatusLabel;
    QLabel *m_verificationLabel;
    QPushButton *m_connectButton;
    QPushButton *m_openVerificationButton;
    QLineEdit *m_responseUrlEdit;
    QPushButton *m_completeButton;
    bool m_graphConnectionPending = false;
    bool m_loadingFolders = false;
};

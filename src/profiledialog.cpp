// SPDX-License-Identifier: GPL-3.0-only

#include "profiledialog.h"

#include "onedrivecontroller.h"
#include "profilestore.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

ProfileDialog::ProfileDialog(ProfileStore *store, OneDriveController *controller, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_controller(controller)
    , m_profileList(new QListWidget(this))
    , m_nameEdit(new QLineEdit(this))
    , m_backendCombo(new QComboBox(this))
    , m_directoryEdit(new QLineEdit(this))
    , m_availabilityCombo(new QComboBox(this))
    , m_syncEnabledCheck(new QCheckBox(i18n("Synchronize this account"), this))
    , m_globalSyncEnabledCheck(new QCheckBox(i18n("Synchronize all accounts"), this))
    , m_folderTree(new QTreeWidget(this))
    , m_refreshFoldersButton(new QPushButton(i18n("Refresh folders"), this))
    , m_remoteIntervalSpin(new QSpinBox(this))
    , m_concurrentDownloadsSpin(new QSpinBox(this))
    , m_concurrentUploadsSpin(new QSpinBox(this))
    , m_concurrentLargeTransfersSpin(new QSpinBox(this))
    , m_clientIdEdit(new QLineEdit(this))
    , m_clientIdHelpButton(new QPushButton(i18n("How to create one…"), this))
    , m_driveIdEdit(new QLineEdit(this))
    , m_graphStatusLabel(new QLabel(this))
    , m_verificationLabel(new QLabel(this))
    , m_connectButton(new QPushButton(i18n("Connect"), this))
    , m_openVerificationButton(new QPushButton(i18n("Open sign-in page"), this))
    , m_responseUrlEdit(new QLineEdit(this))
    , m_completeButton(new QPushButton(i18n("Complete sign-in"), this))
{
    setWindowTitle(i18n("DriveBeacon profiles"));
    resize(720, 430);

    m_backendCombo->addItem(i18n("abraunegg journal"), QStringLiteral("abraunegg-journal"));
    m_backendCombo->addItem(i18n("Microsoft Graph"), QStringLiteral("graph"));
    m_availabilityCombo->addItem(i18n("Keep local copy"), QStringLiteral("keep-local"));
    m_availabilityCombo->addItem(i18n("Remote only"), QStringLiteral("remote-only"));
    m_availabilityCombo->addItem(i18n("Download on demand"), QStringLiteral("on-demand"));
    m_folderTree->setHeaderLabels({i18n("Remote folder"), i18n("Sync"), i18n("Exclude")});
    m_folderTree->setRootIsDecorated(false);
    m_folderTree->setAlternatingRowColors(true);
    m_folderTree->setMinimumHeight(150);
    m_remoteIntervalSpin->setRange(10, 3600);
    m_remoteIntervalSpin->setSuffix(i18n(" s"));
    m_concurrentDownloadsSpin->setRange(1, 8);
    m_concurrentUploadsSpin->setRange(1, 8);
    m_concurrentLargeTransfersSpin->setRange(1, 4);
    m_clientIdEdit->setToolTip(i18n(
        "A public Microsoft application client ID identifies DriveBeacon to Microsoft. "
        "It is not a password. Leave the packaged default unchanged unless you want to "
        "use your own Microsoft Entra application registration."));
    m_clientIdHelpButton->setToolTip(i18n(
        "Learn which Microsoft Entra account types, permissions, and redirect settings "
        "are required for a personal or multi-tenant application."));
    m_syncEnabledCheck->setToolTip(i18n(
        "Pause this account without deleting its local files, tokens, deltas, or baselines."));
    m_globalSyncEnabledCheck->setToolTip(i18n(
        "Pause or resume every configured account without changing each account's own setting."));

    auto *profileButtons = new QHBoxLayout;
    auto *newButton = new QPushButton(i18n("New"), this);
    auto *loadButton = new QPushButton(i18n("Load profile"), this);
    auto *saveButton = new QPushButton(i18n("Save"), this);
    auto *useButton = new QPushButton(i18n("Use profile"), this);
    profileButtons->addWidget(newButton);
    profileButtons->addWidget(loadButton);
    profileButtons->addWidget(saveButton);
    profileButtons->addWidget(useButton);

    auto *profileColumn = new QVBoxLayout;
    profileColumn->addWidget(new QLabel(i18n("Profiles"), this));
    profileColumn->addWidget(m_profileList);
    profileColumn->addLayout(profileButtons);

    auto *directoryRow = new QHBoxLayout;
    directoryRow->addWidget(m_directoryEdit);
    auto *browseButton = new QPushButton(i18n("Browse…"), this);
    directoryRow->addWidget(browseButton);

    auto *graphBox = new QGroupBox(i18n("Microsoft Graph"), this);
    auto *graphForm = new QFormLayout(graphBox);
    auto *clientIdRow = new QHBoxLayout;
    clientIdRow->addWidget(m_clientIdEdit, 1);
    clientIdRow->addWidget(m_clientIdHelpButton);
    graphForm->addRow(i18n("Application client ID:"), clientIdRow);
    graphForm->addRow(i18n("Remote drive ID (optional):"), m_driveIdEdit);
    auto *graphActions = new QHBoxLayout;
    graphActions->addWidget(m_connectButton);
    graphActions->addWidget(m_openVerificationButton);
    graphActions->addWidget(m_graphStatusLabel, 1);
    graphForm->addRow({}, graphActions);
    graphForm->addRow(i18n("Browser response URL:"), m_responseUrlEdit);
    graphForm->addRow({}, m_completeButton);
    graphForm->addRow({}, m_verificationLabel);

    auto *form = new QFormLayout;
    form->addRow(i18n("Profile name:"), m_nameEdit);
    form->addRow(i18n("Backend:"), m_backendCombo);
    form->addRow(i18n("Local directory:"), directoryRow);
    form->addRow(i18n("Availability:"), m_availabilityCombo);
    form->addRow(i18n("Account state:"), m_syncEnabledCheck);
    form->addRow(i18n("Global state:"), m_globalSyncEnabledCheck);
    form->addRow(i18n("Remote check interval:"), m_remoteIntervalSpin);
    form->addRow(i18n("Simultaneous downloads:"), m_concurrentDownloadsSpin);
    form->addRow(i18n("Simultaneous uploads:"), m_concurrentUploadsSpin);
    form->addRow(i18n("Simultaneous large transfers:"), m_concurrentLargeTransfersSpin);
    auto *folderBox = new QVBoxLayout;
    folderBox->addWidget(new QLabel(i18n("Select first-level remote folders to synchronize or exclude:"), this));
    folderBox->addWidget(m_folderTree);
    folderBox->addWidget(m_refreshFoldersButton, 0, Qt::AlignRight);
    form->addRow(i18n("Remote folders:"), folderBox);
    form->addRow(graphBox);

    auto *root = new QHBoxLayout(this);
    root->addLayout(profileColumn, 1);
    root->addLayout(form, 2);

    connect(m_profileList, &QListWidget::currentRowChanged,
            this, &ProfileDialog::selectProfile);
    connect(newButton, &QPushButton::clicked, this, &ProfileDialog::createProfile);
    connect(loadButton, &QPushButton::clicked, this, &ProfileDialog::loadSelectedProfile);
    connect(saveButton, &QPushButton::clicked, this, &ProfileDialog::saveProfile);
    connect(useButton, &QPushButton::clicked, this, &ProfileDialog::useProfile);
    connect(browseButton, &QPushButton::clicked, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(
            this, i18n("Select local directory"), m_directoryEdit->text());
        if (!directory.isEmpty()) {
            m_directoryEdit->setText(directory);
        }
    });
    connect(m_backendCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { updateGraphStatus(); });
    connect(m_connectButton, &QPushButton::clicked, this, &ProfileDialog::connectGraph);
    connect(m_clientIdHelpButton, &QPushButton::clicked, this, [this] {
        // Keep registration guidance next to the setting so users can choose
        // between the packaged public client and their own application.
        QMessageBox help(this);
        help.setWindowTitle(i18n("Microsoft Graph application client ID"));
        help.setText(i18n(
            "DriveBeacon uses a public application client ID to start Microsoft's OAuth "
            "sign-in flow. The ID does not contain account credentials and can be shared "
            "with the application. Access is granted separately by each Microsoft user."));
        help.setInformativeText(i18n(
            "The packaged value is suitable for normal use. You may replace it with your "
            "own Microsoft Entra app registration if you need to control its branding, "
            "supported account types, permissions, or consent policy. Configure the app "
            "as a public desktop client and allow personal Microsoft accounts and/or "
            "accounts from any organizational directory as appropriate. The redirect URI "
            "must match the one configured for the app."));
        auto *docsButton = help.addButton(i18n("Open Microsoft instructions"),
                                          QMessageBox::AcceptRole);
        help.addButton(QMessageBox::Close);
        help.exec();
        if (help.clickedButton() == docsButton) {
            QDesktopServices::openUrl(QUrl(
                QStringLiteral("https://learn.microsoft.com/en-us/entra/identity-platform/quickstart-register-app")));
        }
    });
    connect(m_openVerificationButton, &QPushButton::clicked,
            this, &ProfileDialog::openVerificationPage);
    connect(m_completeButton, &QPushButton::clicked, this, [this] {
        m_graphConnectionPending = true;
        m_graphStatusLabel->setText(i18n("Completing sign-in…"));
        m_controller->completeGraphLogin(m_responseUrlEdit->text());
    });
    connect(m_controller, &OneDriveController::graphAuthChanged,
            this, &ProfileDialog::updateGraphStatus);
    connect(m_controller, &OneDriveController::graphRemoteFoldersChanged,
            this, &ProfileDialog::populateRemoteFolders);
    connect(m_folderTree, &QTreeWidget::itemChanged,
            this, &ProfileDialog::updateFolderSelection);
    connect(m_refreshFoldersButton, &QPushButton::clicked,
            this, &ProfileDialog::refreshRemoteFolders);

    refreshProfileList(m_controller->profileName());
    updateGraphStatus();
    refreshRemoteFolders();
}

void ProfileDialog::refreshProfileList(const QString &selectName)
{
    const QString selected = selectName.isEmpty() ? m_nameEdit->text() : selectName;
    int selectedRow = -1;
    {
        const QSignalBlocker blocker(m_profileList);
        m_profileList->clear();
        m_profileList->addItems(m_store->profileNames());
        for (int row = 0; row < m_profileList->count(); ++row) {
            if (m_profileList->item(row)->text() == selected) {
                selectedRow = row;
                break;
            }
        }
        if (selectedRow < 0 && m_profileList->count() > 0) {
            selectedRow = 0;
        }
        if (selectedRow >= 0) {
            m_profileList->setCurrentRow(selectedRow);
        }
    }
    if (selectedRow >= 0) {
        loadProfile(m_profileList->item(selectedRow)->text());
    }
}

void ProfileDialog::selectProfile(int row)
{
    if (row >= 0 && row < m_profileList->count()) {
        loadProfile(m_profileList->item(row)->text());
    }
}

void ProfileDialog::loadSelectedProfile()
{
    const QListWidgetItem *item = m_profileList->currentItem();
    if (item != nullptr) {
        loadProfile(item->text());
    }
}

void ProfileDialog::loadProfile(const QString &name)
{
    const SyncProfile profile = m_store->load(name);
    m_nameEdit->setText(profile.name);
    m_backendCombo->setCurrentIndex(m_backendCombo->findData(syncBackendName(profile.backend)));
    m_directoryEdit->setText(profile.localDirectory);
    m_availabilityCombo->setCurrentIndex(
        m_availabilityCombo->findData(localAvailabilityName(profile.availability)));
    m_syncEnabledCheck->setChecked(profile.syncEnabled);
    m_globalSyncEnabledCheck->setChecked(m_store->globalSyncEnabled());
    m_remoteIntervalSpin->setValue(profile.remoteCheckIntervalSeconds);
    m_concurrentDownloadsSpin->setValue(profile.concurrentDownloads);
    m_concurrentUploadsSpin->setValue(profile.concurrentUploads);
    m_concurrentLargeTransfersSpin->setValue(profile.concurrentLargeTransfers);
    m_clientIdEdit->setText(profile.graphClientId);
    m_driveIdEdit->setText(profile.remoteDriveId);
    updateGraphStatus();
    populateRemoteFolders();
}

void ProfileDialog::createProfile()
{
    m_nameEdit->setText(QStringLiteral("graph-test"));
    m_backendCombo->setCurrentIndex(m_backendCombo->findData(QStringLiteral("graph")));
    m_directoryEdit->setText(QDir::home().filePath(QStringLiteral("OneDrive-Graph-Test")));
    m_availabilityCombo->setCurrentIndex(
        m_availabilityCombo->findData(QStringLiteral("keep-local")));
    m_syncEnabledCheck->setChecked(true);
    m_globalSyncEnabledCheck->setChecked(m_store->globalSyncEnabled());
    m_concurrentDownloadsSpin->setValue(2);
    m_concurrentUploadsSpin->setValue(2);
    m_concurrentLargeTransfersSpin->setValue(1);
    m_folderTree->clear();
    m_clientIdEdit->clear();
    m_driveIdEdit->clear();
    m_profileList->clearSelection();
    updateGraphStatus();
}

void ProfileDialog::saveProfile()
{
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty() || m_directoryEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, i18n("Invalid profile"),
                             i18n("A profile name and local directory are required."));
        return;
    }

    // Update the existing profile instead of rebuilding it. In particular,
    // keep Graph's delta cursor and both baselines when only UI settings
    // (limits or folder policy) change.
    SyncProfile profile = m_store->load(name);
    profile.name = name;
    profile.backend = syncBackendFromName(m_backendCombo->currentData().toString());
    profile.localDirectory = QDir::cleanPath(
        QFileInfo(m_directoryEdit->text().trimmed()).absoluteFilePath());
    profile.availability = localAvailabilityFromName(
        m_availabilityCombo->currentData().toString());
    profile.syncEnabled = m_syncEnabledCheck->isChecked();
    profile.remoteCheckIntervalSeconds = m_remoteIntervalSpin->value();
    profile.concurrentDownloads = m_concurrentDownloadsSpin->value();
    profile.concurrentUploads = m_concurrentUploadsSpin->value();
    profile.concurrentLargeTransfers = m_concurrentLargeTransfersSpin->value();
    for (int row = 0; row < m_folderTree->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_folderTree->topLevelItem(row);
        if (item->checkState(1) == Qt::Checked) {
            profile.includedFolders.append(item->text(0));
        }
        if (item->checkState(2) == Qt::Checked) {
            profile.excludedFolders.append(item->text(0));
        }
    }
    profile.graphClientId = m_clientIdEdit->text().trimmed();
    profile.remoteDriveId = m_driveIdEdit->text().trimmed();
    m_store->save(profile);
    m_store->setGlobalSyncEnabled(m_globalSyncEnabledCheck->isChecked());
    refreshProfileList(name);
}

void ProfileDialog::useProfile()
{
    saveProfile();
    const QString name = m_nameEdit->text().trimmed();
    if (!name.isEmpty()) {
        m_store->setActiveProfileName(name);
        Q_EMIT useProfileRequested(name);
    }
}

void ProfileDialog::connectGraph()
{
    saveProfile();
    if (syncBackendFromName(m_backendCombo->currentData().toString()) != SyncBackend::MicrosoftGraph) {
        return;
    }
    if (m_controller->profileName() != m_nameEdit->text().trimmed()) {
        QMessageBox::information(this, i18n("Profile not active"),
                                 i18n("Use this profile first, then open configuration again to connect it."));
        return;
    }
    m_graphConnectionPending = true;
    m_graphStatusLabel->setText(i18n("Connecting…"));
    m_controller->beginGraphLogin(m_clientIdEdit->text().trimmed());
}

void ProfileDialog::openVerificationPage()
{
    const QUrl url(m_controller->graphAuthorizationUrl());
    if (url.isValid()) {
        QDesktopServices::openUrl(url);
    }
}

void ProfileDialog::updateGraphStatus()
{
    const bool graph = m_backendCombo->currentData().toString() == QLatin1String("graph");
    m_connectButton->setEnabled(graph);
    m_openVerificationButton->setEnabled(graph && !m_controller->graphAuthorizationUrl().isEmpty());
    m_completeButton->setEnabled(graph && !m_controller->graphAuthorizationUrl().isEmpty());
    m_graphStatusLabel->setText(graph
                                    ? m_controller->graphAuthenticated() ? i18n("Connected")
                                        : !m_controller->graphErrorMessage().isEmpty()
                                            ? m_controller->graphErrorMessage()
                                            : m_graphConnectionPending ? i18n("Connecting…")
                                                                        : i18n("Not connected")
                                    : i18n("Not applicable"));
    if (m_controller->graphAuthenticated() || !m_controller->graphErrorMessage().isEmpty()) {
        m_graphConnectionPending = false;
    }
    m_verificationLabel->setText(m_controller->graphAuthorizationUrl().isEmpty()
                                     ? QString()
                                     : i18n("Open the sign-in page, authorize DriveBeacon, then paste the final redirect URL above."));
}

void ProfileDialog::refreshRemoteFolders()
{
    if (!m_controller->graphAuthenticated()) {
        return;
    }
    m_controller->refreshGraphFolders();
    populateRemoteFolders();
}

void ProfileDialog::populateRemoteFolders()
{
    if (!m_controller->graphAuthenticated()) {
        return;
    }
    m_loadingFolders = true;
    const QStringList included = m_store->load(m_nameEdit->text().trimmed()).includedFolders;
    const QStringList excluded = m_store->load(m_nameEdit->text().trimmed()).excludedFolders;
    m_folderTree->clear();
    for (const QString &folder : m_controller->graphRemoteFolders()) {
        auto *item = new QTreeWidgetItem(m_folderTree, {folder});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(1, included.contains(folder) ? Qt::Checked : Qt::Unchecked);
        item->setCheckState(2, excluded.contains(folder) ? Qt::Checked : Qt::Unchecked);
    }
    m_loadingFolders = false;
}

void ProfileDialog::updateFolderSelection(QTreeWidgetItem *item, int column)
{
    if (m_loadingFolders || item == nullptr || (column != 1 && column != 2)) {
        return;
    }
    const QSignalBlocker blocker(m_folderTree);
    if (item->checkState(column) == Qt::Checked) {
        item->setCheckState(column == 1 ? 2 : 1, Qt::Unchecked);
    }
}

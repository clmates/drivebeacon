// SPDX-License-Identifier: GPL-3.0-only

#include "profiledialog.h"

#include "onedrivecontroller.h"
#include "profilestore.h"
#include "drivebeaconserviceclient.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
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

namespace {

// This public desktop client registration is the packaged default. It contains
// no user credential; each account still authorizes independently in Graph.
constexpr auto kPackagedGraphClientId = "bf5e2104-7841-4c6a-aad4-a90fbe4dd3a7";

} // namespace

ProfileDialog::ProfileDialog(ProfileStore *store, OneDriveController *controller,
                             DriveBeaconServiceClient *serviceClient, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_controller(controller)
    , m_serviceClient(serviceClient)
    , m_profileList(new QListWidget(this))
    , m_nameEdit(new QLineEdit(this))
    , m_backendCombo(new QComboBox(this))
    , m_directoryEdit(new QLineEdit(this))
    , m_syncEnabledCheck(new QCheckBox(i18n("Synchronize this account"), this))
    , m_globalSyncEnabledCheck(new QCheckBox(i18n("Synchronize all accounts"), this))
    , m_folderTree(new QTreeWidget(this))
    , m_refreshFoldersButton(new QPushButton(i18n("Refresh folders"), this))
    , m_remoteIntervalSpin(new QSpinBox(this))
    , m_concurrentDownloadsSpin(new QSpinBox(this))
    , m_concurrentUploadsSpin(new QSpinBox(this))
    , m_concurrentLargeTransfersSpin(new QSpinBox(this))
    , m_cacheEvictionDaysSpin(new QSpinBox(this))
    , m_globalCacheFreeSpin(new QSpinBox(this))
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

    // Authentication failures often need to be copied into a bug report or
    // journal investigation. QLabel is otherwise not selectable, so expose
    // the status text without adding a second, transient error dialog.
    m_graphStatusLabel->setWordWrap(true);
    m_graphStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse
                                                 | Qt::TextSelectableByKeyboard);
    m_graphStatusLabel->setToolTip(i18n("Select this message and copy it for diagnostics."));

    m_backendCombo->addItem(i18n("abraunegg journal"), QStringLiteral("abraunegg-journal"));
    m_backendCombo->addItem(i18n("Microsoft Graph"), QStringLiteral("graph"));
    m_folderTree->setHeaderLabels({i18n("Remote folder"), i18n("Sync"), i18n("Exclude"),
                                   i18n("Folder policy")});
    m_folderTree->setRootIsDecorated(false);
    m_folderTree->setAlternatingRowColors(true);
    m_folderTree->setMinimumHeight(150);
    m_remoteIntervalSpin->setRange(10, 3600);
    m_remoteIntervalSpin->setSuffix(i18n(" s"));
    m_concurrentDownloadsSpin->setRange(1, 8);
    m_concurrentUploadsSpin->setRange(1, 8);
    m_concurrentLargeTransfersSpin->setRange(1, 4);
    m_cacheEvictionDaysSpin->setRange(0, 3650);
    m_cacheEvictionDaysSpin->setSuffix(i18n(" days"));
    m_cacheEvictionDaysSpin->setSpecialValueText(i18n("Disabled"));
    m_globalCacheFreeSpin->setRange(0, 1024 * 1024);
    m_globalCacheFreeSpin->setSuffix(i18n(" MB free"));
    m_globalCacheFreeSpin->setSpecialValueText(i18n("Disabled"));
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
    auto *deleteButton = new QPushButton(i18n("Delete profile"), this);
    profileButtons->addWidget(newButton);
    profileButtons->addWidget(loadButton);
    profileButtons->addWidget(saveButton);
    profileButtons->addWidget(deleteButton);

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
    form->addRow(i18n("FUSE mount directory:"), directoryRow);
    form->addRow(i18n("Account state:"), m_syncEnabledCheck);
    form->addRow(i18n("Global state:"), m_globalSyncEnabledCheck);
    form->addRow(i18n("Remote check interval:"), m_remoteIntervalSpin);
    form->addRow(i18n("Simultaneous downloads:"), m_concurrentDownloadsSpin);
    form->addRow(i18n("Simultaneous uploads:"), m_concurrentUploadsSpin);
    form->addRow(i18n("Simultaneous large transfers:"), m_concurrentLargeTransfersSpin);
    form->addRow(i18n("Purge after unused days:"), m_cacheEvictionDaysSpin);
    form->addRow(i18n("Global minimum free cache space:"), m_globalCacheFreeSpin);
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
    connect(deleteButton, &QPushButton::clicked, this, &ProfileDialog::deleteProfile);
    connect(browseButton, &QPushButton::clicked, this, [this] {
    const QString directory = QFileDialog::getExistingDirectory(
        this, i18n("Select FUSE mount directory"), m_directoryEdit->text());
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
        const QString profileName = m_nameEdit->text().trimmed();
        if (profileName == m_controller->profileName()) {
            m_controller->completeGraphLogin(m_responseUrlEdit->text());
        } else {
            m_serviceClient->completeGraphLogin(profileName, m_responseUrlEdit->text());
        }
    });
    connect(m_controller, &OneDriveController::graphAuthChanged,
            this, &ProfileDialog::updateGraphStatus);
    connect(m_controller, &OneDriveController::graphRemoteFoldersChanged,
            this, &ProfileDialog::populateRemoteFolders);
    connect(m_serviceClient, &DriveBeaconServiceClient::graphLoginStarted, this,
            [this](const QString &profileName, const QString &authorizationUrl,
                   const QString &errorMessage) {
                if (profileName != m_nameEdit->text().trimmed()) {
                    return;
                }
                m_remoteAuthorizationUrl = authorizationUrl;
                m_remoteGraphError = errorMessage;
                if (!errorMessage.isEmpty()) {
                    m_graphConnectionPending = false;
                }
                updateGraphStatus();
            });
    connect(m_serviceClient, &DriveBeaconServiceClient::graphAuthStateChanged, this,
            [this](const QString &profileName, bool authenticated,
                   const QString &errorMessage, const QString &authorizationUrl) {
                if (profileName != m_nameEdit->text().trimmed()) {
                    return;
                }
                m_remoteAuthenticated = authenticated;
                m_remoteGraphError = errorMessage;
                m_remoteAuthorizationUrl = authorizationUrl;
                if (authenticated || !errorMessage.isEmpty()) {
                    m_graphConnectionPending = false;
                }
                updateGraphStatus();
                if (authenticated) {
                    // The profile-status cache may lag behind this auth
                    // signal, so request the folder listing directly instead
                    // of filtering it through the old cached status.
                    m_folderTree->clear();
                    m_serviceClient->refreshGraphFolders(profileName);
                }
            });
    connect(m_serviceClient, &DriveBeaconServiceClient::graphRemoteFoldersChanged, this,
            &ProfileDialog::updateSelectedServiceFolders);
    connect(m_serviceClient, &DriveBeaconServiceClient::profilesChanged, this,
            [this] {
                updateSelectedServiceProfileState();
            });
    connect(m_serviceClient, &DriveBeaconServiceClient::profileListChanged, this,
            [this] {
                // Only account additions/removals rebuild the profile list.
                // Status and transfer updates must not reload the folder tree.
                refreshProfileList(m_nameEdit->text().trimmed());
            });
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
    m_directoryEdit->setText(profile.mountDirectory);
    m_syncEnabledCheck->setChecked(profile.syncEnabled);
    m_globalSyncEnabledCheck->setChecked(m_store->globalSyncEnabled());
    m_remoteIntervalSpin->setValue(profile.remoteCheckIntervalSeconds);
    m_concurrentDownloadsSpin->setValue(profile.concurrentDownloads);
    m_concurrentUploadsSpin->setValue(profile.concurrentUploads);
    m_concurrentLargeTransfersSpin->setValue(profile.concurrentLargeTransfers);
    m_cacheEvictionDaysSpin->setValue(profile.cacheEvictionDays);
    m_globalCacheFreeSpin->setValue(static_cast<int>(m_store->cacheMinimumFreeBytes()
                                                      / (1024 * 1024)));
    m_clientIdEdit->setText(profile.graphClientId.isEmpty()
                                ? QString::fromLatin1(kPackagedGraphClientId)
                                : profile.graphClientId);
    m_driveIdEdit->setText(profile.remoteDriveId);
    m_remoteFolders.clear();
    m_displayedRemoteFolders.clear();
    updateSelectedServiceProfileState();
    updateGraphStatus();
    refreshRemoteFolders();
}

void ProfileDialog::createProfile()
{
    m_nameEdit->setText(QStringLiteral("graph"));
    m_backendCombo->setCurrentIndex(m_backendCombo->findData(QStringLiteral("graph")));
    m_directoryEdit->setText(QDir::home().filePath(QStringLiteral("Onedrive-Graph")));
    // New profiles must be configured before any Graph enumeration starts.
    // The user can authenticate, refresh the remote folders, and save the
    // selection while the profile remains paused; resuming is an explicit
    // final step.
    m_syncEnabledCheck->setChecked(false);
    m_globalSyncEnabledCheck->setChecked(m_store->globalSyncEnabled());
    m_concurrentDownloadsSpin->setValue(2);
    m_concurrentUploadsSpin->setValue(2);
    m_concurrentLargeTransfersSpin->setValue(1);
    m_cacheEvictionDaysSpin->setValue(0);
    m_globalCacheFreeSpin->setValue(static_cast<int>(m_store->cacheMinimumFreeBytes()
                                                      / (1024 * 1024)));
    m_folderTree->clear();
    m_clientIdEdit->setText(QString::fromLatin1(kPackagedGraphClientId));
    m_driveIdEdit->clear();
    m_profileList->clearSelection();
    updateGraphStatus();
}

void ProfileDialog::saveProfile()
{
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty() || m_directoryEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, i18n("Invalid profile"),
                             i18n("A profile name and FUSE mount directory are required."));
        return;
    }

    const bool isNewProfile = !m_store->profileNames().contains(name);

    // A deleted profile may have left its private cache intentionally. Never
    // reuse that state silently for a newly created profile with the same key.
    if (!m_store->profileNames().contains(name)
        && QDir(ProfileStore::cacheDirectory(name)).exists()) {
        QMessageBox cacheWarning(this);
        cacheWarning.setWindowTitle(i18n("Existing local cache"));
        cacheWarning.setText(i18n("A local cache already exists for '%1'.", name));
        cacheWarning.setInformativeText(i18n(
            "You can reuse it, or delete it before creating the profile. Remote files "
            "will not be changed by either choice."));
        auto *useCache = cacheWarning.addButton(i18n("Use existing cache"),
                                                QMessageBox::AcceptRole);
        auto *deleteCache = cacheWarning.addButton(i18n("Delete cache"),
                                                   QMessageBox::DestructiveRole);
        cacheWarning.addButton(QMessageBox::Cancel);
        cacheWarning.exec();
        if (cacheWarning.clickedButton() == deleteCache) {
            QDir(ProfileStore::cacheDirectory(name)).removeRecursively();
        } else if (cacheWarning.clickedButton() != useCache) {
            return;
        }
    }

    // Update the existing profile instead of rebuilding it. In particular,
    // keep Graph's delta cursor and both baselines when only UI settings
    // (limits or folder policy) change.
    SyncProfile profile = m_store->load(name);
    profile.name = name;
    profile.backend = syncBackendFromName(m_backendCombo->currentData().toString());
    profile.mountDirectory = QDir::cleanPath(
        QFileInfo(m_directoryEdit->text().trimmed()).absoluteFilePath());
    // Keep the legacy field aligned for older clients; the service ignores it
    // as a backing directory and derives a private cache from the profile key.
    profile.localDirectory = profile.mountDirectory;
    profile.availability = LocalAvailability::OnDemand;
    // Keep a newly created profile paused even if a caller changed the
    // checkbox after the New template was shown. This prevents authentication
    // or folder discovery from starting a first enumeration before the
    // include/exclude policy has been saved.
    profile.syncEnabled = !isNewProfile && m_syncEnabledCheck->isChecked();
    profile.remoteCheckIntervalSeconds = m_remoteIntervalSpin->value();
    profile.concurrentDownloads = m_concurrentDownloadsSpin->value();
    profile.concurrentUploads = m_concurrentUploadsSpin->value();
    profile.concurrentLargeTransfers = m_concurrentLargeTransfersSpin->value();
    profile.cacheEvictionDays = m_cacheEvictionDaysSpin->value();
    // Rebuild folder policy from the tree instead of appending to the values
    // loaded above; repeated saves must remain idempotent.
    profile.includedFolders.clear();
    profile.excludedFolders.clear();
    profile.graphPathPolicies.clear();
    for (int row = 0; row < m_folderTree->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_folderTree->topLevelItem(row);
        if (item->checkState(1) == Qt::Checked) {
            profile.includedFolders.append(item->text(0));
        }
        if (item->checkState(2) == Qt::Checked) {
            profile.excludedFolders.append(item->text(0));
        }
        if (auto *policy = qobject_cast<QComboBox *>(m_folderTree->itemWidget(item, 3))) {
            const QString value = policy->currentData().toString();
            if (!value.isEmpty() && value != QLatin1String("inherit")) {
                profile.graphPathPolicies.append(item->text(0) + QLatin1Char('\t') + value);
            }
        }
    }
    profile.graphClientId = m_clientIdEdit->text().trimmed();
    profile.remoteDriveId = m_driveIdEdit->text().trimmed();
    // Path-level controls will be populated by the policy editor; preserving
    // this list here keeps a save of unrelated settings non-destructive.
    m_store->save(profile);
    m_store->setGlobalSyncEnabled(m_globalSyncEnabledCheck->isChecked());
    m_store->setCacheMinimumFreeBytes(static_cast<qint64>(m_globalCacheFreeSpin->value())
                                      * 1024 * 1024);
    Q_EMIT profileSaved(name);
    refreshProfileList(name);
}

void ProfileDialog::deleteProfile()
{
    const QListWidgetItem *item = m_profileList->currentItem();
    if (!item) {
        return;
    }
    const QString name = item->text();
    QMessageBox confirm(this);
    confirm.setWindowTitle(i18n("Delete profile"));
    confirm.setText(i18n("Delete the local profile '%1'?", name));
    confirm.setInformativeText(i18n(
        "The FUSE view will be unmounted. Microsoft Graph files will not be changed. "
        "Choose whether to remove the private local cache too."));
    auto *deleteCache = confirm.addButton(i18n("Delete profile and cache"),
                                          QMessageBox::DestructiveRole);
    auto *keepCache = confirm.addButton(i18n("Delete profile, keep cache"),
                                        QMessageBox::AcceptRole);
    confirm.addButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != deleteCache && confirm.clickedButton() != keepCache) {
        return;
    }
    Q_EMIT profileDeleteRequested(name, confirm.clickedButton() == deleteCache);
    // Keep configuration open after deletion. Remove the row immediately for
    // responsive feedback; the service's profile reload will reconcile the
    // list with persistent settings and select the first remaining profile.
    const int row = m_profileList->row(item);
    delete m_profileList->takeItem(row);
    if (m_profileList->count() > 0) {
        m_profileList->setCurrentRow(0);
    } else {
        createProfile();
    }
}

void ProfileDialog::connectGraph()
{
    saveProfile();
    if (syncBackendFromName(m_backendCombo->currentData().toString()) != SyncBackend::MicrosoftGraph) {
        return;
    }
    m_graphConnectionPending = true;
    m_graphStatusLabel->setText(i18n("Connecting…"));
    const QString profileName = m_nameEdit->text().trimmed();
    if (profileName == m_controller->profileName()) {
        m_controller->beginGraphLogin(m_clientIdEdit->text().trimmed());
    } else {
        // Authentication belongs to the selected account, not to the tray's
        // compatibility controller. The service keeps every profile's token
        // store isolated while the tray remains responsive and open.
        m_serviceClient->beginGraphLogin(profileName, m_clientIdEdit->text().trimmed());
    }
}

void ProfileDialog::openVerificationPage()
{
    const QString authorizationUrl = m_nameEdit->text().trimmed() == m_controller->profileName()
        ? m_controller->graphAuthorizationUrl() : m_remoteAuthorizationUrl;
    const QUrl url(authorizationUrl);
    if (url.isValid()) {
        QDesktopServices::openUrl(url);
    }
}

void ProfileDialog::updateGraphStatus()
{
    const bool graph = m_backendCombo->currentData().toString() == QLatin1String("graph");
    const bool activeProfile = m_nameEdit->text().trimmed() == m_controller->profileName();
    const bool authenticated = activeProfile ? m_controller->graphAuthenticated()
                                             : m_remoteAuthenticated;
    const QString error = activeProfile ? m_controller->graphErrorMessage()
                                        : m_remoteGraphError;
    const QString authorizationUrl = activeProfile ? m_controller->graphAuthorizationUrl()
                                                   : m_remoteAuthorizationUrl;
    m_connectButton->setEnabled(graph);
    m_openVerificationButton->setEnabled(graph && !authorizationUrl.isEmpty());
    m_completeButton->setEnabled(graph && !authorizationUrl.isEmpty());
    m_graphStatusLabel->setText(graph
                                    ? authenticated ? i18n("Connected")
                                        : !error.isEmpty() ? error
                                            : m_graphConnectionPending ? i18n("Connecting…")
                                                                        : i18n("Not connected")
                                    : i18n("Not applicable"));
    if (authenticated || !error.isEmpty()) {
        m_graphConnectionPending = false;
    }
    m_verificationLabel->setText(authorizationUrl.isEmpty()
                                     ? QString()
                                     : i18n("Open the sign-in page, authorize DriveBeacon, then paste the final redirect URL above."));
}

void ProfileDialog::updateSelectedServiceProfileState()
{
    if (!m_serviceClient) {
        return;
    }
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty() || name == m_controller->profileName()) {
        return;
    }
    const QVariantMap status = m_serviceClient->profileStatus(name);
    m_remoteAuthenticated = status.value(QStringLiteral("authenticated")).toBool();
    m_remoteGraphError = status.value(QStringLiteral("error")).toString();
    updateGraphStatus();
}

void ProfileDialog::refreshRemoteFolders()
{
    const QString profileName = m_nameEdit->text().trimmed();
    // The service owns Graph profiles, including paused ones. Folder discovery
    // is configuration metadata and must remain available independently of
    // the profile's synchronization switch; using the tray compatibility
    // controller here loses the result when both names happen to match.
    if (m_serviceClient && m_serviceClient->available()
        && m_serviceClient->graphProfiles().contains(profileName)) {
        const QVariantMap status = m_serviceClient->profileStatus(profileName);
        if (!status.value(QStringLiteral("authenticated")).toBool()) {
            return;
        }
        m_folderTree->clear();
        m_serviceClient->refreshGraphFolders(profileName);
        return;
    }
    if (profileName == m_controller->profileName()) {
        if (!m_controller->graphAuthenticated()) {
            return;
        }
        m_controller->refreshGraphFolders();
        populateRemoteFolders();
        return;
    }
    const QVariantMap status = m_serviceClient->profileStatus(profileName);
    if (!status.value(QStringLiteral("authenticated")).toBool()) {
        return;
    }
    m_folderTree->clear();
    m_serviceClient->refreshGraphFolders(profileName);
}

void ProfileDialog::populateRemoteFolders()
{
    const bool activeProfile = m_nameEdit->text().trimmed() == m_controller->profileName();
    if ((activeProfile && !m_controller->graphAuthenticated())
        || (!activeProfile && !m_remoteAuthenticated)) {
        return;
    }
    m_loadingFolders = true;
    const QStringList included = m_store->load(m_nameEdit->text().trimmed()).includedFolders;
    const QStringList excluded = m_store->load(m_nameEdit->text().trimmed()).excludedFolders;
    const QStringList pathPolicies = m_store->load(m_nameEdit->text().trimmed()).graphPathPolicies;
    const QStringList folders = m_nameEdit->text().trimmed() == m_controller->profileName()
        ? m_controller->graphRemoteFolders() : m_remoteFolders;
    if (folders == m_displayedRemoteFolders
        && m_folderTree->topLevelItemCount() == folders.size()) {
        return;
    }
    QHash<QString, QPair<Qt::CheckState, Qt::CheckState>> currentStates;
    QHash<QString, QString> currentPolicies;
    for (int index = 0; index < m_folderTree->topLevelItemCount(); ++index) {
        auto *item = m_folderTree->topLevelItem(index);
        currentStates.insert(item->text(0), {item->checkState(1), item->checkState(2)});
        if (auto *policy = qobject_cast<QComboBox *>(m_folderTree->itemWidget(item, 3))) {
            currentPolicies.insert(item->text(0), policy->currentData().toString());
        }
    }
    m_folderTree->clear();
    for (const QString &folder : folders) {
        auto *item = new QTreeWidgetItem(m_folderTree, {folder});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const auto current = currentStates.constFind(folder);
        item->setCheckState(1, current == currentStates.cend()
                                  ? (included.contains(folder) ? Qt::Checked : Qt::Unchecked)
                                  : current->first);
        item->setCheckState(2, current == currentStates.cend()
                                  ? (excluded.contains(folder) ? Qt::Checked : Qt::Unchecked)
                                  : current->second);
        auto *policy = new QComboBox(m_folderTree);
        policy->addItem(i18n("Inherit default"), QStringLiteral("inherit"));
        policy->addItem(i18n("Keep local"), QStringLiteral("keep-local"));
        policy->addItem(i18n("On demand"), QStringLiteral("on-demand"));
        policy->addItem(i18n("Release local cache"), QStringLiteral("remote-only"));
        const QString prefix = folder + QLatin1Char('\t');
        for (const QString &record : pathPolicies) {
            if (record.startsWith(prefix)) {
                policy->setCurrentIndex(policy->findData(record.sliced(prefix.size())));
                break;
            }
        }
        if (currentPolicies.contains(folder)) {
            policy->setCurrentIndex(policy->findData(currentPolicies.value(folder)));
        }
        m_folderTree->setItemWidget(item, 3, policy);
    }
    m_displayedRemoteFolders = folders;
    m_loadingFolders = false;
}

void ProfileDialog::updateSelectedServiceFolders(const QString &profileName,
                                                 const QStringList &folders)
{
    if (profileName != m_nameEdit->text().trimmed()) {
        return;
    }
    m_remoteFolders = folders;
    populateRemoteFolders();
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

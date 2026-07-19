// SPDX-License-Identifier: GPL-3.0-only

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root

    width: 760
    height: 520
    minimumWidth: 520
    minimumHeight: 360
    visible: true
    title: i18n("DriveBeacon")

    menuBar: Controls.MenuBar {
        Controls.Menu {
            title: i18n("Help")

            Controls.MenuItem {
                text: i18n("About DriveBeacon")
                onTriggered: root.pageStack.push(aboutPageComponent)
            }
        }
    }

    Component {
        id: aboutPageComponent

        Kirigami.AboutPage {
            aboutData: applicationAboutData

            Controls.ToolButton {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: Kirigami.Units.smallSpacing
                z: 1
                display: Controls.AbstractButton.IconOnly
                icon.name: "dialog-close"
                text: i18n("Close About page")
                onClicked: root.pageStack.pop()

                Controls.ToolTip.visible: hovered
                Controls.ToolTip.text: text
            }
        }
    }

    onClosing: close => {
        close.accepted = false
        root.hide()
    }

    pageStack.initialPage: Kirigami.Page {
        title: i18n("Synchronization status")

        ColumnLayout {
            anchors.fill: parent
            spacing: Kirigami.Units.largeSpacing

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: controller.errorMessage.length > 0
                type: Kirigami.MessageType.Error
                text: controller.errorMessage
                showCloseButton: true
                onVisibleChanged: {
                    if (!visible)
                        controller.clearError()
                }
            }

            RowLayout {
                Layout.fillWidth: true

                Kirigami.Icon {
                    source: controller.activeState === "active"
                            ? "emblem-synchronized"
                            : controller.activeState === "failed"
                              ? "data-error"
                              : "media-playback-pause"
                    implicitWidth: Kirigami.Units.iconSizes.large
                    implicitHeight: implicitWidth
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Kirigami.Heading {
                        level: 2
                        text: controller.statusText
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: i18n("Local folder: %1", controller.syncDirectory)
                        elide: Text.ElideMiddle
                        color: Kirigami.Theme.disabledTextColor
                    }
                }

                Controls.Button {
                    text: controller.activeState === "active" ? i18n("Stop") : i18n("Start")
                    icon.name: controller.activeState === "active"
                               ? "media-playback-stop"
                               : "media-playback-start"
                    enabled: controller.activeState !== "activating"
                             && controller.activeState !== "deactivating"
                    onClicked: controller.activeState === "active"
                               ? controller.stopService()
                               : controller.startService()
                }
                Controls.Button {
                    text: i18n("Restart")
                    icon.name: "view-refresh"
                    enabled: controller.activeState === "active"
                    onClicked: controller.restartService()
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 2
                    text: i18n("Recent activity")
                }
                Controls.ToolButton {
                    text: i18n("Clear")
                    icon.name: "edit-clear-history"
                    onClicked: controller.activities.clear()
                }
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: activityView.count === 0
                text: i18n("No recent file activity was found in the journal.")
                horizontalAlignment: Text.AlignHCenter
                color: Kirigami.Theme.disabledTextColor
            }

            ListView {
                id: activityView

                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: contentHeight
                clip: true
                model: controller.activities
                spacing: Kirigami.Units.smallSpacing

                delegate: Controls.ItemDelegate {
                    id: activityDelegate

                    required property date timestamp
                    required property string operation
                    required property string path
                    required property string destinationPath
                    required property bool completed

                    width: ListView.view.width
                    icon.name: operation === "download" ? "download"
                               : operation === "upload" ? "upload"
                               : operation === "move" ? "go-jump"
                               : "edit-delete"
                    text: destinationPath.length > 0
                          ? i18n("%1 → %2", path, destinationPath)
                          : path
                    onClicked: controller.openActivityPath(destinationPath.length > 0
                                                           ? destinationPath : path)

                    contentItem: RowLayout {
                        Kirigami.Icon {
                            source: activityDelegate.icon.name
                            implicitWidth: Kirigami.Units.iconSizes.medium
                            implicitHeight: implicitWidth
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Controls.Label {
                                Layout.fillWidth: true
                                text: activityDelegate.text
                                elide: Text.ElideMiddle
                            }
                            Controls.Label {
                                text: (activityDelegate.operation === "download"
                                       ? i18n("Downloaded")
                                       : activityDelegate.operation === "upload"
                                         ? i18n("Uploaded")
                                         : activityDelegate.operation === "move"
                                           ? i18n("Moved")
                                           : activityDelegate.completed
                                             ? i18n("Deleted") : i18n("Deleting"))
                                      + " · " + Qt.formatDateTime(activityDelegate.timestamp, Qt.DefaultLocaleShortDate)
                                color: Kirigami.Theme.disabledTextColor
                                font: Kirigami.Theme.smallFont
                            }
                        }
                    }
                }
            }
        }
    }
}

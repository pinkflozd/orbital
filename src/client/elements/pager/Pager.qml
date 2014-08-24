/*
 * Copyright 2013 Giulio Camuffo <giuliocamuffo@gmail.com>
 *
 * This file is part of Orbital
 *
 * Orbital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Orbital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Orbital.  If not, see <http://www.gnu.org/licenses/>.
 */

import QtQuick 2.1
import QtQuick.Window 2.1
import Orbital 1.0

Element {
    id: pager

    property bool horizontal: location == 0 || location == 2 || location == 4
    readonly property real ratio: Screen.height > 0 ? Screen.width / Screen.height : 1

    Layout.preferredWidth: horizontal ? _cols * height * ratio / _rows : 50
    Layout.preferredHeight: horizontal ? 50 / ratio : _rows * width / ratio / _cols

    width: Layout.preferredWidth
    height: 50
    property int _rows: 1
    property int _cols: 1

    onHorizontalChanged: {
        if (horizontal) {
            _rows = Qt.binding(function() { return Client.workspaces.length > 2 ? 2 : 1 });
            _cols = Qt.binding(function() { return Client.workspaces.length > 0 ? Client.workspaces.length / _rows : 1 });
        } else {
            _rows = Qt.binding(function() { return Client.workspaces.length > 0 ? Client.workspaces.length / _cols : 1 });
            _cols = Qt.binding(function() { return Client.workspaces.length > 2 ? 2 : 1 });
        }
    }

    contentItem: StyleItem {
        component: CurrentStyle.pagerBackground
        anchors.fill: parent
        Grid {
            width: parent.width
            height: parent.height
            id: grid

            rows: _rows
            readonly property bool fitWidth: width / _cols * _rows >= height * pager.ratio

            property int itemH: fitWidth ? height / rows : (width / _cols) / pager.ratio
            property int itemW: itemH * pager.ratio

            x: (width - itemW * _cols) / 2
            y: (height - itemH * rows) / 2

            Repeater {
                model: Client.workspaces

                Item {
                    height: grid.itemH
                    width: grid.itemW

                    Connections {
                        target: modelData
                        onActiveChanged: updateActive()
                    }

                    function updateActive() {
                        si.item.active = modelData.isActiveForScreen(pager.screen)
                    }

                    StyleItem {
                        id: si
                        anchors.fill: parent
                        component: CurrentStyle.pagerWorkspace
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: Client.selectWorkspace(pager.screen, modelData)
                    }

                    Component.onCompleted: updateActive()
                }
            }
        }
    }
}

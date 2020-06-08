import QtQuick 2.0
import Sailfish.Silica 1.0
import "../pages/utils.js" as Utils

Dialog {
    id: dialog

    property string name
    property string choice
    property string new_choice: choice
    property var choices
    canAccept: false

    SilicaListView
    {
        anchors.fill: parent

        header: DialogHeader {}

        model: choices
        delegate: BackgroundItem {
            onClicked: {
                new_choice=choices[index]
                dialog.canAccept = true
            }
            Label {
                x: Theme.paddingLarge
                anchors.verticalCenter: parent.verticalCenter
                highlighted: choices[index]==new_choice
                text: Utils.ippName(name, choices[index])
            }
        }
    }

    onDone: {
        if (result == DialogResult.Accepted) {
            choice = new_choice
        }
    }
}

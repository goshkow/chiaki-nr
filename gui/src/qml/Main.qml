import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

Item {
    id: root
    property list<Item> restoreFocusItems
    property bool initialAsk: false
    readonly property bool preferSeparateStreamSettingsWindows: Chiaki.window.runtimeRendererBackend === 1 && Chiaki.session
    property bool useSeparateStreamSettingsWindows: false
    Material.theme: Material.Dark
    Material.accent: "#00a7ff"
    Popup {
        id: nvidiaMenu
        objectName: "nvidiaMenu"
        x: 24; y: 32
        width: Math.max(1, Math.min(540, root.width - 16))
        height: Math.max(1, Math.min(850, root.height - 16))
        padding: 18
        modal: true; focus: true
        closePolicy: Popup.CloseOnEscape
        function constrain() { x=Math.max(8,Math.min(x,root.width-width-8)); y=Math.max(8,Math.min(y,root.height-height-8)); }
        onWidthChanged: constrain()
        onHeightChanged: constrain()
        onAboutToShow: { constrain(); root.grabInput(contentItem); Chiaki.window.requestOverlayUpdate(); }
        onClosed: { root.releaseInput(); Chiaki.window.requestOverlayUpdate(); }
        background: Rectangle { color: "#ee17222d"; radius: 18; border.width: 1; border.color: "#425d72" }
        contentItem: ColumnLayout {
            spacing: 12
            Item {
                Layout.fillWidth: true; Layout.preferredHeight: 50
                Column {
                    Label { text: "Обработка видео"; font.pixelSize: 24; font.bold: true; color: "#eff8ff" }
                    Label { text: "Перетащите заголовок · Insert — закрыть"; font.pixelSize: 12; color: "#9aafc1" }
                }
                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.SizeAllCursor
                    property point origin
                    onPressed: mouse => { origin=mapToItem(root,mouse.x,mouse.y); }
                    onPositionChanged: mouse => { if(pressed){ const next=mapToItem(root,mouse.x,mouse.y); nvidiaMenu.x+=next.x-origin.x; nvidiaMenu.y+=next.y-origin.y; nvidiaMenu.constrain(); origin=next; } }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: "#425d72" }
            ScrollView {
                id: videoControls
                objectName: "videoControls"
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: videoControls.availableWidth
                    spacing: 10
                    Label { text: "Схема выбранного режима"; font.bold: true; color: "#81d8ff" }
                    Repeater {
                        model: ["PS5 → декодирование GPU · исходное разрешение", "Оценка движения + нейтральная глубина", Chiaki.window.nvidia.neural && Chiaki.window.nvidia.neuralBefore ? "Модель NR получает исходное разрешение; поправка сохранена" : "Оригинал сохранён", ["Обычное масштабирование", "Spatial", "Advanced Spatial"][Chiaki.window.upscalingMode] + " → разрешение области вывода", Chiaki.window.nvidia.enabled ? "Резкость NVIDIA NIS (если включена)" : "Резкость AMD RCAS (если включена)", Chiaki.window.nvidia.neural ? (Chiaki.window.nvidia.neuralBefore ? "Поправка ранее вычисленного NR → полная картинка" : "Модель NR получает апскейл + резкость → поправка к полной картинке") : "Neural Rendering выключен", Chiaki.window.nvidia.frameGeneration ? "Optical Flow → промежуточный → исходный кадр, пауза 1–8,3 мс" : "Генерация кадров выключена", "Интерфейс → экран"]
                        delegate: Rectangle {
                            required property string modelData
                            Layout.fillWidth: true; implicitHeight: stageLabel.implicitHeight+16
                            radius: 8; color: "#243544"
                            Label { id: stageLabel; anchors.fill: parent; anchors.margins: 8; text: modelData; wrapMode: Text.WordWrap; font.pixelSize: 13 }
                        }
                    }
                    Label { text: "Render Preset"; font.bold: true }
                    ComboBox {
                        model: ["Fast", "Default", "High Quality", "Custom"]
                        currentIndex: Chiaki.window.videoPreset===ChiakiWindow.VideoPreset.Custom?3:Number(Chiaki.window.videoPreset)
                        onActivated: index => { const value=index===3?5:index; Chiaki.settings.videoPreset=value; Chiaki.window.videoPreset=value; }
                        Layout.fillWidth: true
                    }
                    Label { text: "Апскейлер · исходный поток → размер окна"; font.bold: true }
                    Label { text: "Исходный поток масштабируется до размера окна или экрана выбранным пространственным алгоритмом."; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9aafc1"; font.pixelSize: 12 }
                    ComboBox { model: ["Обычное масштабирование", "Spatial Upscaling", "Advanced Spatial Upscaling"]; currentIndex: Chiaki.window.upscalingMode; onActivated: index => Chiaki.window.upscalingMode=index; Layout.fillWidth: true }
                    Switch { enabled: Chiaki.window.nvidia.neuralAvailable; text: "DLSS 5 Neural Rendering"; checked: Chiaki.window.nvidia.neural; onToggled: Chiaki.window.nvidia.neural=checked }
                    Label { text: Chiaki.window.nvidia.neuralAvailabilityReason; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 12; color: "#9aafc1" }
                    Label { text: "Смешивание результата NR: " + Math.round(Chiaki.window.nvidia.neuralBlend*100) + "%" }
                    Slider { from: 0; to: 1; stepSize: 0.05; value: Chiaki.window.nvidia.neuralBlend; enabled: Chiaki.window.nvidia.neural; onMoved: Chiaki.window.nvidia.neuralBlend=value; Layout.fillWidth: true }
                    Label { text: "NR накладывается после апскейлера и резкости. Последнее изменение заменяет ожидающие параметры; ползунки не ждут GPU. Рабочий размер и стиль требуют пересоздания только NR, интенсивность — нет."; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9aafc1"; font.pixelSize: 12 }
                    GroupBox {
                        id: nrGroup
                        objectName: "nrModelGroup"
                        title: "Параметры модели NR · во время стрима"
                        function applyNR() { nrSaved.text=Chiaki.window.nvidia.saveNeuralConfiguration(nrScale.value,nrPasses.currentIndex+1,nrStyle.currentIndex,nrIntensity.value,nrBefore.checked)?"Последние параметры сохранены; ожидающие заменены.":"Не удалось сохранить INI."; }
                        Timer { id: nrApplyDelay; interval: 300; onTriggered: nrGroup.applyNR() }
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: videoControls.availableWidth
                        ColumnLayout {
                            width: Math.max(0,nrGroup.availableWidth)
                            Label { text: "Рабочее разрешение: " + Math.round(nrScale.value*100) + "%" }
                            Slider { id: nrScale; from: .25; to: 1; stepSize: .025; value: Number(Chiaki.window.nvidia.neuralConfiguration.scale); onMoved: nrApplyDelay.restart(); onPressedChanged: if(!pressed) { nrApplyDelay.stop(); nrGroup.applyNR(); } Layout.fillWidth: true }
                            ComboBox { id: nrPasses; model: ["Один проход", "Два прохода"]; currentIndex: Number(Chiaki.window.nvidia.neuralConfiguration.passes)-1; onActivated: nrGroup.applyNR(); Layout.fillWidth: true }
                            ComboBox { id: nrStyle; model: ["Стандартный", "Естественный", "Кинематографический"]; currentIndex: Number(Chiaki.window.nvidia.neuralConfiguration.style); onActivated: nrGroup.applyNR(); Layout.fillWidth: true }
                            Label { text: "Интенсивность эффекта: " + nrIntensity.value.toFixed(2) }
                            Slider { id: nrIntensity; from: 0; to: 2; stepSize: .1; value: Number(Chiaki.window.nvidia.neuralConfiguration.intensity); onMoved: nrApplyDelay.restart(); onPressedChanged: if(!pressed) { nrApplyDelay.stop(); nrGroup.applyNR(); } Layout.fillWidth: true }
                            Switch { id: nrBefore; objectName: "nrBeforeSwitch"; Layout.fillWidth: true; Layout.minimumWidth: 0; contentItem: Text { text: nrBefore.text; color: "#eff8ff"; leftPadding: nrBefore.indicator.width + nrBefore.spacing; verticalAlignment: Text.AlignVCenter; wrapMode: Text.WordWrap } text: "Обрабатывать NR до апскейлера"; checked: Boolean(Chiaki.window.nvidia.neuralConfiguration.before); onToggled: nrGroup.applyNR() }
                            Button { text: "Применить параметры NR"; Layout.fillWidth: true; onClicked: nrGroup.applyNR() }
                            Label { id: nrSaved; text: "Рабочий масштаб меняет только размер обработки NR. Детали исходного потока не заменяются уменьшенной картинкой."; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 12; color: "#9aafc1" }
                        }
                    }
                    Label { visible: !Chiaki.window.nvidia.enabled || Chiaki.window.upscalingMode===6 || Chiaki.window.upscalingMode===8; text: "Резкость AMD RCAS: " + Math.round(Chiaki.window.videoSharpness*100) + "%" }
                    Slider { visible: !Chiaki.window.nvidia.enabled || Chiaki.window.upscalingMode===6 || Chiaki.window.upscalingMode===8; from: 0; to: 1; stepSize: .05; value: Chiaki.window.videoSharpness; onMoved: Chiaki.window.videoSharpness=value; Layout.fillWidth: true }
                    Label { visible: !Chiaki.window.nvidia.enabled || Chiaki.window.upscalingMode===6 || Chiaki.window.upscalingMode===8; text: "RCAS после масштабирования, до поправки NR. Изменения применяются к текущему кадру."; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 12; color: "#9aafc1" }
                    Label { visible: Chiaki.window.nvidia.neural; text: "Интенсивность регулирует силу поправки NR: 1 — обычная, 2 — усиленная. Это настройка наложения результата, а не гарантированно поддерживаемый библиотекой параметр модели."; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 12; color: "#9aafc1" }
                    Label { text: "Генерация кадров"; font.bold: true }
                    ComboBox { model: ["Выключено · минимальная задержка", "Видео · Optical Flow · 2× (экспериментально)"]; currentIndex: Chiaki.window.nvidia.frameGeneration?1:0; onActivated: index => Chiaki.window.nvidia.frameGeneration=index===1; Layout.fillWidth: true }
                    Label { text: Chiaki.window.nvidia.frameGenerationStatus; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#81d8ff" }
                    Label { text: "NVIDIA MFG / SM86 пока не подключён к выводу Chiaki. Интерполяция видео ожидает текущий кадр и сдвигает показ исходного примерно на пол-интервала; 2× не ускоряет ввод PS5."; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9aafc1"; font.pixelSize: 12 }
                    Label { text: Chiaki.window.nvidia.status; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: "Последняя обработка GPU + ожидание: " + Chiaki.window.nvidia.milliseconds.toFixed(2) + " мс" }
                    Label { text: {const t=Chiaki.window.nvidia.gpuTimings;return "GPU: масштабирование " + Number(t.sr||0).toFixed(2) + " · NR " + Number(t.nr||0).toFixed(2) + " · FG " + Number(t.fg||0).toFixed(2) + " · резкость " + Number(t.sharpness||0).toFixed(2) + " мс";} wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 12; color: "#81d8ff" }
                    Label { visible: Chiaki.window.nvidia.frameGeneration; text: "Пауза между фазами уменьшается при тяжёлой обработке NR. Если обработка медленнее потока, используются свежие кадры; 120 FPS не гарантированы."; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 12; color: "#ffc379" }
                    Label { visible: Chiaki.window.nvidia.milliseconds>16.67; text: "Обработка превышает бюджет 60 FPS (16,7 мс). Уменьшите рабочий размер/число проходов NR или его отключите. Смешивание уменьшает эффект, но не время модели."; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#ffc379"; font.pixelSize: 12 }
                    Label { text: "Интерполяция по движению в самом видео. При неоднозначном движении сохраняется исходная картинка. Счётчики фаз не являются FPS монитора; возможны ошибки на сложном движении."; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9aafc1"; font.pixelSize: 12 }
                }
            }
            Button { text: "Закрыть · Insert"; Layout.fillWidth: true; onClicked: nvidiaMenu.close() }
        }
    }
    Connections { target: root; function onWidthChanged(){nvidiaMenu.constrain();} function onHeightChanged(){nvidiaMenu.constrain();} }
    Connections {
        target: Chiaki.window
        function onNvidiaMenuRequested() {
            console.debug("NVIDIA menu request, visible:", nvidiaMenu.visible);
            nvidiaMenu.visible ? nvidiaMenu.close() : nvidiaMenu.open();
        }
    }

    function controllerButton(name) {
        let type = "deck";
        for (let i = 0; i < Chiaki.controllers.length; ++i) {
            if (Chiaki.controllers[i].playStation) {
                type = "ps";
                break;
            }
        }
        return "image://svg/button-%1#%2".arg(type).arg(name);
    }
    function grabInput(item) {
        Chiaki.window.grabInput();
        restoreFocusItems.push(Window.window.activeFocusItem);
        if (item)
            item.forceActiveFocus(Qt.TabFocusReason);
    }

    function releaseInput() {
        Chiaki.window.releaseInput();
        let item = restoreFocusItems.pop();
        if (item && item.visible)
            item.forceActiveFocus(Qt.TabFocusReason);
    }

    function autoRegister(auto, host, ps5) {
        if(auto)
            Chiaki.autoRegister()
        else
            showRegistDialog(host, ps5);
    }

    function openDisplaySettings() {
        useSeparateStreamSettingsWindows = preferSeparateStreamSettingsWindows;
        const loader = useSeparateStreamSettingsWindows ? separateDisplaySettingsLoader : displaySettingsLoader;
        if(loader.status == Loader.Ready)
        {
            if(placeboSettingsRect.opacity || displaySettingsRect.opacity || colorMappingSettingsRect.opacity)
                closeDialog();
            displaySettingsRect.opacity = 0.8;
            grabInput(loader);
            if (!loader.item.restoreFocusItem) {
                let item = loader.item.mainItem.nextItemInFocusChain();
                if (item)
                    item.forceActiveFocus(Qt.TabFocusReason);
            } else {
                loader.item.restoreFocusItem.forceActiveFocus(Qt.TabFocusReason);
                loader.item.restoreFocusItem = null;
            }
        }
    }
    function openPlaceboSettings() {
        if (!displaySettingsRect.opacity && !placeboSettingsRect.opacity && !colorMappingSettingsRect.opacity)
            useSeparateStreamSettingsWindows = preferSeparateStreamSettingsWindows;
        const loader = useSeparateStreamSettingsWindows ? separatePlaceboSettingsLoader : placeboSettingsLoader;
        if(loader.status == Loader.Ready)
        {
            if(placeboSettingsRect.opacity || displaySettingsRect.opacity || colorMappingSettingsRect.opacity)
                closeDialog();
            placeboSettingsRect.opacity = 0.8;
            grabInput(loader);
            if (!loader.item.restoreFocusItem) {
                let item = loader.item.mainItem.nextItemInFocusChain();
                if (item)
                    item.forceActiveFocus(Qt.TabFocusReason);
            } else {
                loader.item.restoreFocusItem.forceActiveFocus(Qt.TabFocusReason);
                loader.item.restoreFocusItem = null;
            }
        }
    }

    function closeDialog() {
       if(displaySettingsRect.opacity) {
        displaySettingsRect.opacity = 0.0;
        const loader = useSeparateStreamSettingsWindows ? separateDisplaySettingsLoader : displaySettingsLoader;
        loader.item.restoreFocusItem = Window.window.activeFocusItem;
        releaseInput();
        useSeparateStreamSettingsWindows = false;
       }
       else if(placeboSettingsRect.opacity) {
        placeboSettingsRect.opacity = 0.0;
        const loader = useSeparateStreamSettingsWindows ? separatePlaceboSettingsLoader : placeboSettingsLoader;
        loader.item.restoreFocusItem = Window.window.activeFocusItem;
        releaseInput();
        useSeparateStreamSettingsWindows = false;
       }
       else if(colorMappingSettingsRect.opacity) {
        releaseInput();
        colorMappingSettingsRect.opacity = 0.0;
        const loader = useSeparateStreamSettingsWindows ? separateColorMappingSettingsLoader : colorMappingSettingsLoader;
        loader.item.restoreFocusItem = Window.window.activeFocusItem;
        root.openPlaceboSettings();
       }
       else
        stack.pop();
    }

    function showMainView() {
        if(displaySettingsRect.opacity) {
            displaySettingsRect.opacity = 0.0;
            const loader = useSeparateStreamSettingsWindows ? separateDisplaySettingsLoader : displaySettingsLoader;
            loader.item.restoreFocusItem = Window.window.activeFocusItem;
            releaseInput();
            useSeparateStreamSettingsWindows = false;
        }
        else if(placeboSettingsRect.opacity) {
            placeboSettingsRect.opacity = 0.0;
            const loader = useSeparateStreamSettingsWindows ? separatePlaceboSettingsLoader : placeboSettingsLoader;
            loader.item.restoreFocusItem = Window.window.activeFocusItem;
            releaseInput();
            useSeparateStreamSettingsWindows = false;
        }
        else if(colorMappingSettingsRect.opacity) {
            colorMappingSettingsRect.opacity = 0.0;
            const loader = useSeparateStreamSettingsWindows ? separateColorMappingSettingsLoader : colorMappingSettingsLoader;
            loader.item.restoreFocusItem = Window.window.activeFocusItem;
            releaseInput();
            useSeparateStreamSettingsWindows = false;
        }
        if (stack.depth > 1)
            stack.pop(stack.get(0));
        else
            stack.replace(stack.get(0), mainViewComponent);
    }

    function showStreamView() {
        stack.replace(stack.get(0), streamViewComponent);
    }

    function showPsnView() {
        stack.replace(stack.get(0), psnViewComponent, {}, StackView.Immediate)
    }

    function showManualHostDialog() {
        stack.push(manualHostDialogComponent);
    }

    function showConfirmDialog(title, text, callback, rejectCallback = null) {
        confirmDialog.title = title;
        confirmDialog.text = text;
        confirmDialog.callback = callback;
        confirmDialog.rejectCallback = rejectCallback;
        confirmDialog.restoreFocusItem = Window.window.activeFocusItem;
        confirmDialog.open();
    }

    function showInfoDialog(title, text) {
        infoDialog.title = title;
        infoDialog.text = text;
        infoDialog.restoreFocusItem = Window.window.activeFocusItem;
        infoDialog.open();
    }

    function showRendererFallbackDialog(reason) {
        showInfoDialog(qsTr("Renderer Fallback"),
                       qsTr("Vulkan renderer is unavailable and chiaki-ng switched to OpenGL.\n\nReason: %1").arg(reason));
    }

    function showRemindDialog(title, text, remotePlay, callback) {
        remindDialog.title = title;
        remindDialog.text = text;
        remindDialog.remotePlay = remotePlay;
        remindDialog.callback = callback;
        remindDialog.restoreFocusItem = Window.window.activeFocusItem;
        remindDialog.open();
    }


    function showRegistDialog(host, ps5) {
        stack.push(registDialogComponent, {host: host, ps5: ps5});
    }

    function showSettingsDialog() {
        stack.push(settingsDialogComponent);
    }

    function showDisplaySettingsDialog() {
        stack.push(displaySettingsDialogComponent);
    }

    function showPlaceboSettingsDialog() {
        stack.push(placeboSettingsDialogComponent);
    }

    function showPlaceboColorMappingDialog() {
        if(placeboSettingsRect.opacity)
        {
            root.closeDialog();
            useSeparateStreamSettingsWindows = preferSeparateStreamSettingsWindows;
            const loader = useSeparateStreamSettingsWindows ? separateColorMappingSettingsLoader : colorMappingSettingsLoader;
            if(loader.status == Loader.Ready)
            {
                grabInput(loader);
                colorMappingSettingsRect.opacity = 0.8;
                if (!loader.item.restoreFocusItem) {
                    let item = loader.item.mainItem.nextItemInFocusChain();
                    if (item)
                        item.forceActiveFocus(Qt.TabFocusReason);
                } else {
                    loader.item.restoreFocusItem.forceActiveFocus(Qt.TabFocusReason);
                    loader.item.restoreFocusItem = null;
                }
            }
        }
        else
            stack.push(placeboColorMappingDialogComponent);
    }

    function showProfileDialog() {
        stack.push(profileDialogComponent)
    }

    function showConsolePinDialog(consoleIndex) {
        stack.push(consolePinDialogComponent, {consoleIndex: consoleIndex});
    }

    function showSteamShortcutDialog(fromReminder) {
        stack.push(steamShortcutDialogComponent, {fromReminder: fromReminder});
    }

    function showPSNTokenDialog(psnurl, expired) {
        stack.push(psnTokenDialogComponent, {psnurl: psnurl, expired: expired});
    }

    function showControllerMappingDialog() {
        stack.push(controllerMappingDialogComponent)
    }

    Component.onCompleted: {
        if (Chiaki.session)
            stack.replace(stack.get(0), streamViewComponent, {}, StackView.Immediate);
        else if (Chiaki.autoConnect)
            stack.replace(stack.get(0), autoConnectViewComponent, {}, StackView.Immediate);
    }

    Pane {
        anchors.fill: parent
        visible: !Chiaki.window.hasVideo && !Chiaki.window.keepVideo
    }

    StackView {
        id: stack
        anchors.fill: parent
        hoverEnabled: false
        initialItem: mainViewComponent
        font.pixelSize: 20

        replaceEnter: Transition {
            PropertyAnimation {
                property: "opacity"
                from: 0.01
                to: 1.0
                duration: 200
            }
        }

        replaceExit: Transition {
            PropertyAnimation {
                property: "opacity"
                from: 1.0
                to: 0.0
                duration: 200
            }
        }
    }

    Rectangle {
        id: placeboSettingsRect
        opacity: 0.0
        visible: opacity && !useSeparateStreamSettingsWindows
        height: 650
        width: 1200
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        color: Material.background
        Loader {
            anchors.fill: parent
            id: placeboSettingsLoader
            sourceComponent: placeboSettingsDialogComponent
        }
    }
    Rectangle {
        id: displaySettingsRect
        opacity: 0.0
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        height: 500
        width: 1200
        visible: opacity && !useSeparateStreamSettingsWindows
        color: Material.background
        Loader {
            anchors.fill: parent
            id: displaySettingsLoader
            sourceComponent: displaySettingsDialogComponent
        }
    }
    Rectangle {
        id: colorMappingSettingsRect
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        opacity: 0.0
        height: 600
        width: 1200
        visible: opacity && !useSeparateStreamSettingsWindows
        color: Material.background
        Loader {
            anchors.fill: parent
            id: colorMappingSettingsLoader
            sourceComponent: placeboColorMappingDialogComponent
        }
    }

    Window {
        id: separateDisplaySettingsWindow
        visible: useSeparateStreamSettingsWindows && displaySettingsRect.opacity > 0.0
        transientParent: Chiaki.window
        flags: Qt.Dialog | Qt.FramelessWindowHint
        color: Material.background
        modality: Qt.NonModal
        x: Math.round(root.mapToGlobal(0, 0).x)
        y: Math.round(root.mapToGlobal(0, 0).y)
        width: Math.round(root.width)
        height: separateDisplaySettingsLoader.item ? Math.min(Math.round(root.height), Math.round(separateDisplaySettingsLoader.item.gridHeight + 140)) : 500
        onVisibleChanged: if (visible) requestActivate()

        Shortcut {
            sequence: "Ctrl+O"
            onActivated: root.closeDialog()
        }

        Shortcut {
            sequence: StandardKey.Cancel
            onActivated: root.closeDialog()
        }

        Loader {
            anchors.fill: parent
            id: separateDisplaySettingsLoader
            sourceComponent: displaySettingsDialogComponent
        }
    }

    Window {
        id: separatePlaceboSettingsWindow
        visible: useSeparateStreamSettingsWindows && placeboSettingsRect.opacity > 0.0
        transientParent: Chiaki.window
        flags: Qt.Dialog | Qt.FramelessWindowHint
        color: Material.background
        modality: Qt.NonModal
        x: Math.round(root.mapToGlobal(0, 0).x)
        y: Math.round(root.mapToGlobal(0, 0).y)
        width: Math.round(root.width)
        height: Math.min(Math.round(root.height), 650)
        onVisibleChanged: if (visible) requestActivate()

        Shortcut {
            sequence: "Ctrl+O"
            onActivated: root.closeDialog()
        }

        Shortcut {
            sequence: StandardKey.Cancel
            onActivated: root.closeDialog()
        }

        Loader {
            anchors.fill: parent
            id: separatePlaceboSettingsLoader
            sourceComponent: placeboSettingsDialogComponent
        }
    }

    Window {
        id: separateColorMappingSettingsWindow
        visible: useSeparateStreamSettingsWindows && colorMappingSettingsRect.opacity > 0.0
        transientParent: Chiaki.window
        flags: Qt.Dialog | Qt.FramelessWindowHint
        color: Material.background
        modality: Qt.NonModal
        x: Math.round(root.mapToGlobal(0, 0).x)
        y: Math.round(root.mapToGlobal(0, 0).y)
        width: Math.round(root.width)
        height: Math.min(Math.round(root.height), 600)
        onVisibleChanged: if (visible) requestActivate()

        Shortcut {
            sequence: "Ctrl+O"
            onActivated: root.closeDialog()
        }

        Shortcut {
            sequence: StandardKey.Cancel
            onActivated: root.closeDialog()
        }

        Loader {
            anchors.fill: parent
            id: separateColorMappingSettingsLoader
            sourceComponent: placeboColorMappingDialogComponent
        }
    }
    Rectangle {
        anchors {
            bottom: parent.bottom
            horizontalCenter: parent.horizontalCenter
            bottomMargin: 30
        }
        color: Material.accent
        width: errorLayout.width + 40
        height: errorLayout.height + 20
        radius: 8
        opacity: errorHideTimer.running ? 0.8 : 0.0

        Behavior on opacity { NumberAnimation { duration: 500 } }

        ColumnLayout {
            id: errorLayout
            anchors.centerIn: parent

            Label {
                id: errorTitleLabel
                Layout.alignment: Qt.AlignCenter
                font.bold: true
                font.pixelSize: 24
            }

            Label {
                id: errorTextLabel
                Layout.alignment: Qt.AlignCenter
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 20
            }
        }

        Timer {
            id: errorHideTimer
            interval: 2000
        }
    }

    Dialog {
        id: infoDialog
        property alias text: infoDialogLabel.text
        property Item restoreFocusItem
        parent: Overlay.overlay
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        modal: true
        Material.roundedScale: Material.MediumScale
        onOpened: infoDialogLabel.forceActiveFocus(Qt.TabFocusReason)
        onClosed: {
            if (restoreFocusItem)
                restoreFocusItem.forceActiveFocus(Qt.TabFocusReason);
            infoDialogLabel.focus = false;
        }

        Component.onCompleted: {
            header.horizontalAlignment = Text.AlignHCenter;
            header.background = null;
        }

        ColumnLayout {
            spacing: 20

            Label {
                id: infoDialogLabel
                Keys.onEscapePressed: infoDialog.close()
                Keys.onReturnPressed: infoDialog.close()
            }

            RowLayout {
                Layout.alignment: Qt.AlignCenter

                Button {
                    text: qsTr("OK")
                    Material.background: Material.accent
                    flat: true
                    leftPadding: 50
                    onClicked: infoDialog.close()
                    Material.roundedScale: Material.SmallScale

                    Image {
                        anchors {
                            left: parent.left
                            verticalCenter: parent.verticalCenter
                            leftMargin: 12
                        }
                        width: 28
                        height: 28
                        sourceSize: Qt.size(width, height)
                        source: root.controllerButton("cross")
                    }
                }
            }
        }
    }

    ConfirmDialog {
        id: confirmDialog
    }

    RemindDialog {
        id: remindDialog
    }

    Connections {
        target: Chiaki

        function onSessionChanged() {
            if (Chiaki.session)
                root.showStreamView();
        }

        function onShowPsnView() {
            root.showPsnView();
        }

        function onPsnCredsExpired() {
            Chiaki.settings.psnRefreshToken = ""
            Chiaki.settings.psnAuthToken = ""
            Chiaki.settings.psnAuthTokenExpiry = ""
            Chiaki.settings.psnAccountId = ""
            root.showPSNTokenDialog(true);
        }

        function onError(title, text) {
            errorTitleLabel.text = title;
            errorTextLabel.text = text;
            errorHideTimer.start();
        }

        function onRegistDialogRequested(host, ps5, duid) {
            if(!duid)
                showRegistDialog(host, ps5);
            else
            {
                if(ps5)
                    root.showConfirmDialog(qsTr("Registration Type"), qsTr("Would you like to use automatic registration?"), () => root.autoRegister(true, host, ps5), () => root.autoRegister(false, host, ps5))
                else
                    root.showConfirmDialog(qsTr("Registration Type"), qsTr("Would you like to use automatic registration (must be main PS4 console registered to your PSN)?"), () => root.autoRegister(true, host, ps5), () => root.autoRegister(false, host, ps5))
            }
        }

        function onWakeupStartInitiated() {
            stack.replace(stack.get(0), autoConnectViewComponent, {}, StackView.Immediate);
        }
    }

    Component {
        id: mainViewComponent
        MainView { }
    }

    Component {
        id: streamViewComponent
        StreamView { }
    }

    Component {
        id: autoConnectViewComponent
        AutoConnectView { }
    }

    Component {
        id: psnViewComponent
        PsnView {}
    }

    Component {
        id: manualHostDialogComponent
        ManualHostDialog { }
    }

    Component {
        id: settingsDialogComponent
        SettingsDialog { }
    }

    Component {
        id: displaySettingsDialogComponent
        DisplaySettingsDialog { }
    }

    Component {
        id: placeboSettingsDialogComponent
        PlaceboSettingsDialog { }
    }

    Component {
        id: placeboColorMappingDialogComponent
        PlaceboColorMappingDialog { }
    }

    Component {
        id: profileDialogComponent
        ProfileDialog { }
    }

    Component {
        id: consolePinDialogComponent
        ConsolePinDialog { }
    }

    Component {
        id: steamShortcutDialogComponent
        SteamShortcutDialog { }
    }

    Component {
        id: psnTokenDialogComponent
        PSNTokenDialog { }
    }

    Component {
        id: registDialogComponent
        RegistDialog { }
    }

    Component {
        id: controllerMappingDialogComponent
        ControllerMappingDialog { }
    }
}

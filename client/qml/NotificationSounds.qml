import QtQuick
import QtMultimedia

Item {
    id: control
    property int volumePercent: 50
    function effect(kind) {
        return kind === "invitation" ? invitation : kind === "knock" ? knock
            : kind === "join" ? join : kind === "leave" ? leave
            : kind === "message" ? message : null;
    }
    function play(kind) {
        const sound = effect(kind);
        if (!sound || sound.playing) return;
        sound.play();
        if (kind === "invitation") invitationLimit.restart();
    }
    function stopInvitation() {
        invitationLimit.stop();
        invitation.stop();
    }
    function preview(kind, volume) {
        stopPreview();
        const sound = effect(kind);
        if (!sound) return;
        sample.source = sound.source;
        sample.volume = volume / 100 * (kind === "join" ? 0.8 : kind === "leave" ? 0.7 : kind === "message" ? 0.6 : 1);
        sample.loops = kind === "invitation" ? SoundEffect.Infinite : 1;
        sample.play();
        previewLimit.restart();
    }
    function stopPreview() { previewLimit.stop(); sample.stop(); }
    Timer { id: invitationLimit; interval: 30000; onTriggered: invitation.stop() }
    Timer { id: previewLimit; interval: 5000; onTriggered: sample.stop() }
    SoundEffect {
        id: invitation
        source: "qrc:/sounds/invitation.wav"
        volume: control.volumePercent / 100
        loops: SoundEffect.Infinite
    }
    SoundEffect { id: knock; source: "qrc:/sounds/knock.wav"; volume: control.volumePercent / 100 }
    SoundEffect { id: join; source: "qrc:/sounds/join.wav"; volume: control.volumePercent / 100 * 0.8 }
    SoundEffect { id: leave; source: "qrc:/sounds/leave.wav"; volume: control.volumePercent / 100 * 0.7 }
    SoundEffect { id: message; source: "qrc:/sounds/message.wav"; volume: control.volumePercent / 100 * 0.6 }
    SoundEffect { id: sample }
}

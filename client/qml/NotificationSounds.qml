import QtQuick
import QtMultimedia

Item {
    function play(kind) {
        const sound = kind === "knock" ? knock : kind === "join" ? join
                    : kind === "leave" ? leave : kind === "message" ? message : null;
        if (sound && !sound.playing) sound.play();
    }
    SoundEffect { id: knock; source: "qrc:/sounds/knock.wav"; volume: 0.5 }
    SoundEffect { id: join; source: "qrc:/sounds/join.wav"; volume: 0.4 }
    SoundEffect { id: leave; source: "qrc:/sounds/leave.wav"; volume: 0.35 }
    SoundEffect { id: message; source: "qrc:/sounds/message.wav"; volume: 0.3 }
}

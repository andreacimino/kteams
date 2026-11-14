#pragma once

#include <QList>
#include <QString>

// The microphones, speakers and cameras of this computer, and the ones chosen in the settings
// (see tests/tst_mediadevices.cpp). Audio devices are the PulseAudio ones, which is what the
// call pipeline reads and writes (served by PipeWire on Plasma); cameras are the V4L2 capture
// nodes, as the pipeline opens them.
namespace MediaDevices {

struct Device {
    QString id;   // what the pipeline takes: a PulseAudio name, or a /dev/video path
    QString name; // what the user reads
};

enum class Kind { AudioInput, AudioOutput, Camera };

// "audioInput", "audioOutput" or "camera" (the names QML uses) -> Kind; false for anything else.
bool kindFromName(const QString &name, Kind *kind);

// Monitors of outputs are not microphones and are left out.
QList<Device> audioInputs();
QList<Device> audioOutputs();
// The V4L2 capture nodes found under `sysfsRoot` (the metadata nodes of a camera are skipped).
// Two entries with the same name get their path appended, to tell them apart.
QList<Device> cameras(const QString &sysfsRoot = QStringLiteral("/sys/class/video4linux"), const QString &devRoot = QStringLiteral("/dev"));

// The choice kept in the settings; "" means the system default.
QString selected(Kind kind);
void select(Kind kind, const QString &id);
// The choice, if the device is there now; "" (the default) otherwise, so a call never fails
// because the chosen USB microphone is unplugged. The choice itself is kept for when it is back.
QString selectedIfPresent(Kind kind);

// The camera the call uses: KOLLEGA_CAMERA if set, else the one chosen, else /dev/video0.
QString cameraToUse(const QString &environmentValue, const QString &chosen);

// ` device="<id>" ` for a gst_parse_launch description, or "" for the default.
QString pipelineDeviceProperty(const QString &id);
// A value quoted for gst_parse_launch: in double quotes, with \ and " escaped.
QString quoteForPipeline(const QString &value);

}

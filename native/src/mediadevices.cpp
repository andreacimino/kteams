#include "mediadevices.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QTextStream>
#include <gst/gst.h>

namespace MediaDevices {
namespace {
constexpr auto kDefaultCamera = "/dev/video0";

QString settingsKey(Kind kind)
{
    switch (kind) {
    case Kind::AudioInput: return QStringLiteral("devices/audioInput");
    case Kind::AudioOutput: return QStringLiteral("devices/audioOutput");
    case Kind::Camera: return QStringLiteral("devices/camera");
    }
    return {};
}

// PulseAudio devices of one GStreamer class ("Audio/Source", "Audio/Sink"), through the
// PulseAudio device provider: the element it makes carries the name pulsesrc/pulsesink take.
QList<Device> pulseDevices(const char *wantedClass)
{
    static const bool initialized = [] { gst_init(nullptr, nullptr); return true; }();
    Q_UNUSED(initialized);
    QList<Device> result;
    GstDeviceProvider *provider = gst_device_provider_factory_get_by_name("pulsedeviceprovider");
    if (!provider) return result;
    GList *devices = gst_device_provider_get_devices(provider);
    for (GList *l = devices; l; l = l->next) {
        GstDevice *device = GST_DEVICE(l->data);
        gchar *deviceClass = gst_device_get_device_class(device);
        const bool wanted = g_strcmp0(deviceClass, wantedClass) == 0;
        g_free(deviceClass);
        if (!wanted) continue;
        GstStructure *properties = gst_device_get_properties(device);
        const bool monitor = properties && g_strcmp0(gst_structure_get_string(properties, "device.class"), "monitor") == 0;
        if (properties) gst_structure_free(properties);
        if (monitor) continue;
        gchar *name = gst_device_get_display_name(device);
        GstElement *element = gst_device_create_element(device, nullptr);
        gchar *id = nullptr;
        if (element && g_object_class_find_property(G_OBJECT_GET_CLASS(element), "device")) g_object_get(element, "device", &id, nullptr);
        if (id && *id) result.append({QString::fromUtf8(id), QString::fromUtf8(name)});
        g_free(id);
        g_free(name);
        if (element) gst_object_unref(element);
    }
    g_list_free_full(devices, gst_object_unref);
    gst_object_unref(provider);
    return result;
}

QString readLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readLine()).trimmed();
}
}

bool kindFromName(const QString &name, Kind *kind)
{
    if (name == QLatin1String("audioInput")) *kind = Kind::AudioInput;
    else if (name == QLatin1String("audioOutput")) *kind = Kind::AudioOutput;
    else if (name == QLatin1String("camera")) *kind = Kind::Camera;
    else return false;
    return true;
}

QList<Device> audioInputs() { return pulseDevices("Audio/Source"); }
QList<Device> audioOutputs() { return pulseDevices("Audio/Sink"); }

QList<Device> cameras(const QString &sysfsRoot, const QString &devRoot)
{
    QList<Device> found;
    const QDir root(sysfsRoot);
    QStringList nodes = root.entryList({QStringLiteral("video*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    // video2 before video10: by the number.
    std::sort(nodes.begin(), nodes.end(), [](const QString &a, const QString &b) {
        return a.mid(5).toInt() < b.mid(5).toInt() || (a.mid(5).toInt() == b.mid(5).toInt() && a < b);
    });
    for (const QString &node : nodes) {
        // A camera shows several nodes: index 0 captures, the others carry metadata.
        const QString index = readLine(root.filePath(node + QStringLiteral("/index")));
        if (!index.isEmpty() && index != QLatin1String("0")) continue;
        QString name = readLine(root.filePath(node + QStringLiteral("/name")));
        if (name.isEmpty()) name = node;
        found.append({devRoot + QLatin1Char('/') + node, name});
    }
    QSet<QString> seen, duplicated;
    for (const Device &d : std::as_const(found)) if (seen.contains(d.name)) duplicated.insert(d.name); else seen.insert(d.name);
    for (Device &d : found) if (duplicated.contains(d.name)) d.name += QStringLiteral(" (%1)").arg(d.id);
    return found;
}

QString selected(Kind kind) { return QSettings().value(settingsKey(kind)).toString(); }

void select(Kind kind, const QString &id)
{
    QSettings settings;
    if (id.isEmpty()) settings.remove(settingsKey(kind));
    else settings.setValue(settingsKey(kind), id);
}

QString selectedIfPresent(Kind kind)
{
    const QString chosen = selected(kind);
    if (chosen.isEmpty()) return {};
    const QList<Device> available = kind == Kind::AudioInput ? audioInputs() : kind == Kind::AudioOutput ? audioOutputs() : cameras();
    for (const Device &d : available) if (d.id == chosen) return chosen;
    return {};
}

QString cameraToUse(const QString &environmentValue, const QString &chosen)
{
    if (!environmentValue.isEmpty()) return environmentValue;
    return chosen.isEmpty() ? QString::fromLatin1(kDefaultCamera) : chosen;
}

QString quoteForPipeline(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString pipelineDeviceProperty(const QString &id)
{
    return id.isEmpty() ? QString() : QStringLiteral("device=%1 ").arg(quoteForPipeline(id));
}

}

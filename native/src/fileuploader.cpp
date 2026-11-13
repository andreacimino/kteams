#include "fileuploader.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {
constexpr auto kGraph = "https://graph.microsoft.com/v1.0";
constexpr auto kChatFolder = "Microsoft Teams Chat Files";
// Graph upload sessions want chunks in multiples of 320 KiB; 5 MiB = 16 x 320 KiB.
constexpr qint64 kChunk = 16 * 320 * 1024;

QString httpError(QNetworkReply *reply, const QByteArray &body)
{
    const QString graphMessage = QJsonDocument::fromJson(body).object().value(QLatin1String("error")).toObject()
                                     .value(QLatin1String("message")).toString();
    return QStringLiteral("HTTP %1 %2").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
        .arg(graphMessage.isEmpty() ? reply->errorString() : graphMessage);
}
}

FileUploader::FileUploader(QObject *parent) : QObject(parent), m_graph(QString::fromLatin1(kGraph)) {}

void FileUploader::setGraphBase(const QString &base) { m_graph = base; }

QNetworkRequest FileUploader::graphRequest(const QString &pathOrUrl) const
{
    QNetworkRequest req(QUrl(pathOrUrl.startsWith(QLatin1String("https://")) ? pathOrUrl : m_graph + pathOrUrl));
    req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    req.setRawHeader("Accept", "application/json");
    return req;
}

void FileUploader::fail(const QString &message)
{
    m_file.reset();
    emit failed(message);
}

void FileUploader::upload(const QString &localPath, const QString &graphToken, const QString &channelGroupId, const QString &channelId)
{
    if (m_file) { emit failed(tr("Another file is being uploaded.")); return; }
    m_file = std::make_unique<QFile>(localPath);
    if (!m_file->open(QIODevice::ReadOnly)) { fail(tr("Cannot read %1").arg(localPath)); return; }
    m_token = graphToken;
    m_fileName = QFileInfo(localPath).fileName();
    m_channel = !channelId.isEmpty();
    m_offset = 0;
    emit progress(0);

    const QString encodedName = QString::fromLatin1(QUrl::toPercentEncoding(m_fileName));
    if (!m_channel) {
        startSession(QStringLiteral("/me/drive/root:/%1/%2:")
                         .arg(QString::fromLatin1(QUrl::toPercentEncoding(QString::fromLatin1(kChatFolder))), encodedName));
        return;
    }
    // The channel's folder in the team's SharePoint site.
    QNetworkReply *reply = m_nam.get(graphRequest(QStringLiteral("/teams/%1/channels/%2/filesFolder")
                                                      .arg(QString::fromLatin1(QUrl::toPercentEncoding(channelGroupId)),
                                                           QString::fromLatin1(QUrl::toPercentEncoding(channelId)))));
    connect(reply, &QNetworkReply::finished, this, [this, reply, encodedName] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        const QJsonObject folder = QJsonDocument::fromJson(body).object();
        const QString driveId = folder.value(QLatin1String("parentReference")).toObject().value(QLatin1String("driveId")).toString();
        const QString folderId = folder.value(QLatin1String("id")).toString();
        if (reply->error() != QNetworkReply::NoError || driveId.isEmpty() || folderId.isEmpty()) {
            fail(tr("Channel folder not found: ") + httpError(reply, body));
            return;
        }
        startSession(QStringLiteral("/drives/%1/items/%2:/%3:").arg(driveId, folderId, encodedName));
    });
}

void FileUploader::startSession(const QString &itemPathPrefix)
{
    QNetworkRequest req = graphRequest(itemPathPrefix + QStringLiteral("/createUploadSession"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // Never overwrite: a second file with the same name gets a new name.
    const QJsonObject body{{QStringLiteral("item"), QJsonObject{{QStringLiteral("@microsoft.graph.conflictBehavior"), QStringLiteral("rename")}}}};
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        m_uploadUrl = QJsonDocument::fromJson(body).object().value(QLatin1String("uploadUrl")).toString();
        if (reply->error() != QNetworkReply::NoError || m_uploadUrl.isEmpty()) {
            fail(tr("Upload not started: ") + httpError(reply, body));
            return;
        }
        sendNextChunk();
    });
}

void FileUploader::sendNextChunk()
{
    const qint64 total = m_file->size();
    m_file->seek(m_offset);
    const QByteArray chunk = m_file->read(kChunk);
    const qint64 last = m_offset + chunk.size() - 1;
    // The upload URL is pre-authorized: no Authorization header (Graph documentation).
    QNetworkRequest req{QUrl(m_uploadUrl)};
    req.setRawHeader("Content-Range", QStringLiteral("bytes %1-%2/%3").arg(m_offset).arg(last).arg(total).toLatin1());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    QNetworkReply *reply = m_nam.put(req, chunk);
    connect(reply, &QNetworkReply::finished, this, [this, reply, last, total] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) { fail(tr("Upload interrupted: ") + httpError(reply, body)); return; }
        m_offset = last + 1;
        emit progress(total > 0 ? double(m_offset) / double(total) : 1.0);
        if (m_offset < total) { sendNextChunk(); return; }
        // Last chunk: the response is the new driveItem.
        const QJsonObject item = QJsonDocument::fromJson(body).object();
        describe(item.value(QLatin1String("parentReference")).toObject().value(QLatin1String("driveId")).toString(),
                 item.value(QLatin1String("id")).toString());
    });
}

void FileUploader::describe(const QString &driveId, const QString &itemId)
{
    if (driveId.isEmpty() || itemId.isEmpty()) { fail(tr("Unexpected upload response.")); return; }
    QNetworkReply *reply = m_nam.get(graphRequest(QStringLiteral("/drives/%1/items/%2?$select=id,name,webUrl,sharepointIds,parentReference")
                                                      .arg(driveId, itemId)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, driveId, itemId] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        const QJsonObject item = QJsonDocument::fromJson(body).object();
        if (reply->error() != QNetworkReply::NoError) { fail(tr("File uploaded but not readable: ") + httpError(reply, body)); return; }
        if (m_channel) { finishWith(item, item.value(QLatin1String("webUrl")).toString()); return; }

        QNetworkRequest req = graphRequest(QStringLiteral("/drives/%1/items/%2/createLink").arg(driveId, itemId));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        const QJsonObject linkBody{{QStringLiteral("type"), QStringLiteral("edit")}, {QStringLiteral("scope"), QStringLiteral("organization")}};
        QNetworkReply *link = m_nam.post(req, QJsonDocument(linkBody).toJson(QJsonDocument::Compact));
        connect(link, &QNetworkReply::finished, this, [this, link, item] {
            link->deleteLater();
            const QByteArray linkJson = link->readAll();
            const QString shareUrl = QJsonDocument::fromJson(linkJson).object().value(QLatin1String("link")).toObject()
                                         .value(QLatin1String("webUrl")).toString();
            if (link->error() != QNetworkReply::NoError || shareUrl.isEmpty()) {
                fail(tr("File uploaded but not shared: ") + httpError(link, linkJson));
                return;
            }
            finishWith(item, shareUrl);
        });
    });
}

void FileUploader::finishWith(const QJsonObject &item, const QString &shareUrl)
{
    const QJsonObject sp = item.value(QLatin1String("sharepointIds")).toObject();
    const QString name = item.value(QLatin1String("name")).toString(m_fileName);
    const QString ext = QFileInfo(name).suffix().toLower();
    const QString webUrl = item.value(QLatin1String("webUrl")).toString();
    const QString siteUrl = sp.value(QLatin1String("siteUrl")).toString();
    const QString uniqueId = sp.value(QLatin1String("listItemUniqueId")).toString();
    const QString driveId = item.value(QLatin1String("parentReference")).toObject().value(QLatin1String("driveId")).toString();

    // Same fields and values as the files Teams sends (seen in this tenant's messages).
    const QJsonObject entry{
        {QStringLiteral("@type"), QStringLiteral("http://schema.skype.com/File")},
        {QStringLiteral("version"), 2},
        {QStringLiteral("id"), uniqueId},
        {QStringLiteral("itemid"), uniqueId},
        {QStringLiteral("baseUrl"), siteUrl},
        {QStringLiteral("type"), ext},
        {QStringLiteral("fileType"), ext},
        {QStringLiteral("title"), name},
        {QStringLiteral("fileName"), name},
        {QStringLiteral("state"), QStringLiteral("active")},
        {QStringLiteral("objectUrl"), webUrl},
        {QStringLiteral("providerData"), QString()},
        {QStringLiteral("botFileProperties"), QJsonObject()},
        {QStringLiteral("permissionScope"), m_channel ? QString() : QStringLiteral("Organization")},
        {QStringLiteral("fileChicletState"), QJsonObject{{QStringLiteral("serviceName"), QStringLiteral("p2p")}, {QStringLiteral("state"), QStringLiteral("active")}}},
        {QStringLiteral("filePreview"), QJsonObject{{QStringLiteral("previewUrl"), QString()}, {QStringLiteral("previewWidth"), 0}, {QStringLiteral("previewHeight"), 0}}},
        {QStringLiteral("fileInfo"), QJsonObject{{QStringLiteral("itemId"), item.value(QLatin1String("id")).toString()},
                                                 {QStringLiteral("fileUrl"), webUrl},
                                                 {QStringLiteral("siteUrl"), siteUrl},
                                                 {QStringLiteral("serverRelativeUrl"), QString()},
                                                 {QStringLiteral("shareUrl"), shareUrl},
                                                 {QStringLiteral("shareId"), QString()}}},
        {QStringLiteral("sharepointIds"), QJsonObject{{QStringLiteral("driveId"), driveId},
                                                      {QStringLiteral("listId"), sp.value(QLatin1String("listId"))},
                                                      {QStringLiteral("listItemUniqueId"), uniqueId},
                                                      {QStringLiteral("siteId"), sp.value(QLatin1String("siteId"))},
                                                      {QStringLiteral("siteUrl"), siteUrl},
                                                      {QStringLiteral("webId"), sp.value(QLatin1String("webId"))}}}};
    m_file.reset();
    emit uploaded(entry);
}

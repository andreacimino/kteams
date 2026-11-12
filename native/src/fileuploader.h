#pragma once

#include <QFile>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <memory>

// Shares a file the way Teams does, through Microsoft Graph:
// - chats: into the user's OneDrive "Microsoft Teams Chat Files" folder, plus an
//   organization-wide edit link (what this tenant's Teams uses: permissionScope "Organization");
// - channels: into the channel's SharePoint folder (team members already have access).
// The result is one entry of a message's properties.files, in the format Teams sends.
class FileUploader : public QObject
{
    Q_OBJECT
public:
    explicit FileUploader(QObject *parent = nullptr);

    // channelGroupId/channelId empty: a chat. One upload at a time.
    void upload(const QString &localPath, const QString &graphToken,
                const QString &channelGroupId = QString(), const QString &channelId = QString());
    bool busy() const { return m_file != nullptr; }

signals:
    void progress(double fraction);
    void uploaded(const QJsonObject &fileEntry);
    void failed(const QString &message);

private:
    QNetworkRequest graphRequest(const QString &pathOrUrl) const;
    void startSession(const QString &itemPathPrefix);
    void sendNextChunk();
    void describe(const QString &driveId, const QString &itemId);
    void finishWith(const QJsonObject &item, const QString &shareUrl);
    void fail(const QString &message);

    QNetworkAccessManager m_nam;
    std::unique_ptr<QFile> m_file;
    QString m_token;
    QString m_fileName;
    bool m_channel = false;
    QString m_uploadUrl;
    qint64 m_offset = 0;
};

#pragma once

#include <QCryptographicHash>
#include <QString>
#include <QStringList>

// Demo mode (KOLLEGA_DEMO=1): what the interface shows is replaced by made-up names, titles
// and sentences, chosen deterministically from the real text, so screenshots carry no
// real data. Nothing else changes: the real data is still fetched and cached as usual.
namespace Demo {

inline bool enabled()
{
    static const bool on = qgetenv("KOLLEGA_DEMO") == "1";
    return on;
}

inline int pick(const QString &s, int n)
{
    const QByteArray h = QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Md5);
    return int(quint8(h[0]) | (quint8(h[1]) << 8)) % n;
}

inline QString name(const QString &real)
{
    static const QStringList names{QStringLiteral("Alice Bianchi"), QStringLiteral("Marco Verdi"), QStringLiteral("Giulia Russo"),
                                   QStringLiteral("Luca Ferrari"), QStringLiteral("Sara Esposito"), QStringLiteral("Paolo Romano"),
                                   QStringLiteral("Chiara Colombo"), QStringLiteral("Davide Ricci"), QStringLiteral("Elena Greco"),
                                   QStringLiteral("Andrea Conti"), QStringLiteral("Martina Gallo"), QStringLiteral("Simone Costa")};
    if (!enabled() || real.isEmpty()) return real;
    return names[pick(real, names.size())];
}

inline QString title(const QString &real)
{
    static const QStringList titles{QStringLiteral("Project Falcon"), QStringLiteral("Marketing team"), QStringLiteral("Design review"),
                                    QStringLiteral("Release 2.4"), QStringLiteral("Customer onboarding"), QStringLiteral("Friday lunch"),
                                    QStringLiteral("Infrastructure"), QStringLiteral("Sales EMEA"), QStringLiteral("Hackathon 2026")};
    if (!enabled() || real.isEmpty()) return real;
    return titles[pick(real, titles.size())];
}

inline QString text(const QString &real)
{
    static const QStringList lines{
        QStringLiteral("Can we move the review to Thursday afternoon?"), QStringLiteral("Sounds good, I'll send the notes after the call."),
        QStringLiteral("The build is green again, thanks for the quick fix!"), QStringLiteral("Lunch at 12:30? The usual place."),
        QStringLiteral("I've uploaded the slides to the shared folder."), QStringLiteral("Reminder: the demo starts in ten minutes."),
        QStringLiteral("Great work on the release, everyone."), QStringLiteral("Could you have a look at the draft when you have a minute?"),
        QStringLiteral("Let's take this offline, I'll call you."), QStringLiteral("Agreed. I'll update the ticket."),
        QStringLiteral("The customer confirmed the date for the workshop."), QStringLiteral("See you tomorrow at the stand-up.")};
    if (!enabled() || real.isEmpty()) return real;
    return lines[pick(real, lines.size())];
}

}

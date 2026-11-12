# KTeams Native (WIP)

Obiettivo: client **nativo Qt/QML (Kirigami)** per Microsoft Teams (account aziendale), senza webview per la UI, con parità funzionale massima possibile **escluse le videochiamate**.

## Stato
Scaffold iniziale: shell QML + componente di login basato su WebEngine per acquisire sessione/token con le normali credenziali aziendali (flusso Microsoft). L’implementazione delle API Teams (chat/canali/team/messaggi/file/presenza/ricerca/ecc.) e della UI nativa è incrementale.

## Nota di realismo
“tutto tranne videochiamate” richiede copertura di molte aree (chat 1:1/gruppo, canali/thread, reazioni, @mention, file/SharePoint, ricerca, persone/presenza, notifiche, activity, calendario, app/tab/bot, ecc.) su API in parte non documentate: va affrontato per moduli prioritari.

#include "mixxxmainwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QStandardPaths>
#include <QStatusBar>
#include <QUrl>

#ifdef Q_OS_ANDROID
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSet>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#endif

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QGLFormat>
#endif

#if defined(__LINUX__) && !defined(__ANDROID__)
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#endif

#ifdef MIXXX_USE_QOPENGL
#include <QGuiApplication>

#include "widget/tooltipqopengl.h"
#include "widget/winitialglwidget.h"
#endif

#include "controllers/keyboard/keyboardeventfilter.h"
#include "coreservices.h"
#include "defs_urls.h"
#include "dialog/dlgabout.h"
#include "dialog/dlgdevelopertools.h"
#include "dialog/dlgkeywheel.h"
#include "moc_mixxxmainwindow.cpp"
#include "preferences/dialog/dlgpreferences.h"
#include "widget/wsearchlineedit.h"
#include "widget/wsuggestionsbar.h"
#ifdef __BROADCAST__
#include "broadcast/broadcastmanager.h"
#endif
#include "control/controlindicatortimer.h"
#include "control/controlobject.h"
#include "library/library.h"
#include "library/library_decl.h"
#include "library/library_prefs.h"
#ifdef __ENGINEPRIME__
#include "library/export/libraryexporter.h"
#endif
#include "library/library_prefs.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#ifdef Q_OS_ANDROID
#include "library/youtube/youtubefeature.h"
#endif
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "recording/recordingmanager.h"
#include "skin/legacy/launchimage.h"
#include "skin/skinloader.h"
#include "soundio/soundmanager.h"
#include "sources/soundsourceproxy.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/debug.h"
#include "util/desktophelper.h"
#include "util/sandbox.h"
#include "util/scopedoverridecursor.h"
#include "util/timer.h"
#include "util/versionstore.h"
#include "waveform/guitick.h"
#include "waveform/sharedglcontext.h"
#include "waveform/visualsmanager.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/wglwidget.h"
#include "widget/wmainmenubar.h"

#ifdef __VINYLCONTROL__
#include "vinylcontrol/vinylcontrolmanager.h"
#endif

namespace {
#if defined(__LINUX__) && !defined(__ANDROID__)
// Detect if the desktop supports a global menu to decide whether we need to rebuild
// and reconnect the menu bar when switching to/from fullscreen mode.
// Compared to QMenuBar::isNativeMenuBar() (requires a set menu bar) and
// Qt::AA_DontUseNativeMenuBar, which may both change, this is way more reliable
// since it's rather unlikely that the Appmenu.Registrar service is unloaded/stopped
// while Mixxx is running.
// This is a reimplementation of QGenericUnixTheme > checkDBusGlobalMenuAvailable()
inline bool supportsGlobalMenu() {
#ifndef QT_NO_DBUS
    QDBusConnection conn = QDBusConnection::sessionBus();
    if (const auto* pIface = conn.interface()) {
        return pIface->isServiceRegistered("com.canonical.AppMenu.Registrar");
    }
#endif
    return false;
}
#endif

const ConfigKey kHideMenuBarConfigKey = ConfigKey("[Config]", "hide_menubar");
const ConfigKey kMenuBarHintConfigKey = ConfigKey("[Config]", "show_menubar_hint");

#ifdef Q_OS_ANDROID
class AndroidRemoteLibraryServer final : public QTcpServer {
  public:
    AndroidRemoteLibraryServer(
            std::shared_ptr<mixxx::CoreServices> pCoreServices,
            QObject* pParent)
            : QTcpServer(pParent),
              m_pCoreServices(std::move(pCoreServices)) {
        connect(this,
                &QTcpServer::newConnection,
                this,
                [this]() {
                    while (hasPendingConnections()) {
                        QTcpSocket* pSocket = nextPendingConnection();
                        if (!pSocket) {
                            continue;
                        }
                        connect(pSocket,
                                &QTcpSocket::readyRead,
                                pSocket,
                                [this, pSocket]() {
                                    handleSocket(pSocket);
                                });
                        connect(pSocket,
                                &QTcpSocket::disconnected,
                                pSocket,
                                &QObject::deleteLater);
                    }
                });

        auto* playedTimer = new QTimer(this);
        playedTimer->setInterval(500);
        connect(playedTimer,
                &QTimer::timeout,
                this,
                [this]() {
                    updatePlayedState();
                });
        playedTimer->start();

        if (auto* pService = onlineAudioService()) {
            connect(pService,
                    &mixxx::YouTubeService::soundCloudDownloadFinished,
                    this,
                    [this](const QString& requestKey,
                            const QString& localPath,
                            const QString& title,
                            const QString& uploader,
                            const QString& sourceUrl) {
                        finishSoundCloudDownload(requestKey,
                                localPath,
                                title,
                                uploader,
                                sourceUrl);
                    });
            connect(pService,
                    &mixxx::YouTubeService::soundCloudDownloadFailed,
                    this,
                    [this](const QString& requestKey, const QString& error) {
                        auto it = m_soundCloudJobs.find(requestKey);
                        if (it == m_soundCloudJobs.end()) {
                            return;
                        }
                        it->state = QStringLiteral("error");
                        it->error = error;
                    });
        }
    }

    bool start() {
        for (quint16 port = 8090; port <= 8099; ++port) {
            if (listen(QHostAddress::AnyIPv4, port)) {
                m_port = port;
                qInfo() << "[RemoteLibrary] listening on port" << m_port;
                return true;
            }
        }
        qWarning() << "[RemoteLibrary] could not bind ports 8090-8099:"
                   << errorString();
        return false;
    }

    QStringList urls() const {
        QStringList result;
        const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
        for (const QHostAddress& address : addresses) {
            if (address.protocol() != QAbstractSocket::IPv4Protocol ||
                    address.isLoopback()) {
                continue;
            }
            const QString ip = address.toString();
            if (ip.startsWith(QStringLiteral("169.254."))) {
                continue;
            }
            result.append(QStringLiteral("http://%1:%2")
                                  .arg(ip)
                                  .arg(m_port));
        }
        result.removeDuplicates();
        return result;
    }

  private:
    static QByteArray pageHtml() {
        return QByteArrayLiteral(R"HTML(<!doctype html>
<html lang="nl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DJ Sugar Bibliotheek</title>
<style>
body{font-family:system-ui,sans-serif;background:#eef1f4;color:#101214;margin:0}
header{position:sticky;top:0;background:#fff;border-bottom:1px solid #bcc4cb;padding:12px;z-index:2}
h1{font-size:24px;margin:0 0 10px}
.controls{display:flex;gap:8px;margin-top:8px}
input{font-size:19px;padding:11px;flex:1;min-width:0;border:1px solid #9aa3ab;border-radius:7px}
button{font-size:18px;min-height:48px;padding:8px 14px;border:1px solid #7f8992;border-radius:7px;background:#fff}
#status{padding:9px 12px;font-size:16px;position:sticky;top:139px;background:#eef1f4;z-index:1}
.section{padding:5px 12px;font-size:16px;font-weight:700;color:#495057}
.track{background:#fff;border-bottom:1px solid #d5dbe0;padding:12px}
.title{font-size:21px;font-weight:700}
.artist{font-size:18px;margin-top:3px;color:#343a40}
.album{font-size:15px;margin-top:3px;color:#697078}
.meta{display:flex;gap:7px;flex-wrap:wrap;margin-top:7px}
.badge{font-size:13px;padding:3px 7px;border-radius:12px;background:#e9ecef}
.played{background:#d8f3dc;font-weight:700}
.online{background:#fff3bf}
.actions{display:flex;gap:10px;margin-top:10px}
.actions button{flex:1;background:#d9ebfa;font-weight:700}
#scresults:empty{display:none}
</style>
</head>
<body>
<header>
<h1>DJ Sugar Bibliotheek</h1>
<div class="controls">
<input id="q" placeholder="Zoek in mijn muziek">
<button id="refresh">Vernieuw</button>
</div>
<div class="controls">
<input id="scq" placeholder="Zoek op SoundCloud">
<button id="scsearch">Zoek</button>
</div>
</header>
<div id="status">Laden...</div>
<div id="scresults"></div>
<div id="list"></div>
<script>
const q=document.getElementById('q');
const scq=document.getElementById('scq');
const list=document.getElementById('list');
const scresults=document.getElementById('scresults');
const status=document.getElementById('status');

function addMeta(row,t,online=false){
  const meta=document.createElement('div');
  meta.className='meta';
  const source=document.createElement('span');
  source.className='badge'+(online?' online':'');
  source.textContent=t.source||'Lokaal';
  meta.appendChild(source);
  if(t.played){
    const p=document.createElement('span');
    p.className='badge played';
    p.textContent='GEDRAAID ✓';
    meta.appendChild(p);
  }
  row.appendChild(meta);
}
function baseRow(t,online=false){
  const row=document.createElement('div');
  row.className='track';
  const title=document.createElement('div');
  title.className='title';
  title.textContent=t.title||'(zonder titel)';
  const artist=document.createElement('div');
  artist.className='artist';
  artist.textContent=t.artist||'';
  const album=document.createElement('div');
  album.className='album';
  album.textContent=t.album||'';
  row.append(title,artist,album);
  addMeta(row,t,online);
  return row;
}
async function tracks(){
  status.textContent='Laden...';
  const r=await fetch('/api/tracks?q='+encodeURIComponent(q.value));
  const data=await r.json();
  list.innerHTML='';
  if(!Array.isArray(data)){
    status.textContent='Bibliotheek laden mislukt';
    return;
  }
  status.textContent=data.length+' nummers';
  for(const t of data){
    const row=baseRow(t,false);
    const actions=document.createElement('div');
    actions.className='actions';
    const b=document.createElement('button');
    b.textContent='LOAD';
    b.onclick=async()=>{
      b.disabled=true;
      const rr=await fetch('/api/load?id='+encodeURIComponent(t.id));
      const x=await rr.json();
      status.textContent=x.ok
        ? 'Geladen op Deck '+x.deck+': '+(t.artist?t.artist+' — ':'')+t.title
        : 'Laden geweigerd: '+(x.error||'onbekend');
      b.disabled=false;
    };
    actions.appendChild(b);
    row.appendChild(actions);
    list.appendChild(row);
  }
}
async function searchSoundCloud(){
  const term=scq.value.trim();
  if(!term)return;
  status.textContent='SoundCloud zoeken...';
  document.getElementById('scsearch').disabled=true;
  try{
    const r=await fetch('/api/soundcloud/search?q='+encodeURIComponent(term));
    const data=await r.json();
    scresults.innerHTML='';
    if(!Array.isArray(data)){
      status.textContent='SoundCloud zoeken mislukt: '+(data.error||'onbekend');
      return;
    }
    const head=document.createElement('div');
    head.className='section';
    head.textContent='SoundCloud — '+data.length+' resultaten';
    scresults.appendChild(head);
    for(const t of data){
      t.source='SoundCloud online';
      const row=baseRow(t,true);
      const actions=document.createElement('div');
      actions.className='actions';
      const b=document.createElement('button');
      b.textContent='LOAD';
      b.onclick=()=>loadSoundCloud(t,b);
      actions.appendChild(b);
      row.appendChild(actions);
      scresults.appendChild(row);
    }
    status.textContent='Kies een SoundCloud-nummer';
  }finally{
    document.getElementById('scsearch').disabled=false;
  }
}
async function loadSoundCloud(t,b){
  b.disabled=true;
  status.textContent='SoundCloud downloaden...';
  const u='/api/soundcloud/load?id='+encodeURIComponent(t.id)
    +'&url='+encodeURIComponent(t.url)
    +'&title='+encodeURIComponent(t.title||'')
    +'&artist='+encodeURIComponent(t.artist||'');
  const r=await fetch(u);
  const x=await r.json();
  if(!x.ok){
    status.textContent='SoundCloud laden mislukt: '+(x.error||'onbekend');
    b.disabled=false;
    return;
  }
  const key=x.key;
  for(let i=0;i<120;i++){
    await new Promise(resolve=>setTimeout(resolve,1000));
    const sr=await fetch('/api/soundcloud/status?key='+encodeURIComponent(key));
    const s=await sr.json();
    if(s.state==='done'){
      status.textContent=s.deck
        ? 'SoundCloud geladen op Deck '+s.deck
        : 'SoundCloud opgeslagen: '+(s.message||'klaar');
      b.disabled=false;
      await tracks();
      return;
    }
    if(s.state==='error'){
      status.textContent='SoundCloud laden mislukt: '+(s.error||'onbekend');
      b.disabled=false;
      return;
    }
  }
  status.textContent='SoundCloud download duurt langer; probeer Vernieuw';
  b.disabled=false;
}
let timer;
q.addEventListener('input',()=>{clearTimeout(timer);timer=setTimeout(tracks,250)});
q.addEventListener('keydown',e=>{if(e.key==='Enter')tracks()});
scq.addEventListener('keydown',e=>{if(e.key==='Enter')searchSoundCloud()});
document.getElementById('refresh').onclick=tracks;
document.getElementById('scsearch').onclick=searchSoundCloud;
tracks();
</script>
</body>
</html>)HTML");
    }

    void sendResponse(QTcpSocket* pSocket,
            const QByteArray& status,
            const QByteArray& contentType,
            const QByteArray& body) {
        QByteArray response = "HTTP/1.1 " + status + "\r\n";
        response += "Content-Type: " + contentType + "\r\n";
        response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        response += "Cache-Control: no-store\r\n";
        response += "Connection: close\r\n\r\n";
        response += body;
        pSocket->write(response);
        pSocket->disconnectFromHost();
    }

    void sendJson(QTcpSocket* pSocket,
            const QJsonDocument& document,
            const QByteArray& status = QByteArrayLiteral("200 OK")) {
        sendResponse(pSocket,
                status,
                QByteArrayLiteral("application/json; charset=utf-8"),
                document.toJson(QJsonDocument::Compact));
    }

    QSqlDatabase database() const {
        auto pLibrary = m_pCoreServices->getLibrary();
        if (!pLibrary || !pLibrary->trackCollectionManager()) {
            return {};
        }
        auto* pInternal =
                pLibrary->trackCollectionManager()->internalCollection();
        return pInternal ? pInternal->database() : QSqlDatabase();
    }

    void handleTracks(QTcpSocket* pSocket, const QUrlQuery& query) {
        QSqlDatabase db = database();
        if (!db.isValid() || !db.isOpen()) {
            sendJson(pSocket,
                    QJsonDocument(QJsonObject{
                            {QStringLiteral("error"),
                                    QStringLiteral("Library database unavailable")}}),
                    QByteArrayLiteral("503 Service Unavailable"));
            return;
        }

        const QString search = query.queryItemValue(QStringLiteral("q")).trimmed();
        QSqlQuery sql(db);
        QString statement = QStringLiteral(
                "SELECT library.id, library.title, library.artist, library.album "
                "FROM library "
                "JOIN track_locations ON track_locations.id = library.location "
                "WHERE library.mixxx_deleted = 0 "
                "AND track_locations.fs_deleted = 0 "
                "AND TRIM(COALESCE(track_locations.location, '')) <> '' "
                // Historical rescans can leave more than one active library
                // row pointing at the same unique track_locations row. The
                // browser must expose each physical file only once. Keep the
                // oldest/original library row so existing metadata/cues are
                // preferred over later duplicate rows.
                "AND library.id = ("
                "SELECT MIN(l2.id) FROM library l2 "
                "WHERE l2.location = library.location "
                "AND l2.mixxx_deleted = 0"
                ") ");
        if (!search.isEmpty()) {
            statement += QStringLiteral(
                    "AND (library.title LIKE :q OR library.artist LIKE :q "
                    "OR library.album LIKE :q) ");
        }
        statement += QStringLiteral(
                "ORDER BY library.artist COLLATE NOCASE, "
                "library.title COLLATE NOCASE LIMIT 600");
        sql.prepare(statement);
        if (!search.isEmpty()) {
            const QString searchPattern =
                    QStringLiteral("%") + search + QStringLiteral("%");
            sql.bindValue(QStringLiteral(":q"), QVariant(searchPattern));
        }

        QJsonArray rows;
        if (sql.exec()) {
            while (sql.next()) {
                QJsonObject row;
                row.insert(QStringLiteral("id"), sql.value(0).toInt());
                row.insert(QStringLiteral("title"), sql.value(1).toString());
                row.insert(QStringLiteral("artist"), sql.value(2).toString());
                row.insert(QStringLiteral("album"), sql.value(3).toString());
                rows.append(row);
            }
        }
        sendJson(pSocket, QJsonDocument(rows));
    }

    void handleLoad(QTcpSocket* pSocket, const QUrlQuery& query) {
        bool idOk = false;
        bool deckOk = false;
        const int id =
                query.queryItemValue(QStringLiteral("id")).toInt(&idOk);
        const int deck =
                query.queryItemValue(QStringLiteral("deck")).toInt(&deckOk);
        if (!idOk || !deckOk || id <= 0 || deck < 1 || deck > 2) {
            sendJson(pSocket,
                    QJsonDocument(QJsonObject{
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"),
                                    QStringLiteral("Invalid id or deck")}}),
                    QByteArrayLiteral("400 Bad Request"));
            return;
        }

        QSqlDatabase db = database();
        QSqlQuery sql(db);
        sql.prepare(QStringLiteral(
                "SELECT track_locations.location "
                "FROM library "
                "JOIN track_locations ON track_locations.id = library.location "
                "WHERE library.id = :id AND library.mixxx_deleted = 0 "
                "AND track_locations.fs_deleted = 0 LIMIT 1"));
        sql.bindValue(QStringLiteral(":id"), id);
        if (!sql.exec() || !sql.next()) {
            sendJson(pSocket,
                    QJsonDocument(QJsonObject{
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"),
                                    QStringLiteral("Track not found")}}),
                    QByteArrayLiteral("404 Not Found"));
            return;
        }

        const QString remoteTrackPath = sql.value(0).toString().trimmed();
        if (remoteTrackPath.isEmpty()) {
            sendJson(pSocket,
                    QJsonDocument(QJsonObject{
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"),
                                    QStringLiteral("Track has no file path")}}),
                    QByteArrayLiteral("409 Conflict"));
            return;
        }

        // Load the existing library track by its database id. Reconstructing a
        // TrackRef from the stored path is unreliable on Android shared/removable
        // storage because canonical-path resolution can fail even though Mixxx
        // already has a valid library track for the file.
        auto pTrackCollectionManager = m_pCoreServices->getTrackCollectionManager();
        auto pPlayerManager = m_pCoreServices->getPlayerManager();
        const TrackPointer pTrack = pTrackCollectionManager
                ? pTrackCollectionManager->getTrackById(TrackId(QVariant(id)))
                : TrackPointer();
        if (!pPlayerManager || !pTrack) {
            sendJson(pSocket,
                    QJsonDocument(QJsonObject{
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"),
                                    QStringLiteral("Track or player unavailable")}}),
                    QByteArrayLiteral("503 Service Unavailable"));
            return;
        }

#ifdef __STEM__
        pPlayerManager->slotLoadTrackToPlayer(
                pTrack,
                PlayerManager::groupForDeck(deck - 1),
                mixxx::StemChannelSelection(),
                false);
#else
        pPlayerManager->slotLoadTrackToPlayer(
                pTrack,
                PlayerManager::groupForDeck(deck - 1),
                false);
#endif
        sendJson(pSocket,
                QJsonDocument(QJsonObject{
                        {QStringLiteral("ok"), true},
                        {QStringLiteral("deck"), deck}}));
    }

    void handleSocket(QTcpSocket* pSocket) {
        QByteArray request = pSocket->property("remoteHttpBuffer").toByteArray();
        request += pSocket->readAll();
        if (!request.contains("\r\n\r\n")) {
            pSocket->setProperty("remoteHttpBuffer", request);
            return;
        }

        const int firstLineEnd = request.indexOf("\r\n");
        const QByteArray firstLine =
                firstLineEnd >= 0 ? request.left(firstLineEnd) : request;
        const QList<QByteArray> parts = firstLine.split(' ');
        if (parts.size() < 2 || parts.at(0) != QByteArrayLiteral("GET")) {
            sendResponse(pSocket,
                    QByteArrayLiteral("405 Method Not Allowed"),
                    QByteArrayLiteral("text/plain; charset=utf-8"),
                    QByteArrayLiteral("Only GET is supported"));
            return;
        }

        const QUrl requestUrl = QUrl::fromEncoded(parts.at(1));
        const QString path = requestUrl.path();
        const QUrlQuery query(requestUrl);

        if (path == QStringLiteral("/") ||
                path == QStringLiteral("/index.html")) {
            sendResponse(pSocket,
                    QByteArrayLiteral("200 OK"),
                    QByteArrayLiteral("text/html; charset=utf-8"),
                    pageHtml());
        } else if (path == QStringLiteral("/api/tracks")) {
            handleTracks(pSocket, query);
        } else if (path == QStringLiteral("/api/load")) {
            handleLoad(pSocket, query);
        } else {
            sendResponse(pSocket,
                    QByteArrayLiteral("404 Not Found"),
                    QByteArrayLiteral("text/plain; charset=utf-8"),
                    QByteArrayLiteral("Not found"));
        }
    }

    std::shared_ptr<mixxx::CoreServices> m_pCoreServices;
    quint16 m_port = 0;
};
#endif

} // namespace

MixxxMainWindow::MixxxMainWindow(std::shared_ptr<mixxx::CoreServices> pCoreServices)
        : m_pCoreServices(pCoreServices),
          m_pCentralWidget(nullptr),
          m_pLaunchImage(nullptr),
#ifndef __APPLE__
          m_prevState(Qt::WindowNoState),
#endif
          m_noVinylInputDialog(nullptr),
          m_noPassthroughInputDialog(nullptr),
          m_noMicInputDialog(nullptr),
          m_noAuxInputDialog(nullptr),
          m_pGuiTick(nullptr),
#if defined(__LINUX__) && !defined(__ANDROID__)
          m_supportsGlobalMenuBar(supportsGlobalMenu()),
#endif
          m_inRebootMixxxView(false),
          m_pDeveloperToolsDlg(nullptr),
          m_pPrefDlg(nullptr),
          m_toolTipsCfg(mixxx::preferences::Tooltips::On) {
    DEBUG_ASSERT(pCoreServices);
    // These depend on the settings
#if defined(__LINUX__) && !defined(__ANDROID__)
    // If the desktop features a global menubar and we'll go fullscreen during
    // startup, set Qt::AA_DontUseNativeMenuBar so the menubar is placed in the
    // window like it's done in slotViewFullScreen(). On other desktops this
    // attribute has no effect. This is a safe alternative to setNativeMenuBar()
    // which can cause a crash when using menu shortcuts like Alt+F after resetting
    // the menubar. See https://github.com/mixxxdj/mixxx/issues/11320
    if (m_supportsGlobalMenuBar) {
        bool fullscreenPref = m_pCoreServices->getSettings()->getValue<bool>(
                ConfigKey("[Config]", "StartInFullscreen"));
        QApplication::setAttribute(
                Qt::AA_DontUseNativeMenuBar,
                CmdlineArgs::Instance().getStartInFullscreen() || fullscreenPref);
    }
#endif // __LINUX__

    connect(m_pCoreServices.get(),
            &mixxx::CoreServices::libraryScanSummary,
            this,
            &MixxxMainWindow::slotLibraryScanSummaryDlg);

    createMenuBar();
    m_pMenuBar->hide();

    initializeWindow();

    // Show launch image immediately so the user knows Mixxx is starting
    m_pSkinLoader = std::make_unique<mixxx::skin::SkinLoader>(m_pCoreServices->getSettings());
    m_pLaunchImage = m_pSkinLoader->loadLaunchImage(this);
    m_pCentralWidget = (QWidget*)m_pLaunchImage;
    setCentralWidget(m_pCentralWidget);

    show();

    m_pGuiTick = new GuiTick();
    m_pVisualsManager = new VisualsManager();
}

#ifdef MIXXX_USE_QOPENGL
void MixxxMainWindow::initializeQOpenGL() {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Qt 6 will nno longer crash if no GL is available and
    // QGLFormat::hasOpenGL() has been removed.
    if (!CmdlineArgs::Instance().getSafeMode() && QGLFormat::hasOpenGL()) {
#else
    // With EGLFS there is always exactly one native window and one EGL window surface
    // OpenGL windows cannot be embedded into our QWidgets main window we already have.
    // https://doc.qt.io/qt-6/embedded-linux.html
    bool isEglfs = QGuiApplication::platformName() == "eglfs";

    if (!CmdlineArgs::Instance().getSafeMode() && !isEglfs) {
#endif
        QOpenGLContext context;
        context.setFormat(WaveformWidgetFactory::getSurfaceFormat(m_pCoreServices->getSettings()));
        if (context.create()) {
            std::pair version = context.format().version();
            qDebug().noquote()
                    << "QOpenGLContext created:"
                    << QGuiApplication::platformName()
                    << context.format().renderableType()
                    << QString("V%1.%2").arg(QString::number(version.first),
                               QString::number(version.second))
                    << context.format().profile();
            // This widget and its QOpenGLWindow will be used to query QOpenGL
            // information (version, driver, etc) in WaveformWidgetFactory.
            // The "SharedGLContext" terminology here doesn't really apply,
            // but allows us to take advantage of the existing classes.
            auto pWidget = make_parented<WInitialGLWidget>(this);
            pWidget->setGeometry(QRect(0, 0, 3, 3));
            SharedGLContext::setWidget(pWidget);
            // When the widget's QOpenGLWindow has been initialized, we continue
            // with the actual initialization
            connect(pWidget, &WInitialGLWidget::onInitialized, this, &MixxxMainWindow::initialize);
            pWidget->show();
            return;
        }
        qDebug() << "QOpenGLContext::create() failed";
    }
    qInfo() << "Initializing without OpenGL";
    initialize();
}
#endif

void MixxxMainWindow::initialize() {
    m_pCoreServices->getControlIndicatorTimer()->setLegacyVsyncEnabled(true);

    UserSettingsPointer pConfig = m_pCoreServices->getSettings();

#ifdef Q_OS_ANDROID
    // DJ Sugar phone defaults: expose LateNight's built-in stacked deck
    // waveforms so Deck 1 and Deck 2 can be beat-matched visually. Apply once,
    // then leave the user's later choice alone.
    const ConfigKey kAndroidVisualDefaults(
            QStringLiteral("[DJ-Sugar-Android]"),
            QStringLiteral("visual_defaults_v2"));
    if (pConfig->getValueString(kAndroidVisualDefaults) != QStringLiteral("1")) {
        pConfig->setValue(ConfigKey(QStringLiteral("[Skin]"),
                                  QStringLiteral("show_waveforms")),
                1);
        // Give the two stacked waveforms roughly twice the old default height:
        // ~110 px per deck instead of ~50 px, while retaining room for decks.
        pConfig->setValue(ConfigKey(QStringLiteral("[Skin]"),
                                  QStringLiteral("stackedWaveforms_splitSize")),
                QStringLiteral("220,430"));
        pConfig->setValue(kAndroidVisualDefaults, QStringLiteral("1"));
    }
#endif


#ifdef Q_OS_ANDROID
    // One comprehensive phone-readability default pass. The library has its
    // own runtime font/row-height settings, so skin QSS alone cannot make the
    // song list reliably larger. Apply and persist these defaults once.
    const ConfigKey kAndroidReadabilityDefaults(
            QStringLiteral("[DJ-Sugar-Android]"),
            QStringLiteral("readability_defaults_v5"));
    if (pConfig->getValueString(kAndroidReadabilityDefaults) != QStringLiteral("1")) {
        QFont libraryFont = QApplication::font();
        if (libraryFont.pointSizeF() > 0.0) {
            libraryFont.setPointSizeF(
                    qMax(19.0, libraryFont.pointSizeF() * 1.60));
            libraryFont.setWeight(QFont::Medium);
        } else {
            const int currentPixels = libraryFont.pixelSize() > 0
                    ? libraryFont.pixelSize()
                    : 14;
            libraryFont.setPixelSize(
                    qMax(26, static_cast<int>(currentPixels * 1.60)));
            libraryFont.setWeight(QFont::Medium);
        }

        const int libraryRowHeight =
                qMax(52, QFontMetrics(libraryFont).height() + 14);
        auto pLibrary = m_pCoreServices->getLibrary();
        if (pLibrary) {
            pLibrary->setFont(libraryFont);
            pLibrary->setRowHeight(libraryRowHeight);
        }

        // Persist so every Library/YouTube/playlist table, including ones
        // created later in the session, receives the same readable sizing.
        pConfig->setValue(
                ConfigKey(QStringLiteral("[Library]"), QStringLiteral("Font")),
                libraryFont.toString());
        pConfig->setValue(
                ConfigKey(QStringLiteral("[Library]"), QStringLiteral("RowHeight")),
                libraryRowHeight);
        pConfig->setValue(kAndroidReadabilityDefaults, QStringLiteral("1"));
    }
#endif

#ifdef Q_OS_ANDROID
    // Make local-phone music usable without a first-run scavenger hunt through
    // Preferences. Add only the normal Music and Download folders; do not scan
    // all of /storage because large USB/SD volumes can hold tens of thousands
    // of non-audio files. USB folders can still be added explicitly.
    const ConfigKey kAndroidLocalDirs(
            QStringLiteral("[DJ-Sugar-Android]"),
            QStringLiteral("local_dirs_v1"));
    if (pConfig->getValueString(kAndroidLocalDirs) != QStringLiteral("1")) {
        const QStringList commonMusicDirs = {
                QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
                QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        };
        auto pLibrary = m_pCoreServices->getLibrary();
        bool storageIsReadable = false;
        if (pLibrary) {
            for (const QString& path : commonMusicDirs) {
                if (path.isEmpty()) {
                    continue;
                }
                const QFileInfo info(path);
                if (!info.exists() || !info.isDir() || !info.isReadable()) {
                    continue;
                }
                storageIsReadable = true;
                pLibrary->requestAddDir(path, /*silent=*/true);
            }
        }
        if (storageIsReadable) {
            pConfig->setValue(kAndroidLocalDirs, QStringLiteral("1"));
        }
    }
#endif
    // Set the visibility of tooltips, default "1" = ON
    m_toolTipsCfg = pConfig->getValue(
            ConfigKey("[Controls]", "Tooltips"),
            mixxx::preferences::Tooltips::On);
#ifdef MIXXX_USE_QOPENGL
    ToolTipQOpenGL::singleton().setActive(
            m_toolTipsCfg == mixxx::preferences::Tooltips::On);
#endif

#ifdef __ENGINEPRIME__
    // Initialise library exporter
    // This has to be done before switching to fullscreen
    m_pLibraryExporter = m_pCoreServices->getLibrary()->makeLibraryExporter(this);
    connect(m_pCoreServices->getLibrary().get(),
            &Library::exportLibrary,
            m_pLibraryExporter.get(),
            &mixxx::LibraryExporter::slotRequestExport);
    connect(m_pCoreServices->getLibrary().get(),
            &Library::exportCrate,
            m_pLibraryExporter.get(),
            &mixxx::LibraryExporter::slotRequestExportWithInitialCrate);
    connect(m_pCoreServices->getLibrary().get(),
            &Library::exportPlaylist,
            m_pLibraryExporter.get(),
            &mixxx::LibraryExporter::slotRequestExportWithInitialPlaylist);
#endif

    // Turn on fullscreen mode
    // if we were told to start in fullscreen mode on the command-line
    // or if the user chose to always start in fullscreen mode.
    // The Fullscreen menu item is refreshed in connectMenuBar()
    bool fullscreenPref = m_pCoreServices->getSettings()->getValue<bool>(
            ConfigKey("[Config]", "StartInFullscreen"));
    if ((CmdlineArgs::Instance().getStartInFullscreen() || fullscreenPref) &&
            // could be we're fullscreen already after setGeomtery(previousGeometry)
            !isFullScreen()) {
        showFullScreen();
    }

    initializationProgressUpdate(65, tr("skin"));

    // Install an event filter to catch certain QT events, such as tooltips.
    // This allows us to turn off tooltips.
    installEventFilter(m_pCoreServices->getKeyboardEventFilter().get());

    auto pPlayerManager = m_pCoreServices->getPlayerManager();
    DEBUG_ASSERT(pPlayerManager);
    const QStringList visualGroups = pPlayerManager->getVisualPlayerGroups();
    for (const QString& group : visualGroups) {
        m_pVisualsManager->addDeck(group);
    }
    connect(pPlayerManager.get(),
            &PlayerManagerInterface::numberOfDecksChanged,
            this,
            [this](int decks) {
                for (int i = 0; i < decks; ++i) {
                    QString group = PlayerManager::groupForDeck(i);
                    m_pVisualsManager->addDeckIfNotExist(group);
                }
            });
    connect(pPlayerManager.get(),
            &PlayerManagerInterface::numberOfSamplersChanged,
            this,
            [this](int decks) {
                for (int i = 0; i < decks; ++i) {
                    QString group = PlayerManager::groupForSampler(i);
                    m_pVisualsManager->addDeckIfNotExist(group);
                }
            });

#if !defined(MIXXX_USE_QOPENGL) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Before creating the first skin we need to create a QGLWidget so that all
    // the QGLWidget's we create can use it as a shared QGLContext.
    // QGLFormat/QGLWidget were removed in Qt6.
    if (!CmdlineArgs::Instance().getSafeMode() && QGLFormat::hasOpenGL()) {
        QGLFormat glFormat;
        glFormat.setDirectRendering(true);
        glFormat.setDoubleBuffer(true);
        glFormat.setDepth(false);
        // Disable waiting for vertical Sync
        // This can be enabled when using a single Threads for each QGLContext
        // Setting 1 causes QGLContext::swapBuffer to sleep until the next VSync
#if defined(__APPLE__)
        // On OS X, syncing to vsync has good performance FPS-wise and
        // eliminates tearing.
        glFormat.setSwapInterval(1);
#else
        // Otherwise, turn VSync off because it could cause horrible FPS on
        // Linux.
        // TODO(XXX): Make this configurable.
        // TODO(XXX): What should we do on Windows?
        glFormat.setSwapInterval(0);
#endif
        glFormat.setRgba(true);
        QGLFormat::setDefaultFormat(glFormat);

        WGLWidget* pContextWidget = new WGLWidget(this);
        pContextWidget->setGeometry(QRect(0, 0, 3, 3));
        pContextWidget->hide();
        SharedGLContext::setWidget(pContextWidget);
    }
#endif

    WaveformWidgetFactory::createInstance(); // takes a long time
    WaveformWidgetFactory::instance()->setConfig(m_pCoreServices->getSettings());
    WaveformWidgetFactory::instance()->startVSync(m_pGuiTick, m_pVisualsManager, false);

    connect(this,
            &MixxxMainWindow::skinLoaded,
            m_pCoreServices->getLibrary().get(),
            &Library::onSkinLoadFinished);

    connect(this,
            &MixxxMainWindow::skinLoaded,
            WaveformWidgetFactory::instance(),
            &WaveformWidgetFactory::slotSkinLoaded);

    // Initialize preference dialog
    m_pPrefDlg = new DlgPreferences(
            m_pCoreServices->getScreensaverManager(),
            m_pSkinLoader,
            m_pCoreServices->getSoundManager(),
            m_pCoreServices->getControllerManager(),
            m_pCoreServices->getVinylControlManager(),
            m_pCoreServices->getEffectsManager(),
            m_pCoreServices->getSettingsManager(),
            m_pCoreServices->getLibrary());
    m_pPrefDlg->setWindowIcon(QIcon(MIXXX_ICON_PATH));
    m_pPrefDlg->setHidden(true);
    connect(m_pPrefDlg,
            &DlgPreferences::tooltipModeChanged,
            this,
            &MixxxMainWindow::slotTooltipModeChanged);
    connect(m_pPrefDlg,
            &DlgPreferences::reloadUserInterface,
            this,
            &MixxxMainWindow::rebootMixxxView,
            Qt::DirectConnection);
#ifndef __APPLE__
    connect(m_pPrefDlg,
            &DlgPreferences::menuBarAutoHideChanged,
            this,
            &MixxxMainWindow::slotUpdateMenuBarAltKeyConnection,
            Qt::DirectConnection);
#endif

    // Connect signals to the menubar. Should be done before emit skinLoaded.
    connectMenuBar();

    QWidget* oldWidget = m_pCentralWidget;

    tryParseAndSetDefaultStyleSheet();

    if (!loadConfiguredSkin()) {
        reportCriticalErrorAndQuit(
                "default skin cannot be loaded - see <b>mixxx</b> trace for more information");
        m_pCentralWidget = oldWidget;
        // TODO (XXX) add dialog to warn user and launch skin choice page
    } else {
        m_pMenuBar->setStyleSheet(m_pCentralWidget->styleSheet());
    }

    // Check direct rendering and warn user if they don't have it
    if (!CmdlineArgs::Instance().getSafeMode()) {
        checkDirectRendering();
    }

    // Sound hardware setup
    // Try to open configured devices. If that fails, display dialogs
    // that allow to either retry, reconfigure devices or exit.
    bool retryClicked;
    do {
        retryClicked = false;
        SoundDeviceStatus result = m_pCoreServices->getSoundManager()->setupDevices();
        if (result == SoundDeviceStatus::ErrorDeviceCount ||
                result == SoundDeviceStatus::ErrorExcessiveOutputChannel) {
            if (soundDeviceBusyDlg(&retryClicked) != QDialog::Accepted) {
                exit(0);
            }
        } else if (result != SoundDeviceStatus::Ok) {
            if (soundDeviceErrorMsgDlg(result, &retryClicked) !=
                    QDialog::Accepted) {
                exit(0);
            }
        }
    } while (retryClicked);

    // Test for at least one output device. If none, display another dialog
    // that says "mixxx will barely work with no outs".
    // In case of persisting errors, the user has already received a message
    // above. So we can just check the output count here.
    while (m_pCoreServices->getSoundManager()->getConfig().getOutputs().isEmpty()) {
        // Exit when we press the Exit button in the noSoundDlg dialog
        // only call it if result != OK
        bool continueClicked = false;
        if (noOutputDlg(&continueClicked) != QDialog::Accepted) {
            exit(0);
        }
        if (continueClicked) {
            break;
        }
    }

    // The user has either reconfigured devices or accepted no outputs,
    // so it's now safe to write the new config to disk.
    m_pCoreServices->getSoundManager()->getConfig().writeToDisk();

    // this has to be after the OpenGL widgets are created or depending on a
    // million different variables the first waveform may be horribly
    // corrupted. See bug 521509 -- bkgood ?? -- vrince
    setCentralWidget(m_pCentralWidget);

    // "Up Next" suggestions strip lives in the QStatusBar at the bottom of
    // the main window, so it's visible regardless of which library view the
    // user is currently in (mirrors the QML-mode SuggestionsStrip). Hidden by
    // default if the user prefers a clean LateNight look — the menu toggle
    // (View → Show Suggestions Bar) is added in MixxxMainWindow::createMenu.
    if (!m_pSuggestionsBar) {
        m_pSuggestionsBar = new WSuggestionsBar(m_pCoreServices, this);
        statusBar()->addPermanentWidget(m_pSuggestionsBar, /*stretch=*/1);
        statusBar()->setSizeGripEnabled(false);
        statusBar()->setStyleSheet(QStringLiteral(
                "QStatusBar { background-color: #000000; border: 0; } "
                "QStatusBar::item { border: 0; }"));
    }

#ifdef Q_OS_ANDROID
    // Local-network remote library. A second phone, tablet or computer on the
    // same Wi-Fi/hotspot can browse the phone's Mixxx library and load tracks
    // onto Deck 1 or Deck 2 without needing its own copy of the music files.
    auto* pRemoteLibraryServer =
            new AndroidRemoteLibraryServer(m_pCoreServices, this);
    m_pRemoteLibraryButton = new QPushButton(tr("REMOTE"), m_pCentralWidget);
    m_pRemoteLibraryButton->setFixedSize(116, 42);
    m_pRemoteLibraryButton->setStyleSheet(QStringLiteral(
            "QPushButton { background:#1d2328; color:#ffffff; "
            "border:2px solid #8a949d; border-radius:5px; padding:5px 12px; "
            "font-size:17px; font-weight:700; } "
            "QPushButton:pressed { background:#33414d; }"));
    m_pRemoteLibraryButton->move(
            qMax(8, m_pCentralWidget->width() - m_pRemoteLibraryButton->width() - 12),
            12);
    m_pRemoteLibraryButton->raise();
    m_pRemoteLibraryButton->show();
    m_pCentralWidget->installEventFilter(this);

    if (pRemoteLibraryServer->start()) {
        connect(m_pRemoteLibraryButton,
                &QPushButton::clicked,
                this,
                [this, pRemoteLibraryServer]() {
                    const QStringList urls = pRemoteLibraryServer->urls();
                    const QString message = urls.isEmpty()
                            ? tr("De remote bibliotheek draait, maar er is nog "
                                 "geen lokaal IPv4-adres gevonden. Verbind beide "
                                 "apparaten met dezelfde Wi-Fi of hotspot en "
                                 "probeer opnieuw.")
                            : tr("Open op je tweede telefoon of computer:\n\n%1")
                                      .arg(urls.join(QLatin1Char('\n')));
                    QMessageBox::information(
                            this, tr("DJ Sugar Remote Bibliotheek"), message);
                });
    } else {
        m_pRemoteLibraryButton->setEnabled(false);
        m_pRemoteLibraryButton->setToolTip(
                tr("Remote bibliotheek kon niet worden gestart."));
    }
#endif

#ifndef __APPLE__
    // Ask for permission to auto-hide the menu bar if applicable.
#if defined(__LINUX__) && !defined(__ANDROID__)
    // This makes no sense when starting in windowed mode with a global menu,
    // we'll ask when going fullscreen.
    if (!m_supportsGlobalMenuBar || isFullScreen()) {
        alwaysHideMenuBarDlg();
        slotUpdateMenuBarAltKeyConnection();
    }
#else
#ifndef __ANDROID__
    alwaysHideMenuBarDlg();
    slotUpdateMenuBarAltKeyConnection();
#endif
#endif
#endif

    // Show the menubar after the launch image is replaced by the skin widget,
    // otherwise it would shift the launch image shortly before the skin is visible.
    m_pMenuBar->show();

    // The launch image widget is automatically disposed, but we still have a
    // pointer to it.
    m_pLaunchImage = nullptr;

    connect(pPlayerManager.get(),
            &PlayerManager::noMicrophoneInputConfigured,
            this,
            &MixxxMainWindow::slotNoMicrophoneInputConfigured);
    connect(pPlayerManager.get(),
            &PlayerManager::noAuxiliaryInputConfigured,
            this,
            &MixxxMainWindow::slotNoAuxiliaryInputConfigured);
    connect(pPlayerManager.get(),
            &PlayerManager::noDeckPassthroughInputConfigured,
            this,
            &MixxxMainWindow::slotNoDeckPassthroughInputConfigured);
    connect(pPlayerManager.get(),
            &PlayerManager::noVinylControlInputConfigured,
            this,
            &MixxxMainWindow::slotNoVinylControlInputConfigured);

    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            &MixxxMainWindow::slotUpdateWindowTitle);

    // Start Auto DJ if the cmdline arg is passed.
    if (CmdlineArgs::Instance().getStartAutoDJ()) {
        qDebug("Enabling Auto DJ from CLI flag.");
        ControlObject::set(ConfigKey("[AutoDJ]", "enabled"), 1.0);
        // Switch to Auto DJ feature
        auto* pLibrary = m_pCoreServices->getLibrary().get();
        // Note: auto-scroll is disabled but that doesn't really matter here
        // because the sidebar is still in its initial state (top feature visible,
        // AutoDj is second from the top by default, all features collapsed).
        pLibrary->showAutoDJ();
    }
}

MixxxMainWindow::~MixxxMainWindow() {
    Timer t("~MixxxMainWindow");
    t.start();

    // Save the current window state (position, maximized, etc)
    // Note(ronso0): Unfortunately saveGeometry() also stores the fullscreen state.
    // On next start restoreGeometry would enable fullscreen mode even though that
    // might not be requested (no '--fullscreen' command line arg and
    // [Config],StartInFullscreen is '0'.
    // https://github.com/mixxxdj/mixxx/issues/10005
    // So let's quit fullscreen if StartInFullscreen is not checked in Preferences.
    bool fullscreenPref = m_pCoreServices->getSettings()->getValue<bool>(
            ConfigKey("[Config]", "StartInFullscreen"));
    if (isFullScreen() && !fullscreenPref) {
        // Simply maximize the window so we can store a geometry that fits the screen.
        // Don't call slotViewFullScreen(false) (calls showNormal()) because that
        // can make the main window incl. window decoration too large for the screen.
#ifndef __APPLE__
        // Before, store the expected window state so eventFilter() will ignore
        // the following QWindowChangeEvent and not recreate & re-sync the menu bar.
        m_prevState = Qt::WindowMaximized;
#endif
        showMaximized();
    }
    m_pCoreServices->getSettings()->set(ConfigKey("[MainWindow]", "geometry"),
            QString(saveGeometry().toBase64()));
    m_pCoreServices->getSettings()->set(ConfigKey("[MainWindow]", "state"),
            QString(saveState().toBase64()));

    // GUI depends on KeyboardEventFilter, PlayerManager, Library
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting skin";
    // Clear widget pointer list and destroy all update connections before we
    // delete the main widget (ie. all WBaseWidgets) to prevent KeyboardEventFilter
    // accessing dangling pointers.
    m_pCoreServices->getKeyboardEventFilter()->clearWidgets();
    m_pCentralWidget = nullptr;
    QPointer<QWidget> pSkin(centralWidget());
    setCentralWidget(nullptr);
    if (!pSkin.isNull()) {
        QCoreApplication::sendPostedEvents(pSkin, QEvent::DeferredDelete);
    }
    // Our central widget is now deleted.
    VERIFY_OR_DEBUG_ASSERT(pSkin.isNull()) {
        qWarning() << "Central widget was not deleted by our sendPostedEvents trick.";
    }

    // Delete Controls created by skins
    qDeleteAll(m_skinCreatedControls);
    m_skinCreatedControls.clear();

    // TODO() Verify if this comment still applies:
    // WMainMenuBar holds references to controls so we need to delete it
    // before MixxxMainWindow is destroyed. QMainWindow calls deleteLater() in
    // setMenuBar() but we need to delete it now so we can ask for
    // DeferredDelete events to be processed for it. Once Mixxx shutdown lives
    // outside of MixxxMainWindow the parent relationship will directly destroy
    // the WMainMenuBar and this will no longer be a problem.
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting menubar";
    // Clear action pointer list before we delete the menubar
    // to prevent KeyboardEventFilter accessing dangling pointers.
    m_pCoreServices->getKeyboardEventFilter()->clearMenuBarActions();
    QPointer<WMainMenuBar> pMenuBar = m_pMenuBar.toWeakRef();
    DEBUG_ASSERT(menuBar() == m_pMenuBar.get());
    // We need to reset the parented pointer here that it does not become a
    // dangling pointer after the object has been deleted.
    m_pMenuBar = nullptr;
    setMenuBar(nullptr);
    if (!pMenuBar.isNull()) {
        QCoreApplication::sendPostedEvents(pMenuBar, QEvent::DeferredDelete);
    }
    // Our main menu is now deleted.
    VERIFY_OR_DEBUG_ASSERT(pMenuBar.isNull()) {
        qWarning() << "WMainMenuBar was not deleted by our sendPostedEvents trick.";
    }

    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting DeveloperToolsDlg";
    delete m_pDeveloperToolsDlg;

#ifdef __ENGINEPRIME__
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting LibraryExporter";
    m_pLibraryExporter.reset();
#endif

    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting DlgPreferences";
    delete m_pPrefDlg;

    m_pCoreServices->getControlIndicatorTimer()->setLegacyVsyncEnabled(false);

    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting ControllerManager";

    WaveformWidgetFactory::destroy();

    delete m_pGuiTick;
    delete m_pVisualsManager;
}

void MixxxMainWindow::initializeWindow() {
    // be sure createMenuBar() is called first
    DEBUG_ASSERT(m_pMenuBar);

    QPalette Pal(palette());
    // safe default QMenuBar background
    QColor MenuBarBackground(m_pMenuBar->palette().color(QPalette::Window));
    Pal.setColor(QPalette::Window, QColor(0x202020));
    setAutoFillBackground(true);
    setPalette(Pal);
    // restore default QMenuBar background
    Pal.setColor(QPalette::Window, MenuBarBackground);
    m_pMenuBar->setPalette(Pal);

    // Restore the current window state (position, maximized, etc).
    // This will also restore fullscreen and thereby create a seamless
    // start if we did shut down while in fullscreen mode and with
    // [Config],StartInFullscreen = 1
    // (slotViewFullScreen(true) in  initialize() is a no-op then)
    restoreGeometry(QByteArray::fromBase64(
            m_pCoreServices->getSettings()
                    ->getValueString(ConfigKey("[MainWindow]", "geometry"))
                    .toUtf8()));
    restoreState(QByteArray::fromBase64(
            m_pCoreServices->getSettings()
                    ->getValueString(ConfigKey("[MainWindow]", "state"))
                    .toUtf8()));

    setWindowIcon(QIcon(MIXXX_ICON_PATH));
    slotUpdateWindowTitle(TrackPointer());
}

#ifndef __APPLE__
void MixxxMainWindow::alwaysHideMenuBarDlg() {
    // Don't show the dialog if the user unchecked "Ask me again"
    if (!m_pCoreServices->getSettings()->getValue<bool>(
                kMenuBarHintConfigKey, true)) {
        return;
    }
    QString title = tr("Allow Mixxx to hide the menu bar?");
    //: Always show the menu bar?
    QString hideBtnLabel = tr("Hide");
    QString showBtnLabel = tr("Always show");
    //: Keep formatting tags <b> (bold text) and <br> (linebreak).
    //: %1 is the placeholder for the 'Always show' button label
    QString desc = tr(
            "The Mixxx menu bar is hidden and can be toggled with a single press "
            "of the <b>Alt</b> key.<br><br>"
            "Click <b>%1</b> to agree.<br><br>"
            "Click <b>%2</b> to disable that, for example if you don't use Mixxx "
            "with a keyboard.<br><br>"
            "You can change this setting any time in Preferences -> Interface."
            "<br>") // line break for some extra margin to the checkbox
                           .arg(hideBtnLabel, showBtnLabel);

    QMessageBox msg;
    msg.setIcon(QMessageBox::Question);
    msg.setWindowTitle(title);
    msg.setText(desc);
    QCheckBox askAgainCheckBox;
    askAgainCheckBox.setText(tr("Ask me again"));
    askAgainCheckBox.setCheckState(Qt::Checked);
    msg.setCheckBox(&askAgainCheckBox);
    QPushButton* pHideBtn = msg.addButton(hideBtnLabel, QMessageBox::AcceptRole);
    QPushButton* pShowBtn = msg.addButton(showBtnLabel, QMessageBox::RejectRole);
    msg.setDefaultButton(pShowBtn);
    msg.exec();

    m_pCoreServices->getSettings()->setValue(
            kMenuBarHintConfigKey,
            askAgainCheckBox.checkState() == Qt::Checked ? 1 : 0);

    m_pCoreServices->getSettings()->setValue(
            kHideMenuBarConfigKey,
            msg.clickedButton() == pHideBtn ? 1 : 0);
}
#endif

QDialog::DialogCode MixxxMainWindow::soundDeviceErrorDlg(
        const QString& title, const QString& text, bool* retryClicked) {
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle(title);
    msgBox.setText(text);

    QPushButton* retryButton =
            msgBox.addButton(tr("Retry"), QMessageBox::ActionRole);
    QPushButton* reconfigureButton =
            msgBox.addButton(tr("Reconfigure"), QMessageBox::ActionRole);
    QPushButton* wikiButton =
            msgBox.addButton(tr("Help"), QMessageBox::ActionRole);
    QPushButton* exitButton =
            msgBox.addButton(tr("Exit"), QMessageBox::ActionRole);

    while (true) {
        msgBox.exec();

        if (msgBox.clickedButton() == retryButton) {
            m_pCoreServices->getSoundManager()->clearAndQueryDevices();
            *retryClicked = true;
            return QDialog::Accepted;
        } else if (msgBox.clickedButton() == wikiButton) {
            mixxx::DesktopHelper::openUrl(QUrl(MIXXX_WIKI_TROUBLESHOOTING_SOUND_URL));
            wikiButton->setEnabled(false);
        } else if (msgBox.clickedButton() == reconfigureButton) {
            msgBox.hide();

            m_pCoreServices->getSoundManager()->clearAndQueryDevices();
            // This way of opening the dialog allows us to use it synchronously
            m_pPrefDlg->setWindowModality(Qt::ApplicationModal);
            // Open preferences, sound hardware page is selected (default on first call)
            m_pPrefDlg->exec();
            if (m_pPrefDlg->result() == QDialog::Accepted) {
                return QDialog::Accepted;
            }

            msgBox.show();
        } else if (msgBox.clickedButton() == exitButton) {
            // Will finally quit Mixxx
            return QDialog::Rejected;
        }
    }
}

QDialog::DialogCode MixxxMainWindow::soundDeviceBusyDlg(bool* retryClicked) {
    QString title(tr("Sound Device Busy"));
    QString text(
            "<html> <p>" %
                    tr("Mixxx was unable to open all the configured sound devices.") +
            "</p> <p>" %
                    m_pCoreServices->getSoundManager()->getErrorDeviceName() %
                    " is used by another application or not plugged in."
                    "</p><ul>"
                    "<li>" %
                    tr("<b>Retry</b> after closing the other application "
                       "or reconnecting a sound device") %
                    "</li>"
                    "<li>" %
                    tr("<b>Reconfigure</b> Mixxx's sound device settings.") %
                    "</li>"
                    "<li>" %
                    tr("Get <b>Help</b> from the Mixxx Wiki.") %
                    "</li>"
                    "<li>" %
                    tr("<b>Exit</b> Mixxx.") %
                    "</li>"
                    "</ul></html>");
    return soundDeviceErrorDlg(title, text, retryClicked);
}

QDialog::DialogCode MixxxMainWindow::soundDeviceErrorMsgDlg(
        SoundDeviceStatus status, bool* retryClicked) {
    QString title(tr("Sound Device Error"));
    QString text("<html> <p>" %
                    tr("Mixxx was unable to open all the configured sound "
                       "devices.") +
            "</p> <p>" %
                    m_pCoreServices->getSoundManager()
                            ->getLastErrorMessage(status)
                            .replace("\n", "<br/>") %
                    "</p><ul>"
                    "<li>" %
                    tr("<b>Retry</b> after fixing an issue") %
                    "</li>"
                    "<li>" %
                    tr("<b>Reconfigure</b> Mixxx's sound device settings.") %
                    "</li>"
                    "<li>" %
                    tr("Get <b>Help</b> from the Mixxx Wiki.") %
                    "</li>"
                    "<li>" %
                    tr("<b>Exit</b> Mixxx.") %
                    "</li>"
                    "</ul></html>");
    return soundDeviceErrorDlg(title, text, retryClicked);
}

QDialog::DialogCode MixxxMainWindow::noOutputDlg(bool* continueClicked) {
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle(tr("No Output Devices"));
    msgBox.setText(
            "<html>" + tr("Mixxx was configured without any output sound devices. "
                          "Audio processing will be disabled without a configured output device.") +
            "<ul>"
            "<li>" +
            tr("<b>Continue</b> without any outputs.") +
            "</li>"
            "<li>" +
            tr("<b>Reconfigure</b> Mixxx's sound device settings.") +
            "</li>"
            "<li>" +
            tr("<b>Exit</b> Mixxx.") +
            "</li>"
            "</ul></html>");

    QPushButton* continueButton =
            msgBox.addButton(tr("Continue"), QMessageBox::ActionRole);
    QPushButton* reconfigureButton =
            msgBox.addButton(tr("Reconfigure"), QMessageBox::ActionRole);
    QPushButton* exitButton =
            msgBox.addButton(tr("Exit"), QMessageBox::ActionRole);

    while (true) {
        msgBox.exec();

        if (msgBox.clickedButton() == continueButton) {
            *continueClicked = true;
            return QDialog::Accepted;
        } else if (msgBox.clickedButton() == reconfigureButton) {
            msgBox.hide();

            // This way of opening the dialog allows us to use it synchronously
            m_pPrefDlg->setWindowModality(Qt::ApplicationModal);
            m_pPrefDlg->showSoundHardwarePage(mixxx::preferences::SoundHardwareTab::Output);
            m_pPrefDlg->exec();
            if (m_pPrefDlg->result() == QDialog::Accepted) {
                return QDialog::Accepted;
            }

            msgBox.show();

        } else if (msgBox.clickedButton() == exitButton) {
            // Will finally quit Mixxx
            return QDialog::Rejected;
        }
    }
}

void MixxxMainWindow::slotUpdateWindowTitle(TrackPointer pTrack) {
    QString appTitle = VersionStore::applicationName();
    QString filePath;

    // If we have a track, use getInfo() to format a summary string and prepend
    // it to the title.
    // TODO(rryan): Does this violate Mac App Store policies?
    if (pTrack) {
        QString trackInfo = pTrack->getInfo();
        if (!trackInfo.isEmpty()) {
            appTitle = QString("%1 | %2").arg(trackInfo, appTitle);
        }
        filePath = pTrack->getLocation();
    }
    setWindowTitle(appTitle);

    // Display a draggable proxy icon for the track in the title bar on
    // platforms that support it, e.g. macOS
    setWindowFilePath(filePath);
}

void MixxxMainWindow::createMenuBar() {
    ScopedTimer t(QStringLiteral("MixxxMainWindow::createMenuBar"));
    DEBUG_ASSERT(m_pCoreServices->getKeyboardEventFilter());
    m_pMenuBar = make_parented<WMainMenuBar>(
            this, m_pCoreServices->getSettings(), m_pCoreServices->getKeyboardEventFilter());
    if (m_pCentralWidget) {
        m_pMenuBar->setStyleSheet(m_pCentralWidget->styleSheet());
    }
    setMenuBar(m_pMenuBar);
}

void MixxxMainWindow::connectMenuBar() {
    // This function might be invoked multiple times on startup
    // so all connections must be unique!

    ScopedTimer t(QStringLiteral("MixxxMainWindow::connectMenuBar"));
    connect(this,
            &MixxxMainWindow::skinLoaded,
            m_pMenuBar,
            &WMainMenuBar::onNewSkinLoaded,
            Qt::UniqueConnection);

    // Misc
    connect(m_pMenuBar,
            &WMainMenuBar::quit,
            this,
            &MixxxMainWindow::close,
            Qt::UniqueConnection);
    connect(m_pMenuBar,
            &WMainMenuBar::showPreferences,
            this,
            &MixxxMainWindow::slotOptionsPreferences,
            Qt::UniqueConnection);
    connect(m_pMenuBar,
            &WMainMenuBar::loadTrackToDeck,
            this,
            &MixxxMainWindow::slotFileLoadSongPlayer,
            Qt::UniqueConnection);

    connect(m_pMenuBar,
            &WMainMenuBar::showKeywheel,
            this,
            &MixxxMainWindow::slotShowKeywheel,
            Qt::UniqueConnection);
#ifndef __APPLE__
    // Menubar auto-hide
    connect(m_pMenuBar,
            &WMainMenuBar::menubarAutoHideChanged,
            this,
            &MixxxMainWindow::slotUpdateMenuBarAltKeyConnection,
            Qt::UniqueConnection);
#endif

    // Fullscreen
    connect(m_pMenuBar,
            &WMainMenuBar::toggleFullScreen,
            this,
            &MixxxMainWindow::slotViewFullScreen,
            Qt::UniqueConnection);
    connect(this,
            &MixxxMainWindow::fullScreenChanged,
            m_pMenuBar,
            &WMainMenuBar::onFullScreenStateChange,
            Qt::UniqueConnection);
    // Refresh the Fullscreen checkbox for the case we went fullscreen earlier
    m_pMenuBar->onFullScreenStateChange(isFullScreen());

    // Help
    connect(m_pMenuBar,
            &WMainMenuBar::showAbout,
            this,
            &MixxxMainWindow::slotHelpAbout,
            Qt::UniqueConnection);

    // Developer
    connect(m_pMenuBar,
            &WMainMenuBar::reloadSkin,
            this,
            &MixxxMainWindow::rebootMixxxView,
            Qt::UniqueConnection);
    connect(m_pMenuBar,
            &WMainMenuBar::toggleDeveloperTools,
            this,
            &MixxxMainWindow::slotDeveloperTools,
            Qt::UniqueConnection);

    if (m_pCoreServices->getRecordingManager()) {
        connect(m_pCoreServices->getRecordingManager().get(),
                &RecordingManager::isRecording,
                m_pMenuBar,
                &WMainMenuBar::onRecordingStateChange,
                Qt::UniqueConnection);
        connect(m_pMenuBar,
                &WMainMenuBar::toggleRecording,
                m_pCoreServices->getRecordingManager().get(),
                &RecordingManager::slotSetRecording,
                Qt::UniqueConnection);
        m_pMenuBar->onRecordingStateChange(
                m_pCoreServices->getRecordingManager()->isRecordingActive());
    }

#ifdef __BROADCAST__
    if (m_pCoreServices->getBroadcastManager()) {
        connect(m_pCoreServices->getBroadcastManager().get(),
                &BroadcastManager::broadcastEnabled,
                m_pMenuBar,
                &WMainMenuBar::onBroadcastingStateChange,
                Qt::UniqueConnection);
        connect(m_pMenuBar,
                &WMainMenuBar::toggleBroadcasting,
                m_pCoreServices->getBroadcastManager().get(),
                &BroadcastManager::setEnabled,
                Qt::UniqueConnection);
        m_pMenuBar->onBroadcastingStateChange(m_pCoreServices->getBroadcastManager()->isEnabled());
    }
#endif

#ifdef __VINYLCONTROL__
    if (m_pCoreServices->getVinylControlManager()) {
        connect(m_pMenuBar,
                &WMainMenuBar::toggleVinylControl,
                m_pCoreServices->getVinylControlManager().get(),
                &VinylControlManager::toggleVinylControl,
                Qt::UniqueConnection);
        connect(m_pCoreServices->getVinylControlManager().get(),
                &VinylControlManager::vinylControlDeckEnabled,
                m_pMenuBar,
                &WMainMenuBar::onVinylControlDeckEnabledStateChange,
                Qt::UniqueConnection);
    }
#endif

    auto pPlayerManager = m_pCoreServices->getPlayerManager();
    if (pPlayerManager) {
        connect(pPlayerManager.get(),
                &PlayerManager::numberOfDecksChanged,
                m_pMenuBar,
                &WMainMenuBar::onNumberOfDecksChanged,
                Qt::UniqueConnection);
        m_pMenuBar->onNumberOfDecksChanged(pPlayerManager->numberOfDecks());
    }

    if (m_pCoreServices->getTrackCollectionManager()) {
        connect(m_pMenuBar,
                &WMainMenuBar::rescanLibrary,
                m_pCoreServices->getTrackCollectionManager().get(),
                &TrackCollectionManager::startLibraryScan,
                Qt::UniqueConnection);
        connect(m_pCoreServices->getTrackCollectionManager().get(),
                &TrackCollectionManager::libraryScanStarted,
                m_pMenuBar,
                &WMainMenuBar::onLibraryScanStarted,
                Qt::UniqueConnection);
        connect(m_pCoreServices->getTrackCollectionManager().get(),
                &TrackCollectionManager::libraryScanFinished,
                m_pMenuBar,
                &WMainMenuBar::onLibraryScanFinished,
                Qt::UniqueConnection);
    }

    if (m_pCoreServices->getLibrary()) {
        connect(m_pMenuBar,
                &WMainMenuBar::searchInCurrentView,
                m_pCoreServices->getLibrary().get(),
                &Library::slotSearchInCurrentView,
                Qt::UniqueConnection);
        connect(m_pMenuBar,
                &WMainMenuBar::searchInAllTracks,
                m_pCoreServices->getLibrary().get(),
                &Library::slotSearchInAllTracks,
                Qt::UniqueConnection);
        connect(m_pMenuBar,
                &WMainMenuBar::createCrate,
                m_pCoreServices->getLibrary().get(),
                &Library::slotCreateCrate,
                Qt::UniqueConnection);
        connect(m_pMenuBar,
                &WMainMenuBar::createPlaylist,
                m_pCoreServices->getLibrary().get(),
                &Library::slotCreatePlaylist,
                Qt::UniqueConnection);
        connect(m_pMenuBar,
                &WMainMenuBar::showAutoDJ,
                m_pCoreServices->getLibrary().get(),
                &Library::showAutoDJ,
                Qt::UniqueConnection);
    }

#ifdef __ENGINEPRIME__
    DEBUG_ASSERT(m_pLibraryExporter);
    connect(m_pMenuBar,
            &WMainMenuBar::exportLibrary,
            m_pLibraryExporter.get(),
            &mixxx::LibraryExporter::slotRequestExport,
            Qt::UniqueConnection);
#endif
}

/// Enable/disable listening to Alt key press for toggling the menubar.
#ifndef __APPLE__
void MixxxMainWindow::slotUpdateMenuBarAltKeyConnection() {
    if (!m_pCoreServices->getKeyboardEventFilter() || !m_pMenuBar) {
        return;
    }

    if (m_pCoreServices->getSettings()->getValue<bool>(kHideMenuBarConfigKey, false)) {
        // with Qt::UniqueConnection we don't need to check whether we're already connected
        connect(m_pCoreServices->getKeyboardEventFilter().get(),
                &KeyboardEventFilter::altPressedWithoutKeys,
                m_pMenuBar,
                &WMainMenuBar::slotToggleMenuBar,
                Qt::UniqueConnection);
        m_pMenuBar->hideMenuBar();
    } else {
        disconnect(m_pCoreServices->getKeyboardEventFilter().get(),
                &KeyboardEventFilter::altPressedWithoutKeys,
                m_pMenuBar,
                &WMainMenuBar::slotToggleMenuBar);
        m_pMenuBar->showMenuBar();
    }
}
#endif

void MixxxMainWindow::slotFileLoadSongPlayer(int deck) {
    QString group = PlayerManager::groupForDeck(deck - 1);

    QString loadTrackText = tr("Load track to Deck %1").arg(QString::number(deck));
    QString deckWarningMessage = tr("Deck %1 is currently playing a track.")
                                         .arg(QString::number(deck));
    QString areYouSure = tr("Are you sure you want to load a new track?");

    if (ControlObject::get(ConfigKey(group, "play")) > 0.0) {
        int ret = QMessageBox::warning(this,
                VersionStore::applicationName(),
                deckWarningMessage + "\n" + areYouSure,
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);

        if (ret != QMessageBox::Yes) {
            return;
        }
    }

    UserSettingsPointer pConfig = m_pCoreServices->getSettings();
    QString trackPath =
            QFileDialog::getOpenFileName(
                    this,
                    loadTrackText,
                    pConfig->getValueString(mixxx::library::prefs::kLegacyDirectoryConfigKey),
                    QString("Audio (%1)")
                            .arg(SoundSourceProxy::getSupportedFileNamePatterns().join(" ")));

    if (!trackPath.isNull()) {
        // The user has picked a file via a file dialog. This means the system
        // sandboxer (if we are sandboxed) has granted us permission to this
        // folder. Create a security bookmark while we have permission so that
        // we can access the folder on future runs. We need to canonicalize the
        // path so we first wrap the directory string with a QDir.
        mixxx::FileInfo fileInfo(trackPath);
        Sandbox::createSecurityToken(&fileInfo);

        m_pCoreServices->getPlayerManager()->slotLoadToDeck(trackPath, deck);
    }
}

void MixxxMainWindow::slotDeveloperTools(bool visible) {
    if (visible) {
        if (m_pDeveloperToolsDlg == nullptr) {
            UserSettingsPointer pConfig = m_pCoreServices->getSettings();
            m_pDeveloperToolsDlg = new DlgDeveloperTools(this, pConfig);
            connect(m_pDeveloperToolsDlg,
                    &DlgDeveloperTools::destroyed,
                    this,
                    &MixxxMainWindow::slotDeveloperToolsClosed);
            connect(this,
                    &MixxxMainWindow::closeDeveloperToolsDlgChecked,
                    m_pDeveloperToolsDlg,
                    &DlgDeveloperTools::done);
            connect(m_pDeveloperToolsDlg,
                    &DlgDeveloperTools::destroyed,
                    m_pMenuBar,
                    &WMainMenuBar::onDeveloperToolsHidden);
        }
        m_pMenuBar->onDeveloperToolsShown();
        m_pDeveloperToolsDlg->show();
        m_pDeveloperToolsDlg->activateWindow();
    } else {
        emit closeDeveloperToolsDlgChecked(0);
    }
}

void MixxxMainWindow::slotDeveloperToolsClosed() {
    m_pDeveloperToolsDlg = nullptr;
}

void MixxxMainWindow::slotViewFullScreen(bool toggle) {
    if (isFullScreen() == toggle) {
        return;
    }

    // Just switch the window state here. eventFilter() will catch the
    // QWindowStateChangeEvent and inform the menu bar that fullscreen changed.
    if (toggle) {
        showFullScreen();
    } else {
        showNormal();
    }
}

void MixxxMainWindow::slotOptionsPreferences() {
    m_pPrefDlg->show();
    m_pPrefDlg->raise();
    m_pPrefDlg->activateWindow();
}

void MixxxMainWindow::slotNoVinylControlInputConfigured() {
    if (m_noVinylInputDialog && m_noVinylInputDialog->isVisible()) {
        // Don't show redundant dialogs.
        // They might be triggered be repeated controller or keyboard and
        // can lockup the GUI.
        return;
    }

    if (!m_noVinylInputDialog) {
        m_noVinylInputDialog = make_parented<QMessageBox>(
                QMessageBox::Warning,
                VersionStore::applicationName(),
                tr("There is no input device selected for this vinyl control.\n"
                   "Please select an input device in the sound hardware preferences first."),
                QMessageBox::Ok | QMessageBox::Cancel,
                this);
        m_noVinylInputDialog->setWindowModality(Qt::ApplicationModal);
        m_noVinylInputDialog->setDefaultButton(QMessageBox::Cancel);
    }
    m_noVinylInputDialog->exec();
    if (m_noVinylInputDialog->clickedButton() ==
            m_noVinylInputDialog->button(QMessageBox::Ok)) {
        m_pPrefDlg->show();
        m_pPrefDlg->showSoundHardwarePage(mixxx::preferences::SoundHardwareTab::Input);
    }
}

void MixxxMainWindow::slotNoDeckPassthroughInputConfigured() {
    if (m_noPassthroughInputDialog && m_noPassthroughInputDialog->isVisible()) {
        // Don't show redundant dialogs.
        // They might be triggered be repeated controller or keyboard and
        // can lockup the GUI.
        return;
    }

    if (!m_noPassthroughInputDialog) {
        m_noPassthroughInputDialog = make_parented<QMessageBox>(
                QMessageBox::Warning,
                VersionStore::applicationName(),
                tr("There is no input device selected for this passthrough control.\n"
                   "Please select an input device in the sound hardware preferences first."),
                QMessageBox::Ok | QMessageBox::Cancel,
                this);
        m_noPassthroughInputDialog->setWindowModality(Qt::ApplicationModal);
        m_noPassthroughInputDialog->setDefaultButton(QMessageBox::Cancel);
    }
    m_noPassthroughInputDialog->exec();
    if (m_noPassthroughInputDialog->clickedButton() ==
            m_noPassthroughInputDialog->button(QMessageBox::Ok)) {
        m_pPrefDlg->show();
        m_pPrefDlg->showSoundHardwarePage(mixxx::preferences::SoundHardwareTab::Input);
    }
}

void MixxxMainWindow::slotNoMicrophoneInputConfigured() {
    if (m_noMicInputDialog && m_noMicInputDialog->isVisible()) {
        // Don't show redundant dialogs.
        // They might be triggered be repeated controller or keyboard and
        // can lockup the GUI.
        return;
    }

    if (!m_noMicInputDialog) {
        m_noMicInputDialog = make_parented<QMessageBox>(
                QMessageBox::Warning,
                VersionStore::applicationName(),
                tr("There is no input device selected for this microphone.\n"
                   "Do you want to select an input device?"),
                QMessageBox::Ok | QMessageBox::Cancel,
                this);
        m_noMicInputDialog->setWindowModality(Qt::ApplicationModal);
        m_noMicInputDialog->setDefaultButton(QMessageBox::Cancel);
    }
    m_noMicInputDialog->exec();
    if (m_noMicInputDialog->clickedButton() ==
            m_noMicInputDialog->button(QMessageBox::Ok)) {
        m_pPrefDlg->show();
        m_pPrefDlg->showSoundHardwarePage(mixxx::preferences::SoundHardwareTab::Input);
    }
}

void MixxxMainWindow::slotNoAuxiliaryInputConfigured() {
    if (m_noAuxInputDialog && m_noAuxInputDialog->isVisible()) {
        // Don't show redundant dialogs.
        // They might be triggered be repeated controller or keyboard and
        // can lockup the GUI.
        return;
    }

    if (!m_noAuxInputDialog) {
        m_noAuxInputDialog = make_parented<QMessageBox>(
                QMessageBox::Warning,
                VersionStore::applicationName(),
                tr("There is no input device selected for this auxiliary.\n"
                   "Do you want to select an input device?"),
                QMessageBox::Ok | QMessageBox::Cancel,
                this);
        m_noAuxInputDialog->setWindowModality(Qt::ApplicationModal);
        m_noAuxInputDialog->setDefaultButton(QMessageBox::Cancel);
    }
    m_noAuxInputDialog->exec();
    if (m_noAuxInputDialog->clickedButton() ==
            m_noAuxInputDialog->button(QMessageBox::Ok)) {
        m_pPrefDlg->show();
        m_pPrefDlg->showSoundHardwarePage(mixxx::preferences::SoundHardwareTab::Input);
    }
}

void MixxxMainWindow::slotHelpAbout() {
    DlgAbout* about = new DlgAbout;
    about->show();
}

void MixxxMainWindow::slotLibraryScanSummaryDlg(const LibraryScanResultSummary& result) {
    if (!m_pCoreServices->getSettings()->getValue<bool>(
                mixxx::library::prefs::kShowScanSummaryConfigKey, true)) {
        return;
    }

    // Don't show the report dialog when the scan is run during startup and no
    // noteworthy changes have been detected.
    if (result.autoscan &&
            result.numNewTracks == 0 &&
            result.numNewMissingTracks == 0 &&
            result.numRediscoveredTracks == 0) {
        return;
    }

    QMessageBox* pMsg = new QMessageBox();
    pMsg->setAttribute(Qt::WA_DeleteOnClose);
    pMsg->setTextFormat(Qt::RichText); // required to get bold text with <b> tags
    pMsg->setWindowTitle(tr("Library scan finished"));

    if (result.noDirectoriesConfigured) {
        pMsg->setText(tr("No music directories configured for scanning.") +
                QStringLiteral("<br>") +
                tr("Add directories in the library preferences."));
        pMsg->show();
        return;
    }

    QString summary =
            tr("Scan took %1").arg(result.durationString) + QStringLiteral("<br><br>");
    if (result.numNewTracks == 0 &&
            result.numMovedTracks == 0 &&
            result.numNewMissingTracks == 0 &&
            result.numRediscoveredTracks == 0) {
        summary += tr("No changes detected.") +
                QStringLiteral("<br><b>") +
                tr("%n track(s) in total", nullptr, result.tracksTotal) +
                QStringLiteral("</b>");
    } else {
        if (result.numNewTracks != 0) {
            summary += tr("%n new track(s) found", nullptr, result.numNewTracks) +
                    QStringLiteral("<br>");
        }
        if (result.numMovedTracks != 0) {
            summary += tr("%n moved track(s) detected", nullptr, result.numMovedTracks) +
                    QStringLiteral("<br>");
        }
        if (result.numNewMissingTracks != 0) {
            summary += tr("%n track(s) missing (%1 total)",
                    nullptr,
                    result.numNewMissingTracks)
                               .arg(result.numMissingTracks);
        }
        if (result.numRediscoveredTracks != 0) {
            summary += QStringLiteral("<br>") +
                    tr("%n track(s) rediscovered",
                            nullptr,
                            result.numRediscoveredTracks);
        }
        summary += QStringLiteral("<br><br><b>") +
                tr("%n track(s) in total", nullptr, result.tracksTotal) +
                QStringLiteral("</b>");
    }

    pMsg->setText(summary);
    pMsg->show();
}

void MixxxMainWindow::slotShowKeywheel(bool toggle) {
    if (!m_pKeywheel) {
        m_pKeywheel = make_parented<DlgKeywheel>(this, m_pCoreServices->getSettings());
        // uncheck the menu item on window close
        connect(m_pKeywheel.get(),
                &DlgKeywheel::finished,
                m_pMenuBar,
                &WMainMenuBar::onKeywheelChange);
    }
    if (toggle) {
        m_pKeywheel->show();
        m_pKeywheel->raise();
    } else {
        m_pKeywheel->hide();
    }
}

void MixxxMainWindow::slotTooltipModeChanged(mixxx::preferences::Tooltips tt) {
    m_toolTipsCfg = tt;
    m_pCoreServices->getKeyboardEventFilter()->setShowOnlyKbdShortcuts(
            tt == mixxx::preferences::Tooltips::OnlyKbdShortcuts);
#ifdef MIXXX_USE_QOPENGL
    ToolTipQOpenGL::singleton().setActive(
            m_toolTipsCfg == mixxx::preferences::Tooltips::On);
#endif
}

void MixxxMainWindow::rebootMixxxView() {
    qDebug() << "Now in rebootMixxxView...";
    m_inRebootMixxxView = true;

    ScopedWaitCursor cursor;
    // safe geometry for later restoration
    const QRect initGeometry = geometry();

    // We need to tell the menu bar that we are about to delete the old skin and
    // create a new one. It holds "visibility" controls (e.g. "Show Samplers")
    // that need to be deleted -- otherwise we can't tell what features the skin
    // supports since the controls from the previous skin will be left over.
    m_pMenuBar->onNewSkinAboutToLoad();

    if (m_pCentralWidget) {
        // Clear widget pointer list and destroy all update connections before
        // we delete the main widget (ie. all WBaseWidgets) to prevent
        // KeyboardEventFilter accessing dangling pointers, just in case a
        // shortcuts/tooltip update is triggered while we re/load a skin.
        m_pCoreServices->getKeyboardEventFilter()->clearWidgets();
        m_pCentralWidget->hide();
        WaveformWidgetFactory::instance()->destroyWidgets();
        delete m_pCentralWidget;
        m_pCentralWidget = nullptr;
    }

    // Workaround for changing skins while fullscreen, just go out of fullscreen
    // mode. If you change skins while in fullscreen (on Linux, at least) the
    // window returns to 0,0 but and the backdrop disappears so it looks as if
    // it is not fullscreen, but acts as if it is.
    bool wasFullScreen = isFullScreen();
    if (wasFullScreen) {
        showMaximized();
    }

    tryParseAndSetDefaultStyleSheet();

    if (!loadConfiguredSkin()) {
        QMessageBox::critical(this,
                tr("Error in skin file"),
                tr("The selected skin cannot be loaded."));
        m_inRebootMixxxView = false;
        // m_pWidgetParent is NULL, we can't continue.
        return;
    }
    m_pMenuBar->setStyleSheet(m_pCentralWidget->styleSheet());

    setCentralWidget(m_pCentralWidget);
#if defined(__LINUX__) && !defined(__ANDROID__)
    // don't adjustSize() on Linux as this wouldn't use the entire available area
    // to paint the new skin with X11
    // https://github.com/mixxxdj/mixxx/issues/9309
#else
    adjustSize();
#endif

    if (wasFullScreen) {
        showFullScreen();
    } else {
        // Programmatic placement at this point is very problematic.
        // The screen() method returns stale data (primary screen)
        // until the user interacts with mixxx again. Keyboard shortcuts
        // do not count, moving window, opening menu etc does
        // Therefore the placement logic was removed by a simple geometry restore.
        // If the minimum size of the new skin is larger then the restored
        // geometry, the window will be enlarged right & bottom which is
        // safe as the menu is still reachable.
        setGeometry(initGeometry);
    }

    m_inRebootMixxxView = false;
    qDebug() << "rebootMixxxView DONE";
}

bool MixxxMainWindow::loadConfiguredSkin() {
    // TODO: use std::shared_ptr throughout skin widgets instead of these hacky get() calls
    m_pCentralWidget = m_pSkinLoader->loadConfiguredSkin(this,
            &m_skinCreatedControls,
            m_pCoreServices.get());
    if (centralWidget() == m_pLaunchImage) {
        initializationProgressUpdate(100, "");
    }
    emit skinLoaded();
    return m_pCentralWidget != nullptr;
}

/// Try to load default styles that can be overridden by skins
void MixxxMainWindow::tryParseAndSetDefaultStyleSheet() {
    const QString resPath = m_pCoreServices->getSettings()->getResourcePath();
    QFile file(resPath + "/skins/default.qss");
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray fileBytes = file.readAll();
        QString style = QString::fromUtf8(fileBytes);
        setStyleSheet(style);
    } else {
        qWarning() << "Failed to load default skin styles /skins/default.qss!";
    }
}

/// Catch ToolTip and WindowStateChange events
bool MixxxMainWindow::eventFilter(QObject* obj, QEvent* event) {
#ifdef Q_OS_ANDROID
    if (obj == m_pCentralWidget && event->type() == QEvent::Resize &&
            m_pRemoteLibraryButton) {
        m_pRemoteLibraryButton->move(
                qMax(8,
                        m_pCentralWidget->width() -
                                m_pRemoteLibraryButton->width() - 12),
                12);
        m_pRemoteLibraryButton->raise();
    }
#endif
    if (event->type() == QEvent::ToolTip) {
        // Always show tooltips if Ctrl is held down
        if (QApplication::keyboardModifiers().testFlag(Qt::ControlModifier)) {
            return QMainWindow::eventFilter(obj, event);
        }
        // Always show tooltips for cue type buttons in the Cue menu
        if (QLatin1String(obj->metaObject()->className()) == "CueMenuPushButton") {
            return QMainWindow::eventFilter(obj, event);
        }
        // Always show tooltips in Preferences
        QWidget* activeWindow = QApplication::activeWindow();
        if (activeWindow &&
                QLatin1String(activeWindow->metaObject()->className()) ==
                        "DlgPreferences") {
            return QMainWindow::eventFilter(obj, event);
        }

        // For all other we follow the tooltip sett8ing.
        // Return true for no tool tips
        switch (m_toolTipsCfg) {
        case mixxx::preferences::Tooltips::OnlyInLibrary:
            // WLibrary's stacked widgets are not derived from WBaseWidget
            if (dynamic_cast<WBaseWidget*>(obj) != nullptr) {
                return true;
            }
            break;
        case mixxx::preferences::Tooltips::OnlyKbdShortcuts:
            if (dynamic_cast<WBaseWidget*>(obj) == nullptr) {
                return true;
            }
            break;
        case mixxx::preferences::Tooltips::On:
            break;
        case mixxx::preferences::Tooltips::Off:
            return true;
        default:
            DEBUG_ASSERT(!"m_toolTipsCfg value unknown");
            return true;
        }
    } else if (event->type() == QEvent::WindowStateChange) {
#ifndef __APPLE__
        if (windowState() == m_prevState) {
            // Ignore no-op. This happens if another window is raised above
            // MixxxMianWindow,  e.g. DlgPeferences. In such a case event->oldState()
            // will be Qt::WindowNoState which is wrong anyway, so there is nothing
            // to do internally.
            return QMainWindow::eventFilter(obj, event);
        }
        m_prevState = windowState();
#endif
        // Detect if we entered or quit fullscreen mode.
        QWindowStateChangeEvent* changeEvent =
                static_cast<QWindowStateChangeEvent*>(event);
        const bool wasFullScreen = changeEvent->oldState() & Qt::WindowFullScreen;
        const bool isFullScreenNow = windowState() & Qt::WindowFullScreen;
        if ((isFullScreenNow && !wasFullScreen) ||
                (!isFullScreenNow && wasFullScreen)) {
#if defined(__LINUX__) && !defined(__ANDROID__)
            // Fix for "No menu bar with ubuntu unity in full screen mode"
            // (issues #6072 and #6689). Before touching anything here, please
            // read those bugs.
            // Set this attribute instead of calling setNativeMenuBar(false),
            // see https://github.com/mixxxdj/mixxx/issues/11320
            if (m_supportsGlobalMenuBar) {
                QApplication::setAttribute(Qt::AA_DontUseNativeMenuBar, isFullScreenNow);
                createMenuBar();
                connectMenuBar();
            }
#endif

#ifndef __APPLE__
#if defined(__LINUX__) && !defined(__ANDROID__)
            // Only show the dialog if we are able to have the menubar in the
            // main window, only then we're able to hide it.
            if (!m_supportsGlobalMenuBar || isFullScreenNow)
#endif
            {
                if (!m_inRebootMixxxView) {
                    alwaysHideMenuBarDlg();
                }
                slotUpdateMenuBarAltKeyConnection();
            }
#endif

            // This will toggle the Fullscreen checkbox and hide the menubar if
            // we go fullscreen.
            // Skip this during startup or the launchimage will be shifted
            // up & down when the menu is shown menu and 'hidden'. The menu
            // will be updated when the skin finished loading.
            if (centralWidget() != m_pLaunchImage) {
                emit fullScreenChanged(isFullScreen());
            }
        }
    }
    // standard event processing
    return QMainWindow::eventFilter(obj, event);
}

void MixxxMainWindow::closeEvent(QCloseEvent* event) {
    // WARNING: We can receive a CloseEvent while only partially
    // initialized. This is because we call QApplication::processEvents to
    // render LaunchImage progress in the constructor.
#ifdef Q_OS_ANDROID
    // On Android, the system Back gesture / Back button is delivered to the
    // top-level window as a QCloseEvent (Qt 6 behaviour). Treat it as
    // "close current view" rather than "exit the app": if BIG LIBRARY is
    // maximized, just collapse it back to the deck view; only fall through
    // to the real exit-confirm flow when there's nothing left to close.
    // This matches user expectations from every other Android app and means
    // a stray edge swipe does not abruptly tear down audio.
    if (handleAndroidBack()) {
        event->ignore();
        return;
    }
#endif
    if (!confirmExit()) {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

#ifdef Q_OS_ANDROID
void MixxxMainWindow::keyPressEvent(QKeyEvent* event) {
    // Some Android device manufacturers still ship a hardware Back key (or
    // map a gesture to one) that delivers Qt::Key_Back as a key event rather
    // than as a close event. Treat it identically — collapse the current
    // expanded view if any, otherwise let the default propagation kick in
    // (which on Android ends up calling closeEvent → confirmExit()).
    if (event->key() == Qt::Key_Back) {
        if (handleAndroidBack()) {
            event->accept();
            return;
        }
    }

    // Keyboard shortcuts for Android (hardware keyboard)
    if (event->modifiers() & Qt::ControlModifier) {
        switch (event->key()) {
        case Qt::Key_S: {
            // Ctrl+S: Focus the library search box for YouTube search
            auto* searchBox = findChild<WSearchLineEdit*>();
            if (searchBox) {
                searchBox->setFocus(Qt::ShortcutFocusReason);
                // WSearchLineEdit is a QComboBox; select all text in the edit field
                if (searchBox->lineEdit()) {
                    searchBox->lineEdit()->selectAll();
                }
            }
            event->accept();
            return;
        }
        case Qt::Key_P:
            // Ctrl+P: Open preferences/options dialog
            slotOptionsPreferences();
            event->accept();
            return;
        default:
            break;
        }
    }

    QMainWindow::keyPressEvent(event);
}

void MixxxMainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
#ifdef Q_OS_ANDROID
    // On Android DeX/external screen, the cursor may be hidden until the window
    // is explicitly focused. Force cursor visibility on first show.
    setCursor(Qt::ArrowCursor);
    activateWindow();
    raise();
#endif
}

#ifdef Q_OS_ANDROID
void MixxxMainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange) {
        if (isActiveWindow()) {
            setCursor(Qt::ArrowCursor);
        }
    }
}
#endif

bool MixxxMainWindow::handleAndroidBack() {
    // Pop dialogs / menus first — Qt already sends them the Back event before
    // it reaches the main window, but defensively re-check here in case any
    // were opened with Qt::WA_ShowWithoutActivating or similar.
    QWidget* active = QApplication::activeModalWidget();
    if (active && active != this) {
        active->close();
        return true;
    }
    // BIG LIBRARY (`[Master],maximize_library` is the legacy name; aliased
    // to `[Skin],show_maximized_library`). Collapse it back to deck view
    // when on, so the user can return to the decks without exiting.
    const ConfigKey kMaximizeLibrary(
            QStringLiteral("[Master]"), QStringLiteral("maximize_library"));
    if (ControlObject::toBool(kMaximizeLibrary)) {
        ControlObject::set(kMaximizeLibrary, 0.0);
        return true;
    }
    // Nothing to close — let the caller fall through to the real exit
    // confirmation.
    return false;
}
#endif

void MixxxMainWindow::checkDirectRendering() {
    // IF
    //  * A waveform viewer exists
    // AND
    //  * The waveform viewer is an OpenGL waveform viewer
    // AND
    //  * The waveform viewer does not have direct rendering enabled.
    // THEN
    //  * Warn user

    WaveformWidgetFactory* factory = WaveformWidgetFactory::instance();
    if (!factory) {
        return;
    }

    UserSettingsPointer pConfig = m_pCoreServices->getSettings();

    // On Android, OpenGL availability is handled differently and the
    // desktop GL check is not applicable (allshader widgets are used).
    // Skip the direct rendering warning.
#if !defined(Q_OS_ANDROID)
    if (!factory->isOpenGlAvailable() && !factory->isOpenGlesAvailable() &&
            pConfig->getValueString(ConfigKey("[Direct Rendering]", "Warned")) != QString("yes")) {
        QMessageBox::warning(nullptr,
                tr("OpenGL Direct Rendering"),
                tr("Direct rendering is not enabled on your machine.<br><br>"
                   "This means that the waveform displays will be very<br>"
                   "<b>slow and may tax your CPU heavily</b>. Either update "
                   "your<br>"
                   "configuration to enable direct rendering, or disable<br>"
                   "the waveform displays in the Mixxx preferences by "
                   "selecting<br>"
                   "\"Empty\" as the waveform display in the 'Interface' "
                   "section."));
        pConfig->set(ConfigKey("[Direct Rendering]", "Warned"), QString("yes"));
    }
#endif
}

bool MixxxMainWindow::confirmExit() {
    bool playing(false);
    bool playingSampler(false);
    auto pPlayerManager = m_pCoreServices->getPlayerManager();
    int deckCount = pPlayerManager->numberOfDecks();
    int samplerCount = pPlayerManager->numberOfSamplers();
    for (int i = 0; i < deckCount; ++i) {
        if (ControlObject::toBool(
                    ConfigKey(PlayerManager::groupForDeck(i), "play"))) {
            playing = true;
            break;
        }
    }
    for (int i = 0; i < samplerCount; ++i) {
        if (ControlObject::toBool(
                    ConfigKey(PlayerManager::groupForSampler(i), "play"))) {
            playingSampler = true;
            break;
        }
    }
    if (playing) {
        QMessageBox::StandardButton btn = QMessageBox::question(this,
                tr("Confirm Exit"),
                tr("A deck is currently playing. Exit Mixxx?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
        if (btn == QMessageBox::No) {
            return false;
        }
    } else if (playingSampler) {
        QMessageBox::StandardButton btn = QMessageBox::question(this,
                tr("Confirm Exit"),
                tr("A sampler is currently playing. Exit Mixxx?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
        if (btn == QMessageBox::No) {
            return false;
        }
    }
    if (m_pPrefDlg && m_pPrefDlg->isVisible()) {
        QMessageBox::StandardButton btn = QMessageBox::question(
                this, tr("Confirm Exit"), tr("The preferences window is still open.") + "<br>" + tr("Discard any changes and exit Mixxx?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (btn == QMessageBox::No) {
            return false;
        } else {
            m_pPrefDlg->close();
        }
    }

    return true;
}

void MixxxMainWindow::initializationProgressUpdate(int progress, const QString& serviceName) {
    if (m_pLaunchImage) {
        m_pLaunchImage->progress(progress, serviceName);
    }
    qApp->processEvents();
}

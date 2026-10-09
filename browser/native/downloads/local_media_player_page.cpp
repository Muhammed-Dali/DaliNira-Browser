#include "local_media_player_page.h"

#include "browser_window.h"
#include "audio/web_audio_effects_controller.h"
#include "core/browser_icons.h"

#include <QFileInfo>
#include <QFile>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineFullScreenRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include "i18n/i18n.h"

namespace {

class LockedLocalMediaPage final : public QWebEnginePage {
  Q_OBJECT
 public:
  LockedLocalMediaPage(QWebEngineProfile *profile, QObject *parent)
      : QWebEnginePage(profile, parent) {
    connect(this, &QWebEnginePage::loadFinished, this,
            [this] { trustedDocumentLoad_ = false; });
  }

  void prepareTrustedDocument(const QUrl &documentUrl) {
    documentUrl_ = documentUrl;
    trustedDocumentLoad_ = true;
  }

 signals:
  void audioEffectsRequested();

 protected:
  bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override {
    if (url.scheme() == QLatin1String("dalinira")) {
      if (url.host() == QLatin1String("audio-effects")) {
        emit audioEffectsRequested();
      }
      return false;
    }
    if (isMainFrame && url != documentUrl_ && url != QUrl(QStringLiteral("about:blank"))) {
      // QWebEngine implements setHtml() as a data: navigation even when a
      // file: base URL is supplied. Permit only that one native-initiated
      // document load; lock the page again as soon as it finishes.
      if (!trustedDocumentLoad_ || url.scheme() != QLatin1String("data")) return false;
    }
    return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
  }

 private:
  QUrl documentUrl_;
  bool trustedDocumentLoad_ = false;
};

}  // namespace

QString LocalMediaPlayerPage::playerHtml(const LocalMediaOpenRequest &request,
                                         const QUrl &mediaUrl,
                                         bool includeHeader,
                                         const QString &resolvedArtworkUrl,
                                         bool customControls) {
  const bool audio = request.playerKind == LocalMediaPlayerKind::Audio;
  const QString element = audio ? QStringLiteral("audio") : QStringLiteral("video");
  const QString title = (request.title.trimmed().isEmpty()
      ? QFileInfo(request.path).completeBaseName() : request.title).toHtmlEscaped();
  const QString fileName = QFileInfo(request.path).fileName().toHtmlEscaped();
  const QString source = mediaUrl.toString(QUrl::FullyEncoded).toHtmlEscaped();
  const QString mediaClass = audio ? QStringLiteral("audio") : QStringLiteral("video");

  Q_UNUSED(I18n::text(QStringLiteral("media_player.pause")));
  Q_UNUSED(I18n::text(QStringLiteral("media_player.unmute")));
  Q_UNUSED(I18n::text(QStringLiteral("media_player.speed")));

  QString headerHtml;
  if (includeHeader) {
    headerHtml = QStringLiteral(R"HTML(
<header class="player-top-header" id="player-header">
  <div class="header-filename">%1</div>
  <a href="dalinira://audio-effects" class="effects-btn" id="header-effects-btn">
    <svg viewBox="0 0 24 24" width="15" height="15"><path fill="currentColor" d="M3 17v2h6v-2H3zM3 5v2h10V5H3zm10 16v-2h8v-2h-8v-2h-2v6h2zM7 9v2H3v2h4v2h2V9H7zm14 4v-2H11v2h10zm-6-4h2V7h4V5h-4V3h-2v6z"/></svg>
    <span>{{i18n:menu.audio_effects}}</span>
  </a>
</header>
)HTML").arg(fileName);
  }

  const bool hasArtwork = !resolvedArtworkUrl.trimmed().isEmpty();
  const QString coverImgAttr = hasArtwork
      ? QStringLiteral("src=\"%1\"").arg(resolvedArtworkUrl.trimmed().toHtmlEscaped())
      : QStringLiteral("hidden");
  const QString coverPlaceholderStyle = hasArtwork
      ? QStringLiteral("style=\"display:none;\"")
      : QString();

  const QString controlsAttr = (!customControls) ? QStringLiteral("controls") : QString();

  QString customControlsHtml;
  QString customControlsScript;
  if (customControls) {
    customControlsHtml = QStringLiteral(R"HTML(
<div class="media-controls-bar" id="media-controls-bar">
  <button type="button" class="m-btn" id="c-rewind-btn" title="{{i18n:media_player.rewind}}">
    <svg viewBox="0 0 24 24" width="20" height="20"><path fill="currentColor" d="M12 5V1L7 6l5 5V7c3.31 0 6 2.69 6 6s-2.69 6-6 6-6-2.69-6-6H4c0 4.42 3.58 8 8 8s8-3.58 8-8-3.58-8-8-8z"/><text x="12" y="14.5" font-size="7" font-weight="bold" text-anchor="middle" fill="currentColor" font-family="system-ui, sans-serif">15</text></svg>
  </button>
  <button type="button" class="m-btn m-play-btn" id="c-play-btn" title="{{i18n:media_player.play}}">
    <svg id="c-ico-play" viewBox="0 0 24 24" width="22" height="22" style="display:none;"><path fill="currentColor" d="M8 5v14l11-7z"/></svg>
    <svg id="c-ico-pause" viewBox="0 0 24 24" width="22" height="22"><path fill="currentColor" d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>
  </button>
  <button type="button" class="m-btn" id="c-forward-btn" title="{{i18n:media_player.forward}}">
    <svg viewBox="0 0 24 24" width="20" height="20"><path fill="currentColor" d="M12 5V1l5 5-5 5V7c-3.31 0-6 2.69-6 6s2.69 6 6 6 6-2.69 6-6h2c0 4.42-3.58 8-8 8s-8-3.58-8-8 3.58-8 8-8z"/><text x="12" y="14.5" font-size="7" font-weight="bold" text-anchor="middle" fill="currentColor" font-family="system-ui, sans-serif">15</text></svg>
  </button>
  <span class="m-time" id="c-time-cur">00:00</span>
  <div class="m-timeline" id="c-timeline">
    <div class="m-timeline-track">
      <div class="m-timeline-buffer" id="c-timeline-buf"></div>
      <div class="m-timeline-played" id="c-timeline-prog"></div>
      <div class="m-timeline-thumb" id="c-timeline-thumb"></div>
    </div>
  </div>
  <span class="m-time" id="c-time-dur">00:00</span>
  <div class="m-vol-group" id="c-vol-group">
    <button type="button" class="m-btn" id="c-vol-btn" title="{{i18n:media_player.mute}}">
      <svg id="c-vol-ico-high" viewBox="0 0 24 24" width="18" height="18"><path fill="currentColor" d="M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02zM14 3.23v2.06c2.89.86 5 3.54 5 6.71s-2.11 5.85-5 6.71v2.06c4.01-.91 7-4.49 7-8.77s-2.99-7.86-7-8.77z"/></svg>
      <svg id="c-vol-ico-mute" viewBox="0 0 24 24" width="18" height="18" style="display:none;"><path fill="currentColor" d="M16.5 12c0-1.77-1.02-3.29-2.5-4.03v2.21l2.45 2.45c.03-.2.05-.41.05-.63zm2.5 0c0 .94-.2 1.82-.54 2.64l1.51 1.51C20.63 14.91 21 13.5 21 12c0-4.28-2.99-7.86-7-8.77v2.06c2.89.86 5 3.54 5 6.71zM4.27 3L3 4.27 7.73 9H3v6h4l5 5v-6.73l4.25 4.25c-.67.52-1.42.93-2.25 1.18v2.06c1.38-.31 2.63-.95 3.69-1.81L19.73 21 21 19.73l-9-9L4.27 3zM12 4L9.91 6.09 12 8.18V4z"/></svg>
    </button>
    <div class="m-vol-slider-wrap" id="c-vol-wrap">
      <div class="m-vol-bar" id="c-vol-bar" role="slider" tabindex="0" aria-label="{{i18n:media_player.mute}}" aria-valuenow="1" aria-valuemin="0" aria-valuemax="1">
        <div class="m-vol-track" id="c-vol-track">
          <div class="m-vol-played" id="c-vol-prog"></div>
          <div class="m-vol-thumb" id="c-vol-thumb"></div>
        </div>
      </div>
    </div>
  </div>
  <div class="m-settings-group">
    <button type="button" class="m-btn" id="c-gear-btn" title="{{i18n:media_player.speed}}">
      <svg viewBox="0 0 24 24" width="18" height="18"><path fill="currentColor" d="M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61l-1.92-3.32c-.12-.22-.37-.29-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58c-.18.14-.23.41-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"/></svg>
    </button>
    <div class="m-speed-menu" id="c-speed-menu" style="display:none;">
      <div class="m-speed-item" data-speed="0.5">0.5x</div>
      <div class="m-speed-item" data-speed="0.75">0.75x</div>
      <div class="m-speed-item active" data-speed="1.0">1.0x (Normal)</div>
      <div class="m-speed-item" data-speed="1.25">1.25x</div>
      <div class="m-speed-item" data-speed="1.5">1.5x</div>
      <div class="m-speed-item" data-speed="2.0">2.0x</div>
    </div>
  </div>
  <button type="button" class="m-btn" id="c-pip-btn" title="{{i18n:media_player.pip}}">
    <svg viewBox="0 0 24 24" width="18" height="18"><path fill="currentColor" d="M19 7h-8v6h8V7zm2-4H3c-1.1 0-2 .9-2 2v14c0 1.1.9 1.98 2 1.98h18c1.1 0 2-.88 2-1.98V5c0-1.1-.9-2-2-2zm0 16.01H3V4.98h18v14.03z"/></svg>
  </button>
  <button type="button" class="m-btn" id="c-fs-btn" title="{{i18n:media_player.fullscreen}}">
    <svg id="c-ico-fs-enter" viewBox="0 0 24 24" width="18" height="18"><path fill="currentColor" d="M7 14H5v5h5v-2H7v-3zm-2-4h2V7h3V5H5v5zm12 7h-3v2h5v-5h-2v3zM14 5v2h3v3h2V5h-5z"/></svg>
    <svg id="c-ico-fs-exit" viewBox="0 0 24 24" width="18" height="18" style="display:none;"><path fill="currentColor" d="M5 16h3v3h2v-5H5v2zm3-8H5v2h5V5H8v3zm6 11h2v-3h3v-2h-5v5zm2-11V5h-2v5h5V8h-3z"/></svg>
  </button>
</div>
)HTML");

    customControlsScript = QStringLiteral(R"JS(
(function(){
  const container = document.getElementById('media-container');
  const media = document.getElementById('media');
  const ctrl = document.getElementById('media-controls-bar');
  if(!container || !media || !ctrl) return;

  const isAudio = document.body.classList.contains('audio-mode') || media.tagName.toLowerCase() === 'audio';

  const btnPlay = document.getElementById('c-play-btn');
  const icoPlay = document.getElementById('c-ico-play');
  const icoPause = document.getElementById('c-ico-pause');
  const volGroup = document.getElementById('c-vol-group');
  const btnVol = document.getElementById('c-vol-btn');
  const icoVolHigh = document.getElementById('c-vol-ico-high');
  const icoVolMute = document.getElementById('c-vol-ico-mute');
  const volBar = document.getElementById('c-vol-bar');
  const volProg = document.getElementById('c-vol-prog');
  const volThumb = document.getElementById('c-vol-thumb');
  const btnRewind = document.getElementById('c-rewind-btn');
  const btnForward = document.getElementById('c-forward-btn');
  const btnGear = document.getElementById('c-gear-btn');
  const speedMenu = document.getElementById('c-speed-menu');
  const btnPip = document.getElementById('c-pip-btn');
  const btnFs = document.getElementById('c-fs-btn');
  const icoFsEnter = document.getElementById('c-ico-fs-enter');
  const icoFsExit = document.getElementById('c-ico-fs-exit');
  const timeCur = document.getElementById('c-time-cur');
  const timeDur = document.getElementById('c-time-dur');
  const timeline = document.getElementById('c-timeline');
  const prog = document.getElementById('c-timeline-prog');
  const buf = document.getElementById('c-timeline-buf');
  const thumb = document.getElementById('c-timeline-thumb');

  let lastVolume = 1;
  let isDraggingVol = false;
  let isOverControls = false;
  let seeking = false;
  let idleTimer = null;

  function fmtTime(s){
    if(isNaN(s)||!isFinite(s)) return '00:00';
    const totalSec = Math.floor(s);
    const h = Math.floor(totalSec / 3600);
    const m = Math.floor((totalSec % 3600) / 60);
    const sec = totalSec % 60;
    if (h > 0) {
      return h + ':' + (m<10?'0':'') + m + ':' + (sec<10?'0':'') + sec;
    }
    return (m<10?'0':'')+m+':'+(sec<10?'0':'')+sec;
  }

  function updatePlayBtn(){
    if(media.paused){
      if(icoPlay) icoPlay.style.display='block';
      if(icoPause) icoPause.style.display='none';
      if(btnPlay) btnPlay.title='{{i18n:media_player.play}}';
    }else{
      if(icoPlay) icoPlay.style.display='none';
      if(icoPause) icoPause.style.display='block';
      if(btnPlay) btnPlay.title='{{i18n:media_player.pause}}';
    }
  }

  function togglePlay(){
    if(media.paused) media.play().catch(()=>{});
    else media.pause();
  }

  if(btnPlay) btnPlay.addEventListener('click', togglePlay);
  media.addEventListener('play', ()=>{ updatePlayBtn(); resetIdle(); });
  media.addEventListener('pause', ()=>{
    updatePlayBtn();
    ctrl.classList.remove('idle');
    container.classList.remove('idle-cursor');
    if(idleTimer) clearTimeout(idleTimer);
  });
  if(!isAudio) media.addEventListener('click', togglePlay);

  if(btnRewind) btnRewind.addEventListener('click', ()=>{
    media.currentTime = Math.max(0, media.currentTime - 15);
  });
  if(btnForward) btnForward.addEventListener('click', ()=>{
    media.currentTime = Math.min(media.duration||0, media.currentTime + 15);
  });

  function updateVolUi(){
    const m = media.muted || media.volume === 0;
    const v = m ? 0 : media.volume;
    const pct = (v * 100) + '%';
    if(icoVolHigh) icoVolHigh.style.display = m ? 'none' : 'block';
    if(icoVolMute) icoVolMute.style.display = m ? 'block' : 'none';
    if(volProg) volProg.style.width = pct;
    if(volThumb) volThumb.style.left = pct;
    if(volBar){
      volBar.setAttribute('aria-valuenow', v.toFixed(2));
      volBar.title = Math.round(v * 100) + '%';
    }
    if(btnVol) btnVol.title = m ? '{{i18n:media_player.unmute}}' : '{{i18n:media_player.mute}}';
  }

  function setVolumeFromEvent(e){
    const track = document.getElementById('c-vol-track');
    if(!track) return;
    const rect = track.getBoundingClientRect();
    if(rect.width <= 0) return;
    const x = Math.max(0, Math.min(e.clientX - rect.left, rect.width));
    const val = Math.round((x / rect.width) * 100) / 100;
    media.volume = val;
    if(val > 0){
      lastVolume = val;
      media.muted = false;
    }else{
      media.muted = true;
    }
    updateVolUi();
  }

  if(btnVol) btnVol.addEventListener('click', ()=>{
    if(media.muted || media.volume === 0){
      media.muted = false;
      if(media.volume === 0) media.volume = (lastVolume > 0 ? lastVolume : 1);
    }else{
      lastVolume = media.volume > 0 ? media.volume : 1;
      media.muted = true;
    }
    updateVolUi();
    resetIdle();
  });

  if(volBar){
    volBar.addEventListener('mousedown', (e)=>{
      isDraggingVol = true;
      if(volGroup) volGroup.classList.add('active');
      setVolumeFromEvent(e);
      resetIdle();
    });
    volBar.addEventListener('touchstart', (e)=>{
      if(e.touches.length > 0){
        isDraggingVol = true;
        if(volGroup) volGroup.classList.add('active');
        setVolumeFromEvent(e.touches[0]);
        resetIdle();
      }
    }, {passive: true});
    volBar.addEventListener('keydown', (e)=>{
      if(e.key === 'ArrowRight' || e.key === 'ArrowUp'){
        e.preventDefault();
        e.stopPropagation();
        media.volume = Math.min(1, Math.round((media.volume + 0.05) * 100) / 100);
        media.muted = false;
        lastVolume = media.volume;
        updateVolUi();
        resetIdle();
      }else if(e.key === 'ArrowLeft' || e.key === 'ArrowDown'){
        e.preventDefault();
        e.stopPropagation();
        media.volume = Math.max(0, Math.round((media.volume - 0.05) * 100) / 100);
        media.muted = (media.volume === 0);
        if(media.volume > 0) lastVolume = media.volume;
        updateVolUi();
        resetIdle();
      }
    });
  }

  if(volGroup){
    volGroup.addEventListener('wheel', (e)=>{
      e.preventDefault();
      const delta = e.deltaY < 0 ? 0.05 : -0.05;
      const next = Math.max(0, Math.min(1, Math.round((media.volume + delta) * 100) / 100));
      media.volume = next;
      if(next > 0){
        lastVolume = next;
        media.muted = false;
      }else{
        media.muted = true;
      }
      updateVolUi();
      resetIdle();
    }, {passive: false});
  }

  window.addEventListener('mousemove', (e)=>{
    if(isDraggingVol){
      setVolumeFromEvent(e);
      resetIdle();
    }
  });

  window.addEventListener('touchmove', (e)=>{
    if(isDraggingVol && e.touches.length > 0){
      setVolumeFromEvent(e.touches[0]);
      resetIdle();
    }
  }, {passive: true});

  window.addEventListener('mouseup', ()=>{
    if(isDraggingVol){
      isDraggingVol = false;
      if(volGroup) volGroup.classList.remove('active');
    }
    seeking = false;
  });

  window.addEventListener('touchend', ()=>{
    if(isDraggingVol){
      isDraggingVol = false;
      if(volGroup) volGroup.classList.remove('active');
    }
  });

  media.addEventListener('volumechange', updateVolUi);

  if(btnGear && speedMenu){
    btnGear.addEventListener('click', (e)=>{
      e.stopPropagation();
      speedMenu.style.display = speedMenu.style.display === 'none' ? 'block' : 'none';
      resetIdle();
    });
    document.addEventListener('click', (e)=>{
      if(!speedMenu.contains(e.target) && e.target !== btnGear){
        speedMenu.style.display = 'none';
      }
    });
    speedMenu.querySelectorAll('.m-speed-item').forEach(item => {
      item.addEventListener('click', ()=>{
        const spd = parseFloat(item.getAttribute('data-speed'));
        if(!isNaN(spd) && spd > 0){
          media.playbackRate = spd;
          speedMenu.querySelectorAll('.m-speed-item').forEach(i => i.classList.remove('active'));
          item.classList.add('active');
        }
        speedMenu.style.display = 'none';
      });
    });
  }

  function updateProgress(){
    const cur = media.currentTime || 0;
    const dur = media.duration || 0;
    if(timeCur) timeCur.textContent = fmtTime(cur);
    if(timeDur && dur > 0) timeDur.textContent = fmtTime(dur);
    const pct = dur > 0 ? (cur / dur) * 100 : 0;
    if(prog) prog.style.width = pct + '%';
    if(thumb) thumb.style.left = pct + '%';

    if(media.buffered.length > 0 && dur > 0){
      const bEnd = media.buffered.end(media.buffered.length - 1);
      if(buf) buf.style.width = ((bEnd / dur) * 100) + '%';
    }
  }

  media.addEventListener('timeupdate', updateProgress);
  media.addEventListener('loadedmetadata', updateProgress);
  media.addEventListener('progress', updateProgress);

  function seekTo(e){
    if(!timeline) return;
    const rect = timeline.getBoundingClientRect();
    const x = Math.max(0, Math.min(e.clientX - rect.left, rect.width));
    const pct = rect.width > 0 ? (x / rect.width) : 0;
    const dur = media.duration || 0;
    if(dur > 0){
      media.currentTime = pct * dur;
      updateProgress();
    }
  }

  if(timeline){
    timeline.addEventListener('mousedown', (e)=>{
      seeking = true;
      seekTo(e);
      resetIdle();
    });
    window.addEventListener('mousemove', (e)=>{
      if(seeking) {
        seekTo(e);
        resetIdle();
      }
    });
  }

  function toggleFs(){
    if(isAudio) return;
    if(document.fullscreenElement){
      document.exitFullscreen().catch(()=>{});
    }else{
      container.requestFullscreen().catch(()=>{
        media.requestFullscreen().catch(()=>{});
      });
    }
  }

  if(btnFs) btnFs.addEventListener('click', toggleFs);
  if(!isAudio){
    container.addEventListener('dblclick', (e)=>{
      if(e.target === media || e.target === container){
        toggleFs();
      }
    });
  }

  document.addEventListener('fullscreenchange', ()=>{
    const isFs = !!document.fullscreenElement;
    if(icoFsEnter) icoFsEnter.style.display = isFs ? 'none' : 'block';
    if(icoFsExit) icoFsExit.style.display = isFs ? 'block' : 'none';
    const topHdr = document.getElementById('player-header');
    if(topHdr) topHdr.style.display = isFs ? 'none' : 'flex';
    resetIdle();
  });

  if(btnPip){
    if(!isAudio && document.pictureInPictureEnabled){
      btnPip.addEventListener('click', ()=>{
        if(document.pictureInPictureElement){
          document.exitPictureInPicture().catch(()=>{});
        }else{
          media.requestPictureInPicture().catch(()=>{});
        }
      });
    }else{
      btnPip.style.display = 'none';
    }
  }

  ctrl.addEventListener('mouseenter', ()=>{
    isOverControls = true;
    ctrl.classList.remove('idle');
    container.classList.remove('idle-cursor');
    if(idleTimer) clearTimeout(idleTimer);
  });
  ctrl.addEventListener('mouseleave', ()=>{
    isOverControls = false;
    resetIdle();
  });

  function resetIdle(){
    ctrl.classList.remove('idle');
    container.classList.remove('idle-cursor');
    if(idleTimer) clearTimeout(idleTimer);
    if(isAudio) return; // Audio controls never auto-hide
    if(!media.paused && !isOverControls && !isDraggingVol && !seeking && (!speedMenu || speedMenu.style.display === 'none')){
      idleTimer = setTimeout(()=>{
        if(!media.paused && !isOverControls && !isDraggingVol && !seeking && (!speedMenu || speedMenu.style.display === 'none')){
          ctrl.classList.add('idle');
          container.classList.add('idle-cursor');
        }
      }, 2500);
    }
  }

  window.addEventListener('mousemove', resetIdle, {passive: true});
  window.addEventListener('mousedown', resetIdle, {passive: true});
  window.addEventListener('touchstart', resetIdle, {passive: true});

  window.addEventListener('keydown', (ev)=>{
    if(ev.target.tagName === 'INPUT' || ev.target.tagName === 'BUTTON') return;
    resetIdle();
    if(ev.key === ' '){
      ev.preventDefault();
      togglePlay();
    }else if((ev.key === 'f' || ev.key === 'F') && !isAudio){
      ev.preventDefault();
      toggleFs();
    }else if(ev.key === 'm' || ev.key === 'M'){
      ev.preventDefault();
      if(media.muted || media.volume === 0){
        media.muted = false;
        if(media.volume === 0) media.volume = (lastVolume > 0 ? lastVolume : 1);
      }else{
        lastVolume = media.volume > 0 ? media.volume : 1;
        media.muted = true;
      }
      updateVolUi();
    }else if(ev.key === 'ArrowLeft'){
      ev.preventDefault();
      media.currentTime = Math.max(0, media.currentTime - 5);
    }else if(ev.key === 'ArrowRight'){
      ev.preventDefault();
      media.currentTime = Math.min(media.duration||0, media.currentTime + 5);
    }else if(ev.key === 'j' || ev.key === 'J'){
      ev.preventDefault();
      media.currentTime = Math.max(0, media.currentTime - 15);
    }else if(ev.key === 'l' || ev.key === 'L'){
      ev.preventDefault();
      media.currentTime = Math.min(media.duration||0, media.currentTime + 15);
    }else if(ev.key === 'ArrowUp'){
      ev.preventDefault();
      media.volume = Math.min(1, Math.round((media.volume + 0.05) * 100) / 100);
      media.muted = false;
      lastVolume = media.volume;
      updateVolUi();
    }else if(ev.key === 'ArrowDown'){
      ev.preventDefault();
      media.volume = Math.max(0, Math.round((media.volume - 0.05) * 100) / 100);
      media.muted = (media.volume === 0);
      if(media.volume > 0) lastVolume = media.volume;
      updateVolUi();
    }
  });

  updatePlayBtn();
  updateVolUi();
  updateProgress();
})();
)JS");
  }

  QString templateHtml = QStringLiteral(R"HTML(<!doctype html><html lang="{{i18n-lang}}" dir="ltr"><head><meta charset="utf-8">
<title>@@TITLE@@</title>
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; media-src file: blob:; img-src data: file: https: http:; style-src 'unsafe-inline'; script-src 'unsafe-inline'">
<style>
html,body{height:100%;margin:0;background:#0d1319;color:#edf3f8;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;overflow-x:hidden}
body{display:grid;place-items:center}
.player-top-header{position:fixed;top:0;left:0;right:0;height:44px;display:flex;align-items:center;justify-content:space-between;padding:0 16px;background:#0f151c;border-bottom:1px solid #1e2836;z-index:100}
.header-filename{font-size:13px;font-weight:500;color:#94a3b8;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:60%}
.effects-btn{display:inline-flex;align-items:center;gap:7px;background:#242e39;color:#e9f0f7;border:1px solid #354352;border-radius:8px;padding:5px 12px;font-size:12px;font-weight:600;cursor:pointer;text-decoration:none;transition:all 0.15s ease}
.effects-btn:hover{background:#2d3a47;border-color:#4a5d70;color:#ffffff}
.shell{width:min(980px,92vw);text-align:center;margin-top:20px}
.art{margin:auto auto 24px;width:220px;height:220px;overflow:hidden;border-radius:24px;background:linear-gradient(145deg,#244b69,#101820);display:grid;place-items:center;font-size:62px;color:#8ac7f8;box-shadow:0 18px 50px rgba(0,0,0,0.5);border:1px solid rgba(255,255,255,0.08)}
.art img{width:100%;height:100%;object-fit:cover}
.title{font-size:20px;font-weight:700;margin:0 0 8px}
.kind{color:#8da1b2;margin:0 0 22px;font-size:13px}
.media-container{position:relative;display:inline-block;margin:auto;width:min(1100px,92vw);border-radius:12px;overflow:hidden;background:#000;box-shadow:0 20px 50px rgba(0,0,0,0.6)}
.media-container video{display:block;width:100%;max-height:74vh;background:#000;outline:none}
.media-container:fullscreen{width:100vw!important;max-width:100vw!important;height:100vh!important;max-height:100vh!important;border-radius:0!important;margin:0!important;box-shadow:none!important}
.media-container:fullscreen video{width:100%!important;height:100%!important;max-height:100vh!important;border-radius:0!important;object-fit:contain}
.audio-mode .media-container{display:block;width:min(760px,92vw);margin:16px auto 0;background:transparent;box-shadow:none;border-radius:14px;overflow:visible}
.audio-mode #media{display:none!important}
.audio-mode #c-pip-btn,.audio-mode #c-fs-btn{display:none!important}
.media-controls-bar{position:absolute;bottom:12px;left:16px;right:16px;height:44px;background:rgba(15,22,32,0.92);backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);border:1px solid rgba(255,255,255,0.14);border-radius:12px;display:flex;align-items:center;gap:8px;padding:0 12px;box-shadow:0 8px 32px rgba(0,0,0,0.65);z-index:20;transition:opacity 0.28s ease,transform 0.28s ease;user-select:none}
.audio-mode .media-controls-bar{position:relative;bottom:auto;left:auto;right:auto;height:48px;border-radius:14px;box-shadow:0 12px 36px rgba(0,0,0,0.5);opacity:1!important;pointer-events:auto!important;transform:none!important}
.media-container:fullscreen .media-controls-bar{bottom:24px;left:32px;right:32px;height:48px}
.video-mode .media-container .media-controls-bar{opacity:1;pointer-events:auto;transform:translateY(0)}
.video-mode .media-container .media-controls-bar.idle{opacity:0!important;pointer-events:none!important;transform:translateY(14px)!important}
.media-container.idle-cursor,.media-container.idle-cursor *{cursor:none!important}
.m-btn{background:transparent;border:none;color:#cbd5e1;width:32px;height:32px;border-radius:8px;display:flex;align-items:center;justify-content:center;cursor:pointer;padding:0;transition:all 0.15s ease}
.m-btn:hover{background:rgba(255,255,255,0.12);color:#ffffff}
.m-play-btn{color:#38bdf8}
.m-play-btn:hover{background:rgba(56,189,248,0.2);color:#ffffff;transform:scale(1.08)}
.m-time{font-size:11px;font-family:monospace,system-ui;color:#94a3b8;min-width:36px;text-align:center}
.m-timeline{flex:1;height:24px;display:flex;align-items:center;cursor:pointer;position:relative}
.m-timeline-track{width:100%;height:4px;background:rgba(255,255,255,0.18);border-radius:2px;position:relative;overflow:visible;transition:height 0.15s ease}
.m-timeline:hover .m-timeline-track{height:6px}
.m-timeline-buffer{position:absolute;top:0;left:0;bottom:0;width:0%;background:rgba(255,255,255,0.28);border-radius:2px}
.m-timeline-played{position:absolute;top:0;left:0;bottom:0;width:0%;background:#38bdf8;border-radius:2px}
.m-timeline-thumb{position:absolute;top:50%;left:0%;width:12px;height:12px;background:#ffffff;border-radius:50%;transform:translate(-50%,-50%) scale(0);box-shadow:0 2px 6px rgba(0,0,0,0.5);transition:transform 0.15s ease}
.m-timeline:hover .m-timeline-thumb{transform:translate(-50%,-50%) scale(1)}
.m-vol-group{display:flex;align-items:center;position:relative}
.m-vol-slider-wrap{width:0;max-width:0;overflow:hidden;display:flex;align-items:center;opacity:0;margin-left:0;margin-right:0;transition:width 0.24s cubic-bezier(0.4,0,0.2,1),max-width 0.24s cubic-bezier(0.4,0,0.2,1),opacity 0.2s ease,margin 0.2s ease}
.m-vol-group:hover .m-vol-slider-wrap,.m-vol-group:focus-within .m-vol-slider-wrap,.m-vol-group.active .m-vol-slider-wrap{width:76px;max-width:76px;opacity:1;margin-left:6px;margin-right:4px}
.m-vol-bar{width:76px;height:24px;padding:0 6px;box-sizing:border-box;display:flex;align-items:center;cursor:pointer;position:relative;outline:none;user-select:none}
.m-vol-track{width:100%;height:4px;background:rgba(255,255,255,0.22);border-radius:2px;position:relative;overflow:visible;transition:height 0.15s ease}
.m-vol-bar:hover .m-vol-track{height:5px}
.m-vol-played{position:absolute;top:0;left:0;bottom:0;width:100%;background:#38bdf8;border-radius:2px}
.m-vol-thumb{position:absolute;top:50%;left:100%;width:12px;height:12px;background:#ffffff;border-radius:50%;transform:translate(-50%,-50%);box-shadow:0 1px 5px rgba(0,0,0,0.6);cursor:pointer;transition:transform 0.12s ease}
.m-vol-bar:hover .m-vol-thumb,.m-vol-group.active .m-vol-thumb{transform:translate(-50%,-50%) scale(1.15)}
.m-settings-group{position:relative}
.m-speed-menu{position:absolute;bottom:42px;right:0;background:#0f1724;border:1px solid rgba(255,255,255,0.15);border-radius:8px;padding:4px 0;min-width:110px;box-shadow:0 8px 24px rgba(0,0,0,0.7);z-index:30}
.m-speed-item{padding:5px 14px;font-size:12px;color:#94a3b8;cursor:pointer;transition:background 0.12s ease,color 0.12s ease}
.m-speed-item:hover{background:rgba(255,255,255,0.1);color:#fff}
.m-speed-item.active{color:#38bdf8;font-weight:600}
video::-webkit-media-controls-panel,audio::-webkit-media-controls-panel{display:none!important}
.video-mode .shell{width:min(1200px,94vw)}
.video-mode .art{position:fixed;top:56px;right:28px;width:180px;height:102px;margin:0;border-radius:12px;z-index:2;font-size:34px;box-shadow:0 10px 30px rgba(0,0,0,0.5);border:1px solid rgba(255,255,255,0.1)}
.video-mode .title{margin-top:0}
.video-mode .kind{margin-bottom:16px}
@media(max-width:900px){.video-mode .art{top:50px;right:14px;width:128px;height:72px}}
#error{color:#ff9aa5;margin-top:16px}
</style></head><body class="@@MEDIA_MODE@@">@@HEADER_HTML@@<main class="shell"><div class="art"><img id="cover" @@COVER_IMG_ATTR@@ alt="{{i18n:media_player.album_cover}}" onerror="this.style.display='none';document.getElementById('cover-placeholder').style.display='grid';"><span id="cover-placeholder" @@COVER_PLACEHOLDER_STYLE@@>@@MEDIA_ICON@@</span></div><h1 class="title">@@TITLE@@</h1><p class="kind">@@SUBTITLE@@</p><div class="media-container" id="media-container"><@@ELEMENT@@ id="media" class="@@MEDIA_CLASS@@" src="@@SOURCE@@" autoplay preload="metadata" playsinline @@CONTROLS_ATTR@@></@@ELEMENT@@>@@CUSTOM_CONTROLS_HTML@@</div><p id="error" hidden>{{i18n:media_player.unsupported_codec}}</p></main><script>
const m=document.getElementById('media'),e=document.getElementById('error');
if(m){
  m.addEventListener('error',()=>e.hidden=false);
  m.play().catch(()=>{});
}
@@CUSTOM_CONTROLS_SCRIPT@@
</script></body></html>)HTML");

  templateHtml.replace(QLatin1String("@@MEDIA_MODE@@"), QStringLiteral("%1-mode").arg(mediaClass));
  templateHtml.replace(QLatin1String("@@HEADER_HTML@@"), headerHtml);
  templateHtml.replace(QLatin1String("@@COVER_IMG_ATTR@@"), coverImgAttr);
  templateHtml.replace(QLatin1String("@@COVER_PLACEHOLDER_STYLE@@"), coverPlaceholderStyle);
  templateHtml.replace(QLatin1String("@@MEDIA_ICON@@"), audio ? QStringLiteral("♫") : QStringLiteral("▶"));
  templateHtml.replace(QLatin1String("@@TITLE@@"), title);
  templateHtml.replace(QLatin1String("@@SUBTITLE@@"), audio ? I18n::text(QStringLiteral("media_player.audio_title")) : I18n::text(QStringLiteral("media_player.video_title")));
  templateHtml.replace(QLatin1String("@@ELEMENT@@"), element);
  templateHtml.replace(QLatin1String("@@MEDIA_CLASS@@"), mediaClass);
  templateHtml.replace(QLatin1String("@@SOURCE@@"), source);
  templateHtml.replace(QLatin1String("@@CONTROLS_ATTR@@"), controlsAttr);
  templateHtml.replace(QLatin1String("@@CUSTOM_CONTROLS_HTML@@"), customControlsHtml);
  templateHtml.replace(QLatin1String("@@CUSTOM_CONTROLS_SCRIPT@@"), customControlsScript);

  return I18n::localizeHtml(templateHtml);
}

QString LocalMediaPlayerPage::createOrUpdateWebKitPlayerHtml(const LocalMediaOpenRequest &request,
                                                             const QString &ffmpegPath) {
  const QString canonicalPath = QFileInfo(request.path).canonicalFilePath();
  if (canonicalPath.isEmpty()) return {};

  QString artworkUrl = request.thumbnailUrl.trimmed();
  if (artworkUrl.isEmpty()) {
    const QFileInfo media(canonicalPath);
    const QByteArray cacheKey = QCryptographicHash::hash(
        media.canonicalFilePath().toUtf8() + '|' + QByteArray::number(media.size()) + '|'
            + QByteArray::number(media.lastModified().toMSecsSinceEpoch()),
        QCryptographicHash::Sha256).toHex();
    const QString cacheDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
               .filePath(QStringLiteral("media-artwork"));
    QDir().mkpath(cacheDirectory);
    const QString cachePath = QDir(cacheDirectory).filePath(QString::fromLatin1(cacheKey) + QStringLiteral(".png"));
    if (QFile::exists(cachePath) && QFileInfo(cachePath).size() > 0) {
      artworkUrl = QUrl::fromLocalFile(cachePath).toString();
    } else {
      QString actualFfmpeg = ffmpegPath;
      if (actualFfmpeg.isEmpty() || !QFileInfo(actualFfmpeg).isExecutable()) {
        actualFfmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
      }
      if (!actualFfmpeg.isEmpty() && QFileInfo(actualFfmpeg).isExecutable()) {
        QProcess proc;
        proc.start(actualFfmpeg, {
            QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"),
            QStringLiteral("-i"), canonicalPath, QStringLiteral("-map"), QStringLiteral("0:v:0?"),
            QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-an"),
            QStringLiteral("-vf"), QStringLiteral("scale=512:512:force_original_aspect_ratio=decrease"),
            QStringLiteral("-y"), cachePath});
        if (proc.waitForFinished(2000) && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
          if (QFile::exists(cachePath) && QFileInfo(cachePath).size() > 0) {
            artworkUrl = QUrl::fromLocalFile(cachePath).toString();
          }
        }
      }
    }
  }

  const QUrl mediaUrl = QUrl::fromLocalFile(canonicalPath);
  const QString html = playerHtml(request, mediaUrl, true /* includeHeader */, artworkUrl, true /* customControls */);

  const QString playerDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
             .filePath(QStringLiteral("media-player"));
  QDir().mkpath(playerDirectory);
  const QByteArray pathHash = QCryptographicHash::hash(canonicalPath.toUtf8(), QCryptographicHash::Sha256).toHex();
  const QString htmlFilePath = QDir(playerDirectory).filePath(QString::fromLatin1(pathHash) + QStringLiteral(".html"));

  QFile file(htmlFilePath);
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    file.write(html.toUtf8());
    file.close();
  }
  return htmlFilePath;
}

LocalMediaPlayerPage::LocalMediaPlayerPage(const LocalMediaOpenRequest &request,
                                           QWebEngineProfile *profile,
                                           WebAudioEffectsController *audioEffects,
                                           const QString &ffmpegPath,
                                           QWidget *parent)
    : QWidget(parent), mediaPath_(QFileInfo(request.path).canonicalFilePath()),
      ffmpegPath_(ffmpegPath), playerKind_(request.playerKind), audioEffects_(audioEffects) {
  if (profile && profile->isOffTheRecord()) {
    privateArtworkDirectory_ = std::make_unique<QTemporaryDir>();
  }
  setObjectName(QStringLiteral("local-media-player-page"));
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto *header = new QWidget(this);
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(16, 10, 16, 10);
  fileLabel_ = new QLabel(header);
  headerLayout->addWidget(fileLabel_, 1);
  effectsButton_ = new QPushButton(BrowserIcons::icon(BrowserIcon::Tune),
                                   I18n::text(QStringLiteral("menu.audio_effects")), header);
  connect(effectsButton_, &QPushButton::clicked,
          this, &LocalMediaPlayerPage::audioEffectsRequested);
  headerLayout->addWidget(effectsButton_);
  header->hide();  // The unified HTML header is displayed inside the page for both WebKit & Chromium
  layout->addWidget(header);

  view_ = new QWebEngineView(this);
  auto *page = new LockedLocalMediaPage(profile ? profile : QWebEngineProfile::defaultProfile(), view_);
  view_->setPage(page);
  connect(page, &LockedLocalMediaPage::audioEffectsRequested,
          this, &LocalMediaPlayerPage::audioEffectsRequested);
  connect(view_, &QWebEngineView::loadFinished, this, [this](bool ok) {
    if (ok) updateArtworkInDocument();
  });
  page->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
  page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
  page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
  page->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
  connect(page, &QWebEnginePage::fullScreenRequested, this, [this](QWebEngineFullScreenRequest request) {
    request.accept();
    BrowserWindow *bw = qobject_cast<BrowserWindow *>(window());
    if (!bw) {
      for (QWidget *p = parentWidget(); p; p = p->parentWidget()) {
        if (auto *candidate = qobject_cast<BrowserWindow *>(p)) {
          bw = candidate;
          break;
        }
      }
    }
    if (request.toggleOn()) {
      mediaFullScreen_ = true;
      if (bw) {
        bw->setPageFullScreen(true);
      } else if (QWidget *top = window()) {
        top->showFullScreen();
      }
      return;
    }
    if (!mediaFullScreen_) return;
    mediaFullScreen_ = false;
    if (bw) {
      bw->setPageFullScreen(false);
    } else if (QWidget *top = window()) {
      top->showNormal();
    }
  });
  layout->addWidget(view_, 1);
  loadMedia(request);
}

LocalMediaPlayerPage::~LocalMediaPlayerPage() {
  ++artworkGeneration_;
  if (artworkReply_) artworkReply_->abort();
  if (artworkProcess_ && artworkProcess_->state() != QProcess::NotRunning) artworkProcess_->kill();
  if (audioEffects_ && view_) audioEffects_->unregisterWebView(view_);
}

void LocalMediaPlayerPage::loadMedia(const LocalMediaOpenRequest &request) {
  const QString canonicalPath = QFileInfo(request.path).canonicalFilePath();
  if (canonicalPath.isEmpty() || request.playerKind == LocalMediaPlayerKind::External) return;

  mediaPath_ = canonicalPath;
  playerKind_ = request.playerKind;
  fileLabel_->setText(QFileInfo(mediaPath_).fileName());
  fileLabel_->setToolTip(mediaPath_);
  effectsButton_->setVisible(true);

  const QUrl mediaUrl = QUrl::fromLocalFile(mediaPath_);
  view_->setProperty("dalinira-trusted-local-media", true);
  if (audioEffects_) audioEffects_->registerWebView(view_, mediaUrl);
  auto *page = static_cast<LockedLocalMediaPage *>(view_->page());
  page->prepareTrustedDocument(mediaUrl);
  view_->stop();

  QString initialArtworkUrl;
  const QByteArray cacheKey = QCryptographicHash::hash(
      canonicalPath.toUtf8() + '|' + QByteArray::number(QFileInfo(canonicalPath).size()) + '|'
          + QByteArray::number(QFileInfo(canonicalPath).lastModified().toMSecsSinceEpoch()),
      QCryptographicHash::Sha256).toHex();
  const QString cacheDirectory = privateArtworkDirectory_ && privateArtworkDirectory_->isValid()
      ? privateArtworkDirectory_->path()
      : QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("media-artwork"));
  const QString cachePath = QDir(cacheDirectory).filePath(QString::fromLatin1(cacheKey) + QStringLiteral(".png"));
  if (QFile::exists(cachePath) && QFileInfo(cachePath).size() > 0) {
    initialArtworkUrl = QUrl::fromLocalFile(cachePath).toString();
  }

  view_->setHtml(playerHtml(request, mediaUrl, true /* includeHeader */, initialArtworkUrl,
                            true /* customControls */), mediaUrl);
  requestArtwork(request);
}

void LocalMediaPlayerPage::requestArtwork(const LocalMediaOpenRequest &request) {
  const quint64 generation = ++artworkGeneration_;
  artworkData_.clear();
  if (artworkProcess_) {
    artworkProcess_->kill();
    artworkProcess_->deleteLater();
    artworkProcess_ = nullptr;
  }
  if (artworkReply_) {
    artworkReply_->abort();
    artworkReply_->deleteLater();
    artworkReply_ = nullptr;
  }

  const QFileInfo media(mediaPath_);
  const QByteArray cacheKey = QCryptographicHash::hash(
      media.canonicalFilePath().toUtf8() + '|' + QByteArray::number(media.size()) + '|'
          + QByteArray::number(media.lastModified().toMSecsSinceEpoch()),
      QCryptographicHash::Sha256).toHex();
  const QString cacheDirectory = privateArtworkDirectory_ && privateArtworkDirectory_->isValid()
      ? privateArtworkDirectory_->path()
      : QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("media-artwork"));
  QDir().mkpath(cacheDirectory);
  const QString cachePath = QDir(cacheDirectory).filePath(QString::fromLatin1(cacheKey) + QStringLiteral(".png"));
  QFile cached(cachePath);
  if (cached.open(QIODevice::ReadOnly) && cached.size() <= 5 * 1024 * 1024) {
    if (setArtworkData(cached.readAll(), generation)) return;
    cached.close();
    QFile::remove(cachePath);
  }

  const QFileInfo ffmpeg(ffmpegPath_);
  if (!ffmpeg.isAbsolute() || !ffmpeg.isExecutable()) {
    requestRemoteArtwork(request.thumbnailUrl, generation);
    return;
  }
  artworkProcess_ = new QProcess(this);
  QProcess *process = artworkProcess_;
  process->setProgram(ffmpeg.absoluteFilePath());
  process->setArguments({QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"),
      QStringLiteral("-i"), mediaPath_, QStringLiteral("-map"), QStringLiteral("0:v:0?"),
      QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-an"),
      QStringLiteral("-vf"), QStringLiteral("scale=512:512:force_original_aspect_ratio=decrease"),
      QStringLiteral("-y"), cachePath});
  connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, process, cachePath, thumbnailUrl = request.thumbnailUrl, generation]
          (int exitCode, QProcess::ExitStatus status) {
    if (artworkProcess_ == process) artworkProcess_ = nullptr;
    process->deleteLater();
    if (generation != artworkGeneration_) return;
    QFile extracted(cachePath);
    if (status == QProcess::NormalExit && exitCode == 0 && extracted.open(QIODevice::ReadOnly)
        && extracted.size() > 0 && extracted.size() <= 5 * 1024 * 1024) {
      QFile::setPermissions(cachePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      setArtworkData(extracted.readAll(), generation);
      return;
    }
    QFile::remove(cachePath);
    requestRemoteArtwork(thumbnailUrl, generation);
  });
  connect(process, &QProcess::errorOccurred, this,
          [this, process, thumbnailUrl = request.thumbnailUrl, generation](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart || artworkProcess_ != process) return;
    artworkProcess_ = nullptr;
    process->deleteLater();
    requestRemoteArtwork(thumbnailUrl, generation);
  });
  process->start();
}

void LocalMediaPlayerPage::requestRemoteArtwork(const QString &thumbnailUrl, quint64 generation) {
  const QUrl url(thumbnailUrl);
  if (generation != artworkGeneration_ || !url.isValid() || !url.userInfo().isEmpty()
      || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) return;
  if (!artworkNetwork_) artworkNetwork_ = new QNetworkAccessManager(this);
  QNetworkRequest networkRequest(url);
  networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                              QNetworkRequest::NoLessSafeRedirectPolicy);
  networkRequest.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
  networkRequest.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
  artworkReply_ = artworkNetwork_->get(networkRequest);
  QNetworkReply *reply = artworkReply_;
  connect(reply, &QNetworkReply::readyRead, this, [reply] {
    if (reply->bytesAvailable() > 5 * 1024 * 1024) reply->abort();
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
    if (artworkReply_ == reply) artworkReply_ = nullptr;
    const QByteArray bytes = reply->error() == QNetworkReply::NoError
        && reply->bytesAvailable() <= 5 * 1024 * 1024 ? reply->readAll() : QByteArray{};
    reply->deleteLater();
    setArtworkData(bytes, generation);
  });
}

bool LocalMediaPlayerPage::setArtworkData(const QByteArray &bytes, quint64 generation) {
  if (generation != artworkGeneration_ || bytes.isEmpty()) return false;
  QBuffer source;
  source.setData(bytes);
  source.open(QIODevice::ReadOnly);
  QImageReader reader(&source);
  const QSize originalSize = reader.size();
  if (!originalSize.isValid() || originalSize.width() > 8192 || originalSize.height() > 8192) return false;
  reader.setScaledSize(originalSize.scaled(QSize(512, 512), Qt::KeepAspectRatio));
  const QImage image = reader.read();
  if (image.isNull()) return false;
  QByteArray png;
  QBuffer destination(&png);
  destination.open(QIODevice::WriteOnly);
  if (!image.save(&destination, "PNG")) return false;
  artworkData_ = png;
  updateArtworkInDocument();
  return true;
}

void LocalMediaPlayerPage::updateArtworkInDocument() {
  if (!view_ || !view_->page() || artworkData_.isEmpty()) return;
  const QString dataUrl = QStringLiteral("data:image/png;base64,")
      + QString::fromLatin1(artworkData_.toBase64());
  QByteArray literal = QJsonDocument(QJsonArray{dataUrl}).toJson(QJsonDocument::Compact);
  literal = literal.mid(1, literal.size() - 2);
  view_->page()->runJavaScript(QStringLiteral(R"JS((function(src){
    const cover=document.getElementById('cover');
    const placeholder=document.getElementById('cover-placeholder');
    if(!cover||!placeholder)return false;
    cover.src=src;cover.hidden=false;cover.style.display='block';placeholder.style.display='none';placeholder.hidden=true;return true;
  })(%1))JS").arg(QString::fromUtf8(literal)));
}

#include "local_media_player_page.moc"

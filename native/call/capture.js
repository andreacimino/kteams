// Protocol capture for kollega-capture (only with --capture): records what the Teams web app
// exchanges during a call, to design a native implementation. Injected before the page
// scripts. Lines go to the console as "ACCAPTURE <json>"; main.cpp writes them to a file.
// Secrets are masked here, before anything leaves the page.
(function () {
  if (window.__acCapture) return;
  window.__acCapture = true;

  var currentUrl = '';
  function mask(text) {
    if (typeof text !== 'string') {
      try { text = JSON.stringify(text); } catch (e) { return '<unserializable>'; }
    }
    if (!text) return text;
    return text
      // JWT / JWE and any other base64 JSON blob
      .replace(/eyJ[A-Za-z0-9_.\-]{20,}/g, '<jwt>')
      // JSON fields holding secrets
      .replace(/("[A-Za-z_]*(?:token|credential|password|secret|code)[A-Za-z_]*"\s*:\s*")[^"]*"/gi, '$1<masked>"')
      .replace(/(\\"[A-Za-z_]*(?:token|credential|password|secret|code)[A-Za-z_]*\\"\s*:\s*\\")(?:[^"\\]|\\[^"])*\\"/gi, '$1<masked>\\"')
      // query / form parameters holding secrets
      .replace(/([?&#](?:code|client_info|state|sig|[A-Za-z_]*token[A-Za-z_]*)=)[^&"#\s]+/gi, '$1<masked>')
      // call signaling (SDP inside) is kept whole; everything else is trimmed
      .slice(0, /flightproxy|skype\.com|trap\/tokens/.test(String(currentUrl || '')) ? 400000 : 20000);
  }
  function emit(kind, data) {
    try { console.log('ACCAPTURE ' + JSON.stringify({ t: Date.now(), kind: kind, data: data })); } catch (e) {}
  }
  function interesting(url) {
    return /skype\.com|teams\.microsoft\.com|cloud\.microsoft|trouter|flightproxy|office\.com/.test(url) &&
           !/\.(js|css|png|jpg|svg|woff2?|ico|json)(\?|$)/.test(url);
  }

  // fetch: request and response bodies
  var origFetch = window.fetch;
  if (origFetch) {
    window.fetch = function (input, init) {
      var url = String((typeof input === 'string' || input instanceof URL) ? input : (input && input.url) || '');
      var method = (init && init.method) || (input && input.method) || 'GET';
      var body = init && init.body;
      var p = origFetch.apply(this, arguments);
      if (interesting(url)) {
        p.then(function (resp) {
          resp.clone().text().then(function (text) {
            currentUrl = url;
            emit('fetch', { method: method, url: mask(url), reqBody: typeof body === 'string' ? mask(body) : (body ? '<' + typeof body + '>' : null),
                            status: resp.status, respBody: mask(text) });
          }).catch(function () {});
        }).catch(function () {});
      }
      return p;
    };
  }

  // XMLHttpRequest
  var origOpen = XMLHttpRequest.prototype.open, origSend = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function (method, url) { this.__ac = { method: method, url: String(url) }; return origOpen.apply(this, arguments); };
  XMLHttpRequest.prototype.send = function (body) {
    var x = this;
    if (x.__ac && interesting(x.__ac.url)) {
      x.addEventListener('loadend', function () {
        var text = '';
        currentUrl = x.__ac.url;
        try { text = (x.responseType === '' || x.responseType === 'text') ? x.responseText : '<' + x.responseType + '>'; } catch (e) {}
        emit('xhr', { method: x.__ac.method, url: mask(x.__ac.url), reqBody: typeof body === 'string' ? mask(body) : null,
                      status: x.status, respBody: mask(text) });
      });
    }
    return origSend.apply(this, arguments);
  };

  // WebSocket (Trouter and others)
  var OrigWS = window.WebSocket;
  if (OrigWS) {
    var WS = function (url, protocols) {
      var ws = protocols !== undefined ? new OrigWS(url, protocols) : new OrigWS(url);
      emit('ws-open', { url: mask(String(url)) });
      ws.addEventListener('message', function (ev) { emit('ws-in', { url: mask(String(url)), data: mask(typeof ev.data === 'string' ? ev.data : '<binary>') }); });
      var send = ws.send;
      ws.send = function (data) { emit('ws-out', { url: mask(String(url)), data: mask(typeof data === 'string' ? data : '<binary>') }); return send.apply(ws, arguments); };
      return ws;
    };
    WS.prototype = OrigWS.prototype;
    ['CONNECTING', 'OPEN', 'CLOSING', 'CLOSED'].forEach(function (k) { WS[k] = OrigWS[k]; });
    window.WebSocket = WS;
  }

  // WebRTC: configuration (TURN credentials masked), SDP, ICE candidates, state changes.
  var OrigPC = window.RTCPeerConnection;
  if (OrigPC) {
    var counter = 0;
    var PC = function (config, constraints) {
      var pc = new OrigPC(config, constraints);
      var id = ++counter;
      var cfg = config ? JSON.parse(JSON.stringify(config)) : null;
      if (cfg && cfg.iceServers) cfg.iceServers.forEach(function (s) { if (s.credential) s.credential = '<masked>'; if (s.username) s.username = '<masked>'; });
      emit('pc-new', { pc: id, config: cfg });
      function wrap(name, describe) {
        var orig = pc[name];
        pc[name] = function () {
          var args = arguments;
          try { emit('pc-' + name, { pc: id, arg: describe ? describe(args) : null }); } catch (e) {}
          var r = orig.apply(pc, args);
          if (r && r.then && (name === 'createOffer' || name === 'createAnswer'))
            r.then(function (d) { emit('pc-' + name + '-result', { pc: id, type: d && d.type, sdp: d && d.sdp }); });
          return r;
        };
      }
      wrap('createOffer'); wrap('createAnswer');
      wrap('setLocalDescription', function (a) { return a[0] ? { type: a[0].type, sdp: a[0].sdp } : null; });
      wrap('setRemoteDescription', function (a) { return a[0] ? { type: a[0].type, sdp: a[0].sdp } : null; });
      wrap('addIceCandidate', function (a) { return a[0] ? { candidate: a[0].candidate, sdpMid: a[0].sdpMid } : null; });
      wrap('addTransceiver', function (a) { return { kind: typeof a[0] === 'string' ? a[0] : (a[0] && a[0].kind), init: a[1] ? JSON.stringify(a[1]) : null }; });
      wrap('createDataChannel', function (a) { return { label: a[0], options: a[1] ? JSON.stringify(a[1]) : null }; });
      pc.addEventListener('icecandidate', function (ev) { emit('pc-local-candidate', { pc: id, candidate: ev.candidate && ev.candidate.candidate }); });
      pc.addEventListener('connectionstatechange', function () { emit('pc-state', { pc: id, state: pc.connectionState }); });
      pc.addEventListener('track', function (ev) { emit('pc-track', { pc: id, kind: ev.track && ev.track.kind }); });
      pc.addEventListener('datachannel', function (ev) {
        var ch = ev.channel; emit('pc-datachannel', { pc: id, label: ch.label });
        ch.addEventListener('message', function (m) { emit('dc-in', { pc: id, label: ch.label, data: mask(typeof m.data === 'string' ? m.data : '<binary>') }); });
      });
      return pc;
    };
    PC.prototype = OrigPC.prototype;
    PC.generateCertificate = OrigPC.generateCertificate;
    window.RTCPeerConnection = PC;
  }
  emit('capture-ready', { url: location.origin + location.pathname });
})();

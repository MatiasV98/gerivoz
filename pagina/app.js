// GeriVoz · Central
// Lee por Web Serial lo que imprime el ESP32 receptor y arma el panel.
//
//   << PULSO de nodo N ...     -> botón de mensajes, 1 toque:  ATENCIÓN GENERAL
//   << ALERTA del nodo N ...   -> botón de mensajes, 2 toques: EMERGENCIA
//   [AUDIO] Sesion nueva ...   -> botón PTT: LLAMADA DE VOZ, empieza a entrar voz
//   [AUDIO] Fin | recibidos=.. -> termina la llamada de voz (paquetes recibidos y perdidos)

// ============ CONFIGURACIÓN ============
// Nombre de la habitación de cada nodo (NODE_ID del firmware).
// La base es NODE_ID 0 (entorno 'base'); los handies 1..18 (entornos 'handyN').
const NODOS = {
  1: 'Residente 1 · Handy 1',
  2: 'Residente 2 · Handy 2',
};

// ============ ESTADO ============
const events = [];          // todo lo recibido, el más nuevo primero
let selectedId = null;
let nextId = 1;
let voice = null;           // emergencia en curso: { ev, t0 }
let lastVoiceEvent = null;  // última emergencia cerrada (para completarle datos)
let lastNode = null;
let muted = false;

// Audio de las emergencias: vive en la MEMORIA de esta pestaña (no se escribe a disco).
// Se pierde al recargar o cerrar la página; para conservarlo está el botón de descarga .wav.
const RATE = 8000;                 // muestras por segundo que manda el firmware (ya decodificadas)
const MAX_SAMPLES = RATE * 120;    // tope de 2 minutos por emergencia
let playingId = null;              // emergencia que se está escuchando ahora
const player = new Audio();
player.addEventListener('ended', () => { playingId = null; render(); });

// En el firmware los contadores de audio son ACUMULADOS (solo se ponen en cero con 'c').
// Se guarda el total anterior para sacar los paquetes de cada emergencia por separado.
let prevRec = 0, prevPerd = 0;

const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const label = n => n == null ? 'Nodo sin identificar' : (NODOS[n] || ('Nodo ' + n));

const KIND = { emergency: 'Emergencia', attention: 'Atención general', voice: 'Llamada de voz' };
const ICON = { emergency: '🚨', attention: '🔔', voice: '🎙️' };
const PRIO = { emergency: 3, voice: 2, attention: 1 };   // orden en la lista de pendientes

const MUTED_TXT = 'text-slate-500 dark:text-slate-400';
const ROW = {
  emergency: 'border-red-300 bg-red-50 dark:border-red-900 dark:bg-red-950/50 ring-pulse',
  attention: 'border-slate-200 bg-white hover:bg-slate-50 dark:border-slate-700 dark:bg-slate-900 dark:hover:bg-slate-800',
  voice: 'border-blue-200 bg-blue-50 hover:bg-blue-100 dark:border-blue-900 dark:bg-blue-950/40 dark:hover:bg-blue-950/60',
};
const SEL = { emergency: '!border-red-500', attention: '!border-blue-500', voice: '!border-blue-600' };
const ICONBOX = {
  emergency: 'bg-red-600 text-white',
  attention: 'bg-amber-100 text-amber-600 dark:bg-amber-500/15 dark:text-amber-400',
  voice: 'bg-blue-600 text-white',
};
const TAG = {
  emergency: 'bg-red-100 text-red-700 dark:bg-red-500/15 dark:text-red-400',
  attention: 'bg-amber-100 text-amber-700 dark:bg-amber-500/15 dark:text-amber-400',
  voice: 'bg-blue-100 text-blue-700 dark:bg-blue-500/15 dark:text-blue-300',
};

function hhmmss(d) { return d.toLocaleTimeString('es-AR', { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false }); }
function ago(d) {
  const s = Math.round((Date.now() - d.getTime()) / 1000);
  if (s < 10) return 'ahora';
  if (s < 60) return 'hace ' + s + ' s';
  return 'hace ' + Math.round(s / 60) + ' min';
}
function senal(rssi) {
  if (rssi == null) return '—';
  const q = rssi > -70 ? 'buena' : rssi > -90 ? 'regular' : 'débil';
  return rssi + ' dBm (' + q + ')';
}
// Calidad del enlace según el % de paquetes perdidos.
function calidad(p) {
  if (p == null) return { t: '—', c: MUTED_TXT };
  if (p < 2) return { t: 'buena', c: 'text-emerald-600 dark:text-emerald-400' };
  if (p < 10) return { t: 'regular', c: 'text-amber-600 dark:text-amber-400' };
  return { t: 'mala', c: 'text-red-600 dark:text-red-400' };
}
function fmtDur(e) {
  const s = (voice && voice.ev === e) ? (Date.now() - voice.t0) / 1000 : (e.dur || 0);
  return s.toFixed(1) + ' s';
}
function toast(msg) {
  $('toast').textContent = msg;
  $('toastBox').classList.remove('hidden');
  clearTimeout(toast.h);
  toast.h = setTimeout(() => $('toastBox').classList.add('hidden'), 4500);
}

// ============ TEMA ============
function applyTheme(dark) {
  document.documentElement.classList.toggle('dark', dark);
  $('btnTema').textContent = dark ? '☀️ Claro' : '🌙 Oscuro';
  try { localStorage.setItem('tema', dark ? 'dark' : 'light'); } catch (e) {}
}
$('btnTema').onclick = () => applyTheme(!document.documentElement.classList.contains('dark'));
$('btnTema').textContent = document.documentElement.classList.contains('dark') ? '☀️ Claro' : '🌙 Oscuro';

// ============ SONIDO ============
let actx = null;
function initAudio() {
  try {
    actx = actx || new (window.AudioContext || window.webkitAudioContext)();
    if (actx.state === 'suspended') actx.resume();
  } catch (e) {}
}
function beep(freq, veces) {
  if (muted || !actx) return;
  try {
    for (let i = 0; i < veces; i++) {
      const o = actx.createOscillator(), g = actx.createGain();
      o.frequency.value = freq; o.connect(g); g.connect(actx.destination);
      const t = actx.currentTime + i * 0.3;
      g.gain.setValueAtTime(0.0001, t);
      g.gain.exponentialRampToValueAtTime(0.25, t + 0.02);
      g.gain.exponentialRampToValueAtTime(0.0001, t + 0.2);
      o.start(t); o.stop(t + 0.22);
    }
  } catch (e) {}
}
$('btnSonido').onclick = () => {
  muted = !muted;
  $('btnSonido').textContent = muted ? '🔕 Silenciado' : '🔔 Sonido';
  if (!muted) { initAudio(); beep(660, 1); }
};

// ============ EVENTOS ============
function addEvent(ev) {
  ev.id = nextId++; ev.time = new Date(); ev.resolved = false;
  events.unshift(ev);
  if (ev.node != null) lastNode = ev.node;
  if (ev.kind === 'emergency') beep(880, 3);
  else if (ev.kind === 'voice') beep(760, 2);
  else beep(660, 1);
  if (selectedId == null || ev.kind !== 'attention') selectedId = ev.id;
  render();
  return ev;
}

function num(line, re) { const m = line.match(re); return m ? +m[1] : null; }
function delta(cur, prev) { return cur >= prev ? cur - prev : cur; } // si el total bajó, la placa se reinició

function startEmergency(node) {
  if (voice) voice.ev.live = false; // quedó una sesión sin cerrar: se da por terminada
  const ev = addEvent({ kind: 'voice', node: node != null ? node : lastNode, live: true, msg: 'Recibiendo voz…' });
  voice = { ev, t0: Date.now(), tUlt: Date.now(), chunks: [], nSamples: 0, lastSeq: null, frames: 0, nodoOk: false };
}

function endEmergency(recTot, perTot) {
  let ev, dur;
  if (voice) { ev = voice.ev; dur = (Date.now() - voice.t0) / 1000; }
  else { ev = addEvent({ kind: 'voice', node: lastNode }); dur = 0; } // se perdió el inicio de la sesión
  const r = delta(recTot, prevRec), p = delta(perTot, prevPerd);
  prevRec = recTot; prevPerd = perTot;
  ev.live = false;
  ev.dur = dur;
  ev.recibidos = r;
  ev.perdidos = p;
  ev.loss = (r + p) > 0 ? 100 * p / (r + p) : 0;
  ev.msg = r + ' paquetes · ' + ev.loss.toFixed(1) + ' % perdidos';
  ev.audio = (voice && voice.nSamples > 0) ? armarAudio(voice) : null;
  voice = null;
  lastVoiceEvent = ev;
  render();
}

// ============ AUDIO ============
// Llega una trama por cada paquete de radio (104 muestras = 13 ms), ya decodificada por el ESP32.
function handleAudioFrame(nodo, seq, samples) {
  if (!voice) return;               // trama fuera de una emergencia: se ignora
  const v = voice;
  v.frames++;
  v.tUlt = Date.now();
  if (!v.nodoOk) {                  // la trama trae el nodo real: es más confiable que el último pulso
    v.nodoOk = true; v.ev.node = nodo; lastNode = nodo; render();
  }
  if (v.lastSeq != null) {
    const gap = (seq - v.lastSeq - 1) & 0xFFFF;   // paquetes que no llegaron por radio
    if (gap > 0 && gap < 1000 && v.nSamples < MAX_SAMPLES) {
      v.chunks.push(new Int16Array(gap * samples.length)); // se rellenan con silencio para no acelerar el audio
      v.nSamples += gap * samples.length;
    }
  }
  v.lastSeq = seq;
  if (v.nSamples < MAX_SAMPLES) { v.chunks.push(samples); v.nSamples += samples.length; }
}

// Junta los pedazos y arma un WAV (mono, 16 bits, 8 kHz) que el navegador puede reproducir y descargar.
function armarAudio(v) {
  const bytes = 44 + v.nSamples * 2;
  const buf = new ArrayBuffer(bytes), dv = new DataView(buf);
  const txt = (o, s) => { for (let i = 0; i < s.length; i++) dv.setUint8(o + i, s.charCodeAt(i)); };
  txt(0, 'RIFF'); dv.setUint32(4, bytes - 8, true); txt(8, 'WAVE'); txt(12, 'fmt ');
  dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, 1, true);
  dv.setUint32(24, RATE, true); dv.setUint32(28, RATE * 2, true); dv.setUint16(32, 2, true); dv.setUint16(34, 16, true);
  txt(36, 'data'); dv.setUint32(40, v.nSamples * 2, true);
  let off = 44;
  for (const c of v.chunks) for (let i = 0; i < c.length; i++) { dv.setInt16(off, c[i], true); off += 2; }
  const blob = new Blob([buf], { type: 'audio/wav' });
  return { url: URL.createObjectURL(blob), secs: v.nSamples / RATE, samples: v.nSamples, bytes };
}

function togglePlay(id) {
  const e = events.find(x => x.id === id);
  if (!e || !e.audio) return;
  if (playingId === id) { player.pause(); player.currentTime = 0; playingId = null; render(); return; }
  player.pause();
  player.src = e.audio.url;
  playingId = id;
  render();
  player.play().catch(err => { playingId = null; render(); toast('No se pudo reproducir: ' + err.message); });
}

function playBtn(e, big) {
  const base = 'flex-none rounded-xl font-bold ' + (big ? 'px-5 py-3 text-base ' : 'px-3 py-2 text-sm ');
  const off = base + 'cursor-not-allowed bg-slate-200 text-slate-500 dark:bg-slate-800 dark:text-slate-400';
  if (e.live) return `<button disabled class="${off}">● Recibiendo</button>`;
  if (!e.audio) return `<button disabled title="El firmware no mandó audio por el puerto serie" class="${off}">Sin audio</button>`;
  return `<button data-play="${e.id}" class="${base}bg-red-600 text-white hover:bg-red-700">${playingId === e.id ? '⏸ Detener' : '▶ Escuchar voz'}</button>`;
}

function audioBox(e) {
  const nota = 'mb-4 rounded-xl border px-4 py-3 text-sm ';
  if (e.live) return `<div class="${nota}border-slate-200 ${MUTED_TXT} dark:border-slate-700">Se está recibiendo la voz. Vas a poder escucharla cuando termine.</div>`;
  if (!e.audio) return `<div class="${nota}border-amber-300 bg-amber-50 text-amber-900 dark:border-amber-700 dark:bg-amber-950/40 dark:text-amber-200">Esta llamada no tiene audio en la página: el firmware no lo mandó por el puerto serie.</div>`;
  const name = ('voz_' + label(e.node) + '_' + hhmmss(e.time)).normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[\s:]+/g, '-') + '.wav';
  return `<div class="mb-2 flex flex-wrap items-center gap-2">${playBtn(e, true)}
      <a href="${e.audio.url}" download="${esc(name)}" class="rounded-xl border border-slate-200 bg-white px-4 py-3 font-semibold hover:bg-slate-50 dark:border-slate-700 dark:bg-slate-900 dark:hover:bg-slate-800">⬇ Descargar .wav</a>
      <span class="text-sm ${MUTED_TXT}">${e.audio.secs.toFixed(1)} s de audio</span></div>
    <p class="mb-4 text-xs ${MUTED_TXT}">El audio queda en la memoria de esta pestaña: si la recargás o la cerrás se pierde. Descargalo para guardarlo.</p>`;
}

// ============ PARSER DEL PUERTO (texto + tramas de audio) ============
// El ESP32 manda por el mismo cable texto ASCII terminado en \n y tramas binarias de audio:
//   A5 5A C3 | nodo | seq (2, LE) | n (2, LE) | n muestras int16 LE | xor de todos los bytes después del sync
const SYNC0 = 0xA5, SYNC1 = 0x5A, SYNC2 = 0xC3, MAX_N = 512;
let acc = new Uint8Array(0);
let lineBytes = [];
let tramasMalas = 0;
const textDec = new TextDecoder();

function textByte(b) {
  if (b === 10) { const l = textDec.decode(Uint8Array.from(lineBytes)); lineBytes = []; seguro(() => handleLine(l)); }
  else if (lineBytes.length < 4000) lineBytes.push(b);
  else lineBytes = [];
}

// Envoltorio: si algo falla al procesar una linea o una trama, se registra y se
// sigue. Sin esto, la excepcion cortaba feed() antes de descartar los bytes ya
// leidos, y en cada lectura siguiente se volvian a procesar y fallaba de nuevo:
// la pagina quedaba trabada.
function seguro(fn) {
  try { fn(); } catch (e) { console.error(e); try { addLog('!! error en la pagina: ' + e.message); } catch (_) {} }
}

function feed(chunk) {
  const merged = new Uint8Array(acc.length + chunk.length);
  merged.set(acc); merged.set(chunk, acc.length);
  acc = merged;
  let i = 0;
  while (i < acc.length) {
    if (acc[i] === SYNC0) {
      if (acc.length - i < 8) break;                      // falta el resto del encabezado
      if (acc[i + 1] === SYNC1 && acc[i + 2] === SYNC2) {
        const n = acc[i + 6] | (acc[i + 7] << 8);
        if (n > 0 && n <= MAX_N) {
          const total = 9 + 2 * n;
          if (acc.length - i < total) break;              // la trama todavía no llegó entera
          let x = 0;
          for (let k = i + 3; k < i + total - 1; k++) x ^= acc[k];
          if (x === acc[i + total - 1]) {
            const s = new Int16Array(n);
            const dv = new DataView(acc.buffer, acc.byteOffset + i + 8, 2 * n);
            for (let j = 0; j < n; j++) s[j] = dv.getInt16(2 * j, true);
            const nodoF = acc[i + 3], seqF = acc[i + 4] | (acc[i + 5] << 8);
            seguro(() => handleAudioFrame(nodoF, seqF, s));
            i += total; continue;
          }
          tramasMalas++;                                  // checksum mal: se descarta
        }
      }
    }
    textByte(acc[i]); i++;
  }
  acc = acc.slice(i);
}

// Interpreta las líneas de texto que imprime el firmware del receptor.
function handleLine(raw) {
  const line = raw.replace(/\r/g, '').trim();
  if (!line) return;
  addLog(line);
  let m;

  if ((m = line.match(/PULSO de nodo (\d+)/i))) {
    addEvent({ node: +m[1], kind: 'attention', rssi: num(line, /RSSI=(-?\d+)/), lqi: num(line, /LQI=(\d+)/), msg: 'Pidió atención (no urgente)' });
    return;
  }
  if ((m = line.match(/ALERTA del nodo (\d+)/i))) {
    addEvent({ node: +m[1], kind: 'emergency', rssi: num(line, /RSSI=(-?\d+)/), lqi: num(line, /LQI=(\d+)/), msg: 'Requiere asistencia inmediata' });
    return;
  }
  if (/\[AUDIO\]\s*Sesi[oó]n nueva/i.test(line)) {
    startEmergency(num(line, /nodo (\d+)/i));
    return;
  }
  if ((m = line.match(/\[AUDIO\]\s*Fin\s*\|\s*recibidos=(\d+)\s+perdidos=(\d+)/i))) {
    endEmergency(+m[1], +m[2]);
    return;
  }
  // Las dos líneas siguientes llegan después de "Fin": completan la última emergencia.
  if ((m = line.match(/Reproducidas \d+ muestras = ([\d.]+) s de audio.*?\(([\d.]+)x/i)) && lastVoiceEvent) {
    lastVoiceEvent.dur = +m[1];
    lastVoiceEvent.ratio = +m[2];
    render();
    return;
  }
  if (/LLEGA SILENCIO/i.test(line) && lastVoiceEvent) {
    lastVoiceEvent.silencio = true;
    render();
  }
}

// ============ RENDER ============
function active() {
  return events.filter(e => !e.resolved)
    .sort((a, b) => PRIO[b.kind] - PRIO[a.kind] || b.time - a.time);
}

function renderStats() {
  const act = active();
  $('sCalls').textContent = events.length;
  $('sEmerg').textContent = events.filter(e => e.kind === 'emergency').length;
  $('sPend').textContent = act.length;

  const ult = events.find(e => e.kind === 'voice' && e.loss != null);
  const q = ult ? calidad(ult.loss) : null;
  $('sLoss').textContent = ult ? ult.loss.toFixed(1) + ' %' : '—';
  $('sLoss').className = 'mt-0.5 block text-3xl ' + (q ? q.c : '');

  $('badge').textContent = act.length + (act.length === 1 ? ' pendiente' : ' pendientes');
  $('badge').className = 'whitespace-nowrap rounded-full px-3 py-1 text-sm font-bold ' + (act.length
    ? 'bg-red-100 text-red-700 dark:bg-red-500/15 dark:text-red-400'
    : 'bg-emerald-100 text-emerald-700 dark:bg-emerald-500/15 dark:text-emerald-400');
}

function renderAlerts() {
  const act = active();
  if (!act.length) {
    $('alerts').innerHTML = `<div class="py-9 text-center ${MUTED_TXT}"><div class="mb-1 text-4xl text-emerald-500">✓</div>No hay alertas pendientes<br><small>El panel está al día</small></div>`;
    return;
  }
  $('alerts').innerHTML = act.map(e => `
    <div role="button" tabindex="0" data-id="${e.id}" class="alert-row flex w-full cursor-pointer items-center gap-3 rounded-2xl border-2 p-3 text-left transition ${ROW[e.kind]} ${e.id === selectedId ? SEL[e.kind] : ''}">
      <div class="grid h-11 w-11 flex-none place-items-center rounded-xl text-xl ${ICONBOX[e.kind]}">${ICON[e.kind]}</div>
      <div class="min-w-0 flex-1">
        <div class="flex justify-between gap-2 font-bold"><span>${KIND[e.kind]}</span><span class="text-sm font-medium ${MUTED_TXT}">${hhmmss(e.time)} · ${ago(e.time)}</span></div>
        <div>${esc(label(e.node))}</div>
        <div class="text-sm ${MUTED_TXT}">${esc(e.msg || '')}</div>
      </div>
      ${e.kind === 'voice' ? playBtn(e, false) : ''}
    </div>`).join('');
}

function kv(k, v) { return `<dt class="${MUTED_TXT}">${k}</dt><dd class="font-semibold">${v}</dd>`; }

function renderDetail() {
  const e = events.find(x => x.id === selectedId);
  const box = $('detail');
  if (!e || e.resolved) {
    box.innerHTML = `<div class="py-12 text-center ${MUTED_TXT}"><div class="text-4xl">🎧</div><h2 class="mb-1 mt-2 text-xl font-bold text-slate-900 dark:text-slate-100">Seleccioná una alerta</h2>Elegí una de la lista para ver el detalle.</div>`;
    return;
  }

  let rows = kv('Recibida', `${hhmmss(e.time)} (${ago(e.time)})`);
  if (e.node != null) rows += kv('Nodo', e.node);
  let extra = '', howto = '';

  if (e.kind === 'voice') {
    rows += kv('Estado', e.live ? '<span class="text-red-600 dark:text-red-400">Recibiendo voz…</span>' : 'Voz completa');
    rows += kv('Duración', `<span id="liveDur">${fmtDur(e)}</span>`);
    if (!e.live && e.recibidos != null) {
      const q = calidad(e.loss), tot = e.recibidos + e.perdidos, ok = tot ? 100 * e.recibidos / tot : 100;
      rows += kv('Paquetes', `${e.recibidos} recibidos · ${e.perdidos} perdidos`);
      rows += kv('Pérdida', `${e.loss.toFixed(1)} % <span class="${q.c}">(${q.t})</span>`);
      if (e.ratio != null) rows += kv('Continuidad', `${e.ratio.toFixed(2)}x <span class="${MUTED_TXT}">(1.00 = sin cortes)</span>`);
      extra = `<div class="mb-4 h-2.5 overflow-hidden rounded-full bg-red-500" title="Verde: recibidos · Rojo: perdidos"><div class="h-full bg-emerald-500" style="width:${ok.toFixed(1)}%"></div></div>`;
      if (e.silencio) extra += `<div class="mb-4 rounded-xl border border-red-300 bg-red-50 px-4 py-3 text-sm text-red-800 dark:border-red-900 dark:bg-red-950/40 dark:text-red-300">Llegó silencio: el micrófono del paciente no captó audio.</div>`;
    }
    extra += audioBox(e);
    howto = '<b>Para responder:</b> mantené apretado el pulsador del equipo de la central y hablá. El paciente te escucha en su equipo.';
  } else if (e.kind === 'emergency') {
    rows += kv('Señal', esc(senal(e.rssi)));
    howto = '<b>Emergencia: ir a la habitación de inmediato.</b> Para hablar con el residente, mantené apretado el pulsador de la central.';
  } else {
    rows += kv('Señal', esc(senal(e.rssi)));
    howto = '<b>Este aviso no trae voz.</b> Dirigite a la habitación para ver qué necesita.';
  }

  box.innerHTML = `
    <span class="inline-block rounded-full px-2.5 py-1 text-xs font-extrabold tracking-wide ${TAG[e.kind]}">${KIND[e.kind].toUpperCase()}</span>
    <h2 class="mb-1 mt-2.5 text-3xl font-bold">${esc(label(e.node))}</h2>
    <dl class="my-4 grid grid-cols-[auto_1fr] gap-x-4 gap-y-1.5 text-[15px]">${rows}</dl>
    ${extra}
    <div class="mb-4 rounded-xl border border-blue-200 bg-blue-50 px-4 py-3 text-[15px] text-blue-900 dark:border-blue-900 dark:bg-blue-950/40 dark:text-blue-200">${howto}</div>
    <button id="btnResolver" class="w-full rounded-xl bg-emerald-600 py-4 text-lg font-bold text-white hover:bg-emerald-700">✓ Marcar como atendida</button>`;

  $('btnResolver').onclick = () => {
    e.resolved = true;
    const sig = active()[0];
    selectedId = sig ? sig.id : null;
    render();
  };
}

function renderHist() {
  $('histEmpty').classList.toggle('hidden', events.length > 0);
  $('hist').innerHTML = events.slice(0, 100).map(e => `
    <tr class="border-b border-slate-100 dark:border-slate-800">
      <td class="px-2.5 py-2.5">${hhmmss(e.time)}</td>
      <td class="px-2.5 py-2.5 font-bold">${esc(label(e.node))}</td>
      <td class="px-2.5 py-2.5">${KIND[e.kind]}</td>
      <td class="px-2.5 py-2.5">${esc(e.msg || '')}</td>
      <td class="px-2.5 py-2.5"><span class="rounded-full px-2.5 py-0.5 text-[13px] font-bold ${e.resolved
        ? 'bg-emerald-100 text-emerald-700 dark:bg-emerald-500/15 dark:text-emerald-400'
        : 'bg-red-100 text-red-700 dark:bg-red-500/15 dark:text-red-400'}">${e.resolved ? 'Atendida' : 'Pendiente'}</span></td>
    </tr>`).join('');
}

function renderVoice() {
  $('voiceBanner').classList.toggle('hidden', !voice);
  if (!voice) return;
  const s = Math.floor((Date.now() - voice.t0) / 1000);
  $('voiceTxt').textContent = `LLAMADA DE VOZ · ${label(voice.ev.node)} · recibiendo voz · ${s} s · ${voice.frames} tramas de audio`;
}

function render() { renderStats(); renderAlerts(); renderDetail(); renderHist(); renderVoice(); }

// El botón de escuchar está dentro de la fila: se atiende primero, así no selecciona la alerta de rebote.
$('alerts').addEventListener('click', ev => {
  const pb = ev.target.closest('[data-play]');
  if (pb) { togglePlay(+pb.dataset.play); return; }
  const row = ev.target.closest('.alert-row');
  if (row) { selectedId = +row.dataset.id; render(); }
});
$('alerts').addEventListener('keydown', ev => {
  if ((ev.key === 'Enter' || ev.key === ' ') && ev.target.classList.contains('alert-row')) {
    ev.preventDefault(); selectedId = +ev.target.dataset.id; render();
  }
});
$('detail').addEventListener('click', ev => {
  const pb = ev.target.closest('[data-play]');
  if (pb) togglePlay(+pb.dataset.play);
});

// Refrescos livianos: no se vuelve a dibujar el detalle para no pisar el botón mientras se lo toca.
setInterval(() => {
  if (!voice) return;
  // Si no llega nada hace 3 s y nunca vino el "[AUDIO] Fin" (se perdio, la base
  // se reinicio, etc.), cerrar la llamada con lo que haya llegado.
  if (Date.now() - voice.tUlt > 3000) {
    addLog('(pagina) no llego el fin de la llamada de voz: se cierra sola');
    endEmergency(prevRec + voice.frames, prevPerd);
    return;
  }
  renderVoice();
  const el = $('liveDur');
  if (el && selectedId === voice.ev.id) el.textContent = fmtDur(voice.ev);
}, 500);
setInterval(renderAlerts, 30000);

// ============ LOG ============
const logLines = [];
let logPend = false;
function addLog(line) {
  logLines.push(line);
  if (logLines.length > 200) logLines.shift();
  if (logPend) return;                 // ya hay un redibujo agendado
  logPend = true;
  setTimeout(() => {
    logPend = false;
    const pre = $('log');
    pre.textContent = logLines.join('\n');
    pre.scrollTop = pre.scrollHeight;
  }, 100);
}

// ============ SERIAL ============
let port = null, reader = null, keepReading = false;

function setConnected(on) {
  $('dot').className = 'h-2.5 w-2.5 rounded-full ' + (on ? 'bg-emerald-500 shadow-[0_0_0_4px_rgba(16,185,129,.25)]' : 'bg-slate-400');
  $('estadoTxt').textContent = on ? 'ESP32 conectado' : 'ESP32 desconectado';
  $('btnConectar').textContent = on ? 'Desconectar' : 'Conectar ESP32';
  $('btnConectar').className = 'rounded-xl px-5 py-2.5 font-bold text-white ' + (on ? 'bg-red-600 hover:bg-red-700' : 'bg-blue-600 hover:bg-blue-700');
  $('baud').disabled = on;
}

async function conectar() {
  if (!('serial' in navigator)) { toast('Este navegador no soporta Web Serial. Usá Chrome o Edge.'); return; }
  try {
    port = await navigator.serial.requestPort();
    // bufferSize: Chrome trae 255 bytes por defecto; a 921600 con audio se desborda
    // apenas la pagina tarda un poco en dibujar (BufferOverrunError).
    await port.open({ baudRate: +$('baud').value, bufferSize: 1 << 20 });
  } catch (e) {
    if (e.name !== 'NotFoundError') toast('No se pudo abrir el puerto: ' + e.message + ' (¿lo tiene abierto el Monitor Serie?)');
    port = null; return;
  }
  setConnected(true);
  initAudio(); // el navegador exige un clic del usuario para habilitar el sonido
  leer();
}

async function leer() {
  acc = new Uint8Array(0); lineBytes = [];
  keepReading = true;
  while (port && port.readable && keepReading) {
    reader = port.readable.getReader();
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        feed(value);
      }
    } catch (e) { /* cable desenchufado o error de lectura */ }
    finally { reader.releaseLock(); reader = null; }
  }
  try { await port.close(); } catch (e) {}
  port = null;
  setConnected(false);
}

async function desconectar() {
  keepReading = false;
  try { if (reader) await reader.cancel(); } catch (e) {}
}

$('btnConectar').onclick = () => port ? desconectar() : conectar();
if ('serial' in navigator) {
  navigator.serial.addEventListener('disconnect', () => { toast('Se desconectó el ESP32'); keepReading = false; });
} else {
  $('warn').classList.remove('hidden');
}

// ============ SIMULACIÓN (sin hardware) ============
let simRec = 0, simPer = 0; // contadores acumulados, como los del firmware

// Manda 230 tramas (~3 s) de un tono que sube y baja, para probar el botón de escuchar sin la placa.
function simularAudio(nodo) {
  let seq = 0, fase = 0;
  const t = setInterval(() => {
    const s = new Int16Array(104);
    const f = (seq % 40 < 20) ? 440 : 620;
    for (let i = 0; i < 104; i++) { s[i] = Math.round(9000 * Math.sin(2 * Math.PI * fase * f / RATE)); fase++; }
    handleAudioFrame(nodo, seq++, s);
    if (seq >= 230) clearInterval(t);
  }, 13);
}
$('btnSimular').onclick = () => {
  initAudio();
  handleLine('<< PULSO de nodo 5 | seq=1 | RSSI=-62 dBm | LQI=98');
  setTimeout(() => handleLine('<< ALERTA del nodo 3 | RSSI=-71 dBm | LQI=40'), 700);
  setTimeout(() => { handleLine('[AUDIO] Sesion nueva (streaming)'); simularAudio(12); }, 1500);
  setTimeout(() => {
    simRec += 230; simPer += 7;
    handleLine(`[AUDIO] Fin | recibidos=${simRec} perdidos=${simPer} underruns=3`);
    handleLine('[AUDIO] Reproducidas 24000 muestras = 3.00 s de audio, en 3.04 s reales (1.013x; 1.00 = sin cortes)');
  }, 6000);
};

// ============ ARRANQUE ============
$('fecha').textContent = new Date().toLocaleDateString('es-AR', { weekday: 'long', day: 'numeric', month: 'long', year: 'numeric' });
render();
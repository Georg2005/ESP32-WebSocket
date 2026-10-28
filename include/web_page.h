#pragma once
#include <Arduino.h>
const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="uk"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 — моніторинг мікроклімату</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#eef2f7;color:#19283d;font:16px system-ui,sans-serif}main{max-width:900px;margin:32px auto;padding:20px}h1{font-size:30px;margin-bottom:8px}h2{font-size:21px}p{line-height:1.6}.muted{color:#526174}.grid{display:grid;grid-template-columns:1fr 1fr;gap:18px}.card{background:white;padding:24px;border-radius:16px;margin:18px 0;box-shadow:0 4px 18px #19283d09}.value{font-size:42px;font-weight:700}.status{border-left:6px solid #748094}.status.ok{border-color:#15803d}.status.bad{border-color:#dc2626}label{display:block;margin:12px 0}input{display:block;width:100%;padding:12px;border:1px solid #9caabd;border-radius:8px;font:inherit;margin-top:6px}button{background:#1755c2;color:#fff;border:0;border-radius:9px;padding:13px 20px;font:inherit;cursor:pointer}button:disabled{opacity:.45;cursor:default}#message{min-height:26px}#reason{white-space:pre-line}@media(max-width:600px){.grid{grid-template-columns:1fr}main{margin:0 auto}h1{font-size:25px}}
</style></head><body><main>
<p class="muted">КУРСОВА РОБОТА · ESP32 + WebSocket</p>
<h1>Моніторинг мікроклімату</h1>
<p>Григор’єв Георгій Артемович · М-КІ-52</p>
<section id="connectionBox" class="card status"><strong id="connection" role="status">Підключення…</strong></section>
<div class="grid"><section class="card"><div>Температура</div><div class="value"><span id="temperature">—</span> °C</div></section>
<section class="card"><div>Відносна вологість</div><div class="value"><span id="humidity">—</span> %</div></section></div>
<section id="alarmBox" class="card status"><h2 id="alarmTitle">Очікування даних</h2><p id="reason">—</p><p>Червоний світлодіод: <strong id="led">—</strong></p></section>
<section class="card"><h2>Межі спрацювання</h2><p id="applied" class="muted">Зчитування налаштувань…</p>
<form id="limitsForm"><div class="grid"><div>
<label>Мінімальна температура, °C<input id="tMin" type="number" min="-40" max="80" step="0.1" required></label>
<label>Максимальна температура, °C<input id="tMax" type="number" min="-40" max="80" step="0.1" required></label></div><div>
<label>Мінімальна вологість, %<input id="hMin" type="number" min="0" max="100" step="0.1" required></label>
<label>Максимальна вологість, %<input id="hMax" type="number" min="0" max="100" step="0.1" required></label></div></div>
<button id="apply" disabled>Застосувати на ESP32</button><p id="message" role="status"></p></form>
<p class="muted">Межі включені до нормального діапазону. Достатньо виходу хоча б одного показника за межі, щоб світлодіод засвітився. Налаштування діють до перезапуску ESP32.</p></section>
<p class="muted">Автоматичне зчитування: раз на 2 секунди симуляції. За відсутності повідомлень понад 10 секунд показники очищуються, браузер повторює підключення.</p>
<script>
const $ = id => document.getElementById(id);
const fields = ['tMin','tMax','hMin','hMax'];
const TIMEOUT = 10000;
let socket = null, retry = null, openedAt = 0, lastStateAt = 0;
let online = false, dirty = false, pending = false, pendingAt = 0;
function offline(text) {
  online = false; pending = false;
  $('connection').textContent = text;
  $('connectionBox').className = 'card status bad';
  $('temperature').textContent = $('humidity').textContent = '—';
  $('alarmTitle').textContent = 'Стан системи невідомий';
  $('alarmBox').className = 'card status';
  $('reason').textContent = 'Немає актуальних даних від ESP32.';
  $('led').textContent = 'невідомо';
  $('apply').disabled = true;
}
function scheduleReconnect(ws, message) {
  if (socket !== ws) return;
  socket = null;
  ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null;
  try { ws.close(); } catch (_) {}
  offline(message);
  $('message').textContent = '';
  clearTimeout(retry);
  retry = setTimeout(connect, 2000);
}
function connect() {
  clearTimeout(retry);
  openedAt = performance.now(); lastStateAt = 0;
  const ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
  socket = ws;
  ws.onopen = () => {
    if (socket !== ws) return;
    $('connection').textContent = 'З’єднання встановлено. Очікування показників…';
  };
  ws.onmessage = event => {
    if (socket !== ws) return;
    let s; try { s = JSON.parse(event.data); } catch (_) { return; }
    if (s.type === 'ack' || s.type === 'error') {
      pending = false; $('apply').disabled = !online;
      $('message').textContent = s.message;
      if (s.type === 'ack') dirty = false;
      return;
    }
    if (s.type !== 'state' || !s.limits || !fields.every(k => Number.isFinite(s.limits[k]))) return;
    lastStateAt = performance.now(); online = true;
    $('connection').textContent = 'Підключено до ESP32';
    $('connectionBox').className = 'card status ok';
    $('apply').disabled = pending;
    if (!dirty && !pending) fields.forEach(k => $(k).value = s.limits[k]);
    $('applied').textContent = `Застосовано: ${s.limits.tMin}…${s.limits.tMax} °C; ${s.limits.hMin}…${s.limits.hMax} %.`;
    const valid = s.sensorOk && Number.isFinite(s.temperature) && Number.isFinite(s.humidity);
    $('temperature').textContent = valid ? s.temperature.toFixed(1) : '—';
    $('humidity').textContent = valid ? s.humidity.toFixed(1) : '—';
    $('led').textContent = {on:'увімкнений',off:'вимкнений',blink:'блимає — помилка датчика'}[s.ledMode] || 'невідомо';
    const reasons = [];
    if (s.alarmMask & 1) reasons.push('Температура нижча за мінімальну межу.');
    if (s.alarmMask & 2) reasons.push('Температура вища за максимальну межу.');
    if (s.alarmMask & 4) reasons.push('Вологість нижча за мінімальну межу.');
    if (s.alarmMask & 8) reasons.push('Вологість вища за максимальну межу.');
    $('alarmTitle').textContent = !s.sampled ? 'Очікування вимірювання' : !valid ? 'Помилка датчика' : reasons.length ? 'Вихід за задані межі' : 'Показники в нормі';
    $('reason').textContent = !s.sampled ? 'Перше вимірювання ще не виконане.' : !valid ? 'Немає коректного вимірювання DHT22. Перевірте підключення.' : reasons.join('\n') || 'Температура та вологість у встановлених межах.';
    $('alarmBox').className = 'card status ' + (valid && !reasons.length ? 'ok' : 'bad');
  };
  ws.onclose = () => scheduleReconnect(ws, 'З’єднання втрачено. Повторне підключення…');
  ws.onerror = () => scheduleReconnect(ws, 'Сервер недоступний. Повторне підключення…');
}
function checkConnection() {
  if (socket && performance.now() - (lastStateAt || openedAt) > TIMEOUT)
    scheduleReconnect(socket, 'Немає зв’язку: дані не надходять понад 10 секунд.');
  if (pending && performance.now() - pendingAt > TIMEOUT) {
    pending = false; $('apply').disabled = !online;
    $('message').textContent = 'Немає підтвердження команди. Перевірте застосовані межі та повторіть спробу.';
  }
}
fields.forEach(k => $(k).addEventListener('input', () => dirty = true));
$('limitsForm').addEventListener('submit', event => {
  event.preventDefault();
  if (!online || pending || !socket || socket.readyState !== WebSocket.OPEN) return;
  const v = Object.fromEntries(fields.map(k => [k, Number($(k).value)]));
  if (!fields.every(k => Number.isFinite(v[k])) || v.tMin >= v.tMax || v.hMin >= v.hMax) {
    $('message').textContent = 'Мінімальна межа має бути меншою за максимальну.'; return;
  }
  try { socket.send(JSON.stringify({type:'setLimits', ...v})); }
  catch (_) { scheduleReconnect(socket, 'Помилка надсилання. Повторне підключення…'); return; }
  pending = true; pendingAt = performance.now(); $('apply').disabled = true;
  $('message').textContent = 'Очікування підтвердження ESP32…';
});
setInterval(checkConnection, 500);
document.addEventListener('visibilitychange', checkConnection);
connect();
</script></main></body></html>)HTML";

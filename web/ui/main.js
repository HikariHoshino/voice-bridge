'use strict';
const state = { sendMode: 'ctrl_enter', showDesktop: true, expanded: false, expandedHeight: 0, connected: false, ready: false };
const EXPAND_EXTRA = 180;   // default growth of the window when the transfer panel opens

function applyLayout(animate) {
  const transfer = $('transfer');
  const open = state.expanded && state.showDesktop;
  transfer.hidden = !state.showDesktop;
  transfer.classList.remove('open');
  const collapsed = naturalHeight();
  transfer.classList.toggle('open', open);
  const min = open ? naturalHeight() : collapsed;
  const height = open ? Math.max(min, state.expandedHeight || collapsed + EXPAND_EXTRA) : collapsed;
  $('toggle').setAttribute('aria-expanded', String(open));
  $('toggle').setAttribute('aria-label', open ? '收起电脑到手机' : '展开电脑到手机');
  send('layout', { height, min, animate });
}

// The window edge (animated per vsync by Main.cpp) drives the layout: while animating, the panel simply
// fills whatever height the window has, so the content grows and shrinks exactly with the edge.
const RESIZE_MS = 240;
let settleTimer = 0;

function toggleTransfer() {
  const transfer = $('transfer');
  clearTimeout(settleTimer);
  const windowFrom = window.innerHeight;
  if (state.expanded) state.expandedHeight = windowFrom;
  state.expanded = !state.expanded;

  // Measure the landing sizes; nothing paints in between.
  transfer.classList.remove('open', 'animating', 'closing');
  const collapsed = naturalHeight();
  transfer.classList.add('open');
  const openMin = naturalHeight();
  const windowTo = state.expanded ? Math.max(openMin, state.expandedHeight || collapsed + EXPAND_EXTRA) : collapsed;

  transfer.classList.add('animating');
  transfer.classList.toggle('closing', !state.expanded);
  $('toggle').setAttribute('aria-expanded', String(state.expanded));
  $('toggle').setAttribute('aria-label', state.expanded ? '收起电脑到手机' : '展开电脑到手机');
  send('layout', { height: windowTo, min: state.expanded ? openMin : collapsed, animate: true, duration: RESIZE_MS });

  settleTimer = setTimeout(() => {
    transfer.classList.remove('animating', 'closing');
    transfer.classList.toggle('open', state.expanded);
    if (state.expanded) $('text').focus();
  }, RESIZE_MS + 40);
}

function setStatus(text) {
  $('status').textContent = text;
  $('status').title = text;
}

function setConnected(connected) {
  state.connected = connected;
  $('chip').classList.toggle('ok', connected);
  $('chip-text').textContent = connected ? '已连接' : (state.ready ? '等待连接' : '启动中');
}

function setCount(count, show) {
  if (typeof show === 'boolean') $('count').hidden = !show;
  if (typeof count === 'number') $('count').textContent = `累计 ${count.toLocaleString('en-US')} 字符`;
}

function applySendMode(mode) {
  state.sendMode = mode;
  $('shortcut').textContent = mode === 'enter' ? 'Enter 发送 · Ctrl + Enter 换行' : 'Ctrl + Enter 发送 · Enter 换行';
}

const sendText = () => send('send', { text: $('text').value });

Object.assign(handlers, {
  settings(m) {
    setTheme(m.theme === 'dark');
    applySendMode(m.sendMode);
    state.showDesktop = m.showDesktop !== false;
    setCount(m.count, m.showCount);
    applyLayout(false);
    enableTransitions();
  },
  bridge(m) {
    state.ready = !!m.ready;
    $('qr').disabled = $('send').disabled = !m.ready;
    if (m.ready) {
      const address = $('address');
      address.classList.toggle('warn', !m.available);
      address.textContent = m.available ? `${m.ip}:${m.port}` : '地址不可用，请刷新二维码';
    }
    setConnected(state.connected);
  },
  phone(m) { setConnected(!!m.connected); },
  status(m) { setStatus(m.text); setCount(m.count); },
  sent() { $('text').value = ''; },
  shot(m) {
    // Deterministic states for verification screenshots.
    document.body.classList.add('no-anim');
    const states = (m.state || '').split(',');
    if (states.includes('dark')) setTheme(true);
    if (states.includes('connected')) { setConnected(true); setStatus('已输入 36 个字符'); }
    if (states.includes('expanded')) {
      $('text').value = '晚上七点在三教 204 开组会，记得带上周的实验数据。';
      state.expanded = true; applyLayout(false);
    }
  }
});

$('theme').addEventListener('click', () => { const dark = !root.classList.contains('dark'); setTheme(dark); send('theme', { dark }); });
$('settings').addEventListener('click', () => send('open', { page: 'settings' }));
$('qr').addEventListener('click', () => send('open', { page: 'qr' }));
$('toggle').addEventListener('click', toggleTransfer);
$('send').addEventListener('click', sendText);
$('text').addEventListener('keydown', (event) => {
  if (event.key !== 'Enter' || event.isComposing) return;
  const ctrl = event.ctrlKey;
  if ((state.sendMode === 'ctrl_enter' && ctrl) || (state.sendMode === 'enter' && !ctrl)) { event.preventDefault(); sendText(); }
  else if (state.sendMode === 'enter' && ctrl) {
    event.preventDefault();
    const box = $('text');
    box.setRangeText('\n', box.selectionStart, box.selectionEnd, 'end');
  }
});

send('ready');

'use strict';
// Shared by every window: host messaging, theme, and reporting the page's natural height to the shell.
const host = window.chrome && window.chrome.webview;
const $ = (id) => document.getElementById(id);
const send = (cmd, extra = {}) => host && host.postMessage({ cmd, ...extra });
const root = document.documentElement;
const handlers = { theme: (m) => setTheme(m.dark) };

function setTheme(dark) { root.classList.toggle('dark', dark); }

// Pages start with transitions off so the first theme applies instantly, then enable them.
function enableTransitions() {
  requestAnimationFrame(() => requestAnimationFrame(() => document.body.classList.remove('no-anim')));
}

function naturalHeight() {
  const app = document.querySelector('.app');
  app.style.height = 'auto';
  const height = Math.ceil(app.getBoundingClientRect().height);
  app.style.height = '';
  return height;
}

function reportHeight() {
  const height = naturalHeight();
  send('layout', { height, min: height, animate: false });
}

if (host) host.addEventListener('message', (event) => { const m = event.data; if (m && handlers[m.type]) handlers[m.type](m); });
document.querySelectorAll('[data-cmd]').forEach((button) => button.addEventListener('click', () => send(button.dataset.cmd)));
window.addEventListener('contextmenu', (event) => { if (!/^(TEXTAREA|INPUT)$/.test(event.target.tagName)) event.preventDefault(); });

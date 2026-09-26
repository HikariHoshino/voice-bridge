'use strict';
// Choices stay a draft until 保存设置; 取消 or closing the window discards them.
const draft = {};
let loaded = false;

function render() {
  document.querySelectorAll('.switch').forEach((sw) => sw.setAttribute('aria-checked', String(!!draft[sw.dataset.key])));
  document.querySelectorAll('.segmented').forEach((group) => {
    const buttons = [...group.querySelectorAll('button')];
    const index = Math.max(0, buttons.findIndex((b) => b.dataset.value === draft[group.dataset.key]));
    group.style.setProperty('--index', index);
    buttons.forEach((b, i) => { b.setAttribute('aria-checked', String(i === index)); b.tabIndex = i === index ? 0 : -1; });
  });
}

Object.assign(handlers, {
  settings(m) {
    setTheme(m.theme === 'dark');
    if (!loaded) {
      Object.assign(draft, { autostart: m.autostart, show_desktop_to_phone: m.showDesktop, show_char_count: m.showCount,
        close_action: m.closeAction, send_mode: m.sendMode });
      loaded = true;
    }
    document.querySelector('[data-key="autostart"]').disabled = !m.autostartAvailable;
    $('count-now').textContent = `当前 ${Number(m.count || 0).toLocaleString('en-US')} 个`;
    render();
    reportHeight();
    enableTransitions();
  }
});

document.querySelectorAll('.switch').forEach((sw) => sw.addEventListener('click', () => {
  draft[sw.dataset.key] = !draft[sw.dataset.key];
  render();
}));
document.querySelectorAll('.segmented').forEach((group) => {
  const buttons = [...group.querySelectorAll('button')];
  buttons.forEach((button, i) => {
    button.addEventListener('click', () => { draft[group.dataset.key] = button.dataset.value; render(); });
    button.addEventListener('keydown', (event) => {
      const step = event.key === 'ArrowRight' ? 1 : event.key === 'ArrowLeft' ? -1 : 0;
      if (!step) return;
      event.preventDefault();
      const next = buttons[(i + step + buttons.length) % buttons.length];
      draft[group.dataset.key] = next.dataset.value; render(); next.focus();
    });
  });
});
$('clear').addEventListener('click', () => send('clear_count'));
$('save').addEventListener('click', () => send('save', { values: draft }));
window.addEventListener('keydown', (event) => { if (event.key === 'Escape') send('close'); });

send('ready');

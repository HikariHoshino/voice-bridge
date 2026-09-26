'use strict';
Object.assign(handlers, {
  settings(m) { setTheme(m.theme === 'dark'); reportHeight(); enableTransitions(); $('tray').focus(); }
});
$('exit').addEventListener('click', () => send('exit'));
$('tray').addEventListener('click', () => send('tray'));
window.addEventListener('keydown', (event) => { if (event.key === 'Escape') send('close'); });
send('ready');

'use strict';
let rotateTimer = 0, copyTimer = 0;

function drawCode(rows) {
  const size = rows.length;
  let path = '';
  rows.forEach((row, y) => { for (let x = 0; x < size; x++) if (row[x] === '1') path += `M${x} ${y}h1v1h-1z`; });
  $('code').setAttribute('viewBox', `0 0 ${size} ${size}`);
  $('modules').setAttribute('d', path);
}

function resetRotate() {
  clearTimeout(rotateTimer);
  $('rotate').textContent = '重新配对';
  $('rotate').classList.remove('armed');
  $('rotate-hint').hidden = true;
  reportHeight();
}

Object.assign(handlers, {
  settings(m) { setTheme(m.theme === 'dark'); reportHeight(); enableTransitions(); },
  qr(m) {
    if (!m.ready) return;
    drawCode(m.rows);
    $('address').textContent = m.address;
    $('address').classList.toggle('warn', !m.available);
    resetRotate();
  },
  copied() {
    clearTimeout(copyTimer);
    $('copy').textContent = '已复制';
    copyTimer = setTimeout(() => { $('copy').textContent = '复制访问地址'; }, 1500);
  }
});

$('copy').addEventListener('click', () => send('copy'));
$('refresh').addEventListener('click', () => send('refresh'));
$('rotate').addEventListener('click', () => {
  if ($('rotate').classList.contains('armed')) { send('rotate'); return; }
  $('rotate').classList.add('armed');
  $('rotate').textContent = '确认重新配对';
  $('rotate-hint').hidden = false;
  reportHeight();
  rotateTimer = setTimeout(resetRotate, 4000);
});
window.addEventListener('keydown', (event) => { if (event.key === 'Escape') send('close'); });

send('ready');

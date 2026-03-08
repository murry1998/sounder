module.exports = function(mainWindow) {
  const isMac = process.platform === 'darwin';

  function sendMenuAction(action) {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('menu-action', action);
    }
  }

  return [
    ...(isMac ? [{
      label: 'Sounder',
      submenu: [
        { role: 'about' },
        { type: 'separator' },
        { role: 'services' },
        { type: 'separator' },
        { role: 'hide' },
        { role: 'hideOthers' },
        { role: 'unhide' },
        { type: 'separator' },
        { role: 'quit' }
      ]
    }] : []),
    {
      label: 'File',
      submenu: [
        { label: 'New Project', accelerator: 'CmdOrCtrl+N', click: () => sendMenuAction('new') },
        { type: 'separator' },
        { label: 'Open Project...', accelerator: 'CmdOrCtrl+O', click: () => sendMenuAction('open') },
        { type: 'separator' },
        { label: 'Save Project', accelerator: 'CmdOrCtrl+S', click: () => sendMenuAction('save') },
        { label: 'Save As...', accelerator: 'CmdOrCtrl+Shift+S', click: () => sendMenuAction('saveAs') },
        { type: 'separator' },
        { label: 'Import Audio...', accelerator: 'CmdOrCtrl+I', click: () => sendMenuAction('import') },
        { type: 'separator' },
        { label: 'Export WAV...', click: () => sendMenuAction('exportWav') },
        { label: 'Export AIFF...', click: () => sendMenuAction('exportAiff') },
        { type: 'separator' },
        ...(isMac ? [] : [{ role: 'quit' }])
      ]
    },
    {
      label: 'Edit',
      submenu: [
        { label: 'Undo', accelerator: 'CmdOrCtrl+Z', click: () => sendMenuAction('undo') },
        { label: 'Redo', accelerator: 'CmdOrCtrl+Shift+Z', click: () => sendMenuAction('redo') },
        { type: 'separator' },
        { role: 'cut' },
        { role: 'copy' },
        { role: 'paste' },
        { role: 'selectAll' }
      ]
    },
    {
      label: 'View',
      submenu: [
        { label: 'Zoom In', accelerator: 'CmdOrCtrl+=', click: () => sendMenuAction('zoomIn') },
        { label: 'Zoom Out', accelerator: 'CmdOrCtrl+-', click: () => sendMenuAction('zoomOut') },
        { type: 'separator' },
        { role: 'toggleDevTools' },
        { role: 'togglefullscreen' }
      ]
    },
    {
      label: 'Window',
      submenu: [
        { role: 'minimize' },
        { role: 'zoom' },
        ...(isMac ? [
          { type: 'separator' },
          { role: 'front' }
        ] : [{ role: 'close' }])
      ]
    }
  ];
};

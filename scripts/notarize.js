const { notarize } = require('@electron/notarize');

module.exports = async function(context) {
  if (context.electronPlatformName !== 'darwin') return;

  if (!process.env.APPLE_ID || !process.env.APPLE_APP_PASSWORD || !process.env.APPLE_TEAM_ID) {
    console.log('Skipping notarization: APPLE_ID, APPLE_APP_PASSWORD, or APPLE_TEAM_ID not set');
    return;
  }

  console.log('Notarizing Sounder...');
  await notarize({
    appBundleId: 'com.hauksbee.sounder',
    appPath: context.appOutDir + '/Sounder.app',
    appleId: process.env.APPLE_ID,
    appleIdPassword: process.env.APPLE_APP_PASSWORD,
    teamId: process.env.APPLE_TEAM_ID
  });
  console.log('Notarization complete.');
};

#!/usr/bin/env node
/**
 * codesign-electron.js — Postinstall script to codesign Electron.app for development
 *
 * On macOS, the Electron.app binary needs:
 *  1. NSMicrophoneUsageDescription in Info.plist (already present in Electron 33+)
 *  2. com.apple.security.device.audio-input entitlement
 *  3. Proper inside-out ad-hoc codesigning
 *
 * This script also updates the bundle identifier from com.github.Electron to
 * com.hauksbee.sounder so TCC tracks permissions for our app specifically.
 */

const { execSync } = require('child_process');
const path = require('path');
const fs = require('fs');

if (process.platform !== 'darwin') {
  console.log('[codesign] Skipping — not macOS');
  process.exit(0);
}

const ROOT = path.resolve(__dirname, '..');
const ELECTRON_APP = path.join(ROOT, 'node_modules/electron/dist/Electron.app');
const ENTITLEMENTS = path.join(ROOT, 'entitlements.plist');
const CHILD_ENTITLEMENTS = path.join(ROOT, 'entitlements-child.plist');

if (!fs.existsSync(ELECTRON_APP)) {
  console.log('[codesign] Electron.app not found — skipping');
  process.exit(0);
}

if (!fs.existsSync(ENTITLEMENTS)) {
  console.log('[codesign] entitlements.plist not found — skipping');
  process.exit(0);
}

function run(cmd) {
  try {
    execSync(cmd, { stdio: 'pipe' });
    return true;
  } catch (e) {
    console.error(`[codesign] Command failed: ${cmd}`);
    console.error(e.stderr?.toString() || e.message);
    return false;
  }
}

console.log('[codesign] Updating Electron.app bundle identifier and Info.plist...');

// Update bundle identifier
run(`plutil -replace CFBundleIdentifier -string "com.hauksbee.sounder" "${ELECTRON_APP}/Contents/Info.plist"`);
run(`plutil -replace CFBundleName -string "Sounder" "${ELECTRON_APP}/Contents/Info.plist"`);
run(`plutil -replace CFBundleDisplayName -string "Sounder" "${ELECTRON_APP}/Contents/Info.plist"`);

// Ensure NSMicrophoneUsageDescription is set to our custom string
run(`plutil -replace NSMicrophoneUsageDescription -string "Sounder needs microphone access to record audio" "${ELECTRON_APP}/Contents/Info.plist"`);

// Add NSMicrophoneUsageDescription to helper apps
const helpers = [
  'Electron Helper.app',
  'Electron Helper (GPU).app',
  'Electron Helper (Plugin).app',
  'Electron Helper (Renderer).app',
];

for (const helper of helpers) {
  const plist = path.join(ELECTRON_APP, 'Contents/Frameworks', helper, 'Contents/Info.plist');
  if (fs.existsSync(plist)) {
    // Try insert first, fall back to replace if key already exists
    if (!run(`plutil -insert NSMicrophoneUsageDescription -string "Sounder needs microphone access to record audio" "${plist}"`)) {
      run(`plutil -replace NSMicrophoneUsageDescription -string "Sounder needs microphone access to record audio" "${plist}"`);
    }
  }
}

console.log('[codesign] Codesigning Electron.app (inside-out)...');

// Create child entitlements file if it doesn't exist
if (!fs.existsSync(CHILD_ENTITLEMENTS)) {
  fs.writeFileSync(CHILD_ENTITLEMENTS, `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
    <true/>
    <key>com.apple.security.cs.disable-library-validation</key>
    <true/>
    <key>com.apple.security.device.audio-input</key>
    <true/>
    <key>com.apple.security.cs.allow-jit</key>
    <true/>
    <key>com.apple.security.cs.allow-dyld-environment-variables</key>
    <true/>
</dict>
</plist>
`);
}

// Sign frameworks first
const frameworks = ['Electron Framework.framework', 'Mantle.framework', 'ReactiveObjC.framework', 'Squirrel.framework'];
for (const fw of frameworks) {
  const fwPath = path.join(ELECTRON_APP, 'Contents/Frameworks', fw);
  if (fs.existsSync(fwPath)) {
    if (fw === 'Electron Framework.framework') {
      run(`codesign --force --sign - --entitlements "${CHILD_ENTITLEMENTS}" "${fwPath}"`);
    } else {
      run(`codesign --force --sign - "${fwPath}"`);
    }
  }
}

// Sign helper apps
for (const helper of helpers) {
  const helperPath = path.join(ELECTRON_APP, 'Contents/Frameworks', helper);
  if (fs.existsSync(helperPath)) {
    run(`codesign --force --sign - --entitlements "${CHILD_ENTITLEMENTS}" "${helperPath}"`);
  }
}

// Sign main app last
run(`codesign --force --sign - --entitlements "${ENTITLEMENTS}" "${ELECTRON_APP}"`);

// Verify
const verified = run(`codesign --verify --deep --strict "${ELECTRON_APP}"`);
if (verified) {
  console.log('[codesign] Electron.app signed and verified successfully.');
} else {
  console.error('[codesign] WARNING: Verification failed. Microphone permissions may not work.');
}

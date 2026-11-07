// Headless check of the qdev-host Hardware panel overlay on a real PVE node.
//
//   PVE_HOST=172.16.16.17 PVE_TICKET="$(ssh root@HOST perl -e 'use PVE::AccessControl; print PVE::AccessControl::assemble_ticket(q{root@pam})')" \
//   VMID=990 node tools/pve-ui-test.mjs [--dry] [--keep] [--shots DIR]
//
// Needs the playwright package (next to this file, or PLAYWRIGHT=/path/node_modules/playwright/index.mjs)
// and a Chromium (CHROME=/path, default: Playwright's channel "chrome").  The ticket is minted on the
// node, so no password is needed.  Steps: open the VM's Hardware panel, add a tb-vga
// device through Add > qdev-host Device, wait for the "attached" row, remove it, and
// check the row is gone.  Screenshots land in --shots (default ./ui-shots).
import fs from 'node:fs';
// PLAYWRIGHT=/some/node_modules/playwright/index.mjs when the package is not installed next to this file
const { chromium } = await import(process.env.PLAYWRIGHT || 'playwright');

const host = process.env.PVE_HOST, ticket = process.env.PVE_TICKET, vmid = process.env.VMID;
if (!host || !ticket || !vmid) { console.error('PVE_HOST, PVE_TICKET and VMID are required'); process.exit(2); }
const args = process.argv.slice(2);
const keep = args.includes('--keep');
const dry = args.includes('--dry'); // open the panel, menu and dialog only; change nothing
const shots = args.includes('--shots') ? args[args.indexOf('--shots') + 1] : 'ui-shots';
fs.mkdirSync(shots, { recursive: true });
const devId = process.env.DEV_ID || 'uitest0';

const browser = await chromium.launch(process.env.CHROME ? { executablePath: process.env.CHROME } : { channel: 'chrome' });
const context = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1500, height: 900 } });
const page = await context.newPage();
const consoleErrors = [];
page.on('pageerror', (e) => consoleErrors.push(`pageerror: ${e.message}`));
page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(`console: ${m.text()}`); });
const shot = async (name) => page.screenshot({ path: `${shots}/${name}.png` });
const fail = async (why) => { await shot('failure'); console.error(`FAIL: ${why}`); console.error(consoleErrors.join('\n')); if (!keep) await browser.close(); process.exit(1); };

try {
    // Log in the way the login window does, with the ticket as the password: this
    // stores the renewed ticket, the CSRF token and the capability map (GuiCap)
    // that the Hardware panel needs; a bare cookie would leave GuiCap empty.
    await page.goto(`https://${host}:8006/`, { waitUntil: 'load' });
    const user = await page.evaluate(async (t) => {
        const r = await fetch('/api2/json/access/ticket', { method: 'POST', body: new URLSearchParams({ username: 'root@pam', password: t }) });
        if (!r.ok) { throw new Error(`login with ticket failed: HTTP ${r.status}`); }
        const d = (await r.json()).data;
        Proxmox.Utils.setAuthData(d);
        Ext.state.Manager.set('GuiCap', d.cap);
        return d.username;
    }, ticket);
    console.log(`logged in as ${user}`);
    consoleErrors.length = 0;
    await page.reload({ waitUntil: 'load' });
    // The script tag of the overlay must be in the served page.
    const overlayTag = await page.locator('script[src*="qdev-host-hardware.js"]').count();
    if (!overlayTag) await fail('overlay script tag missing from index.html');
    await page.waitForSelector('.x-tree-view', { timeout: 30000 });
    // A fresh profile shows the node collapsed: expand it, then pick the VM.
    const vmNode = page.locator('.x-tree-node-text', { hasText: new RegExp(`^${vmid} \\(`) }).first();
    if (!(await vmNode.count())) {
        const nodeRow = page.locator('.x-tree-view .x-grid-item', { hasText: /^\s*\S+\s*$/ }).nth(1);
        const expander = page.locator('.x-tree-view .x-tree-expander').nth(1);
        if (await expander.count()) { await expander.click(); } else { await nodeRow.dblclick(); }
    }
    await vmNode.waitFor({ timeout: 30000 });
    await vmNode.click();
    await page.locator('.x-treelist-item-text', { hasText: /^Hardware$/ }).first().click();
    const grid = page.locator('.x-grid-view', { has: page.locator('.x-grid-item', { hasText: 'Memory' }) }).first();
    await grid.locator('.x-grid-item', { hasText: 'Memory' }).first().waitFor({ timeout: 30000 });
    await page.waitForTimeout(1500); // let the node service answer once
    await shot('1-hardware');
    const errRow = grid.locator('.x-grid-item', { hasText: 'qdev-host' }).filter({ hasText: /unreachable|not allowed|authentication/ });
    if (await errRow.count()) await fail(`service error row shown: ${await errRow.first().innerText()}`);

    // Add > qdev-host Device
    await page.locator('.x-btn', { hasText: /^Add$/ }).first().click();
    const item = page.locator('.x-menu-item', { hasText: 'qdev-host Device' }).first();
    await item.waitFor({ timeout: 10000 });
    await shot('2-add-menu');
    await item.click();
    const win = page.locator('.x-window', { hasText: 'qdev-host Device' }).first();
    await win.waitFor({ timeout: 10000 });
    await win.locator('input[name="id"]').fill(devId);
    await shot('3-add-dialog');
    if (dry) {
        const plugins = await win.locator('input[name="plugin"]').inputValue();
        const kmsCount = await win.locator('input[name="kms"]').count();
        console.log(`dialog: plugin=${plugins} kms-field=${kmsCount} args=${await win.locator('input[name^="arg_"]').count()}`);
        await win.locator('.x-tool-close').click();
        await win.waitFor({ state: 'detached', timeout: 10000 });
        if (consoleErrors.length) console.log('browser errors:\n' + consoleErrors.join('\n'));
        console.log('PASS (dry)');
        if (!keep) await browser.close();
        process.exit(0);
    }
    await win.locator('.x-btn', { hasText: /^Add$/ }).click();
    await win.waitFor({ state: 'detached', timeout: 90000 });
    // an info box may follow (device defined but not attached): accept it
    const msg = page.locator('.x-message-box');
    if (await msg.count() && await msg.first().isVisible()) { await shot('3b-add-message'); await msg.locator('.x-btn', { hasText: /^OK$/ }).click(); }
    const row = grid.locator('.x-grid-item', { hasText: `qdev-host (${devId})` }).first();
    await row.waitFor({ timeout: 30000 });
    let text = await row.innerText();
    for (let i = 0; i < 20 && !/attached \(/.test(text); i++) { await page.waitForTimeout(1000); text = await row.innerText(); }
    await shot('4-row-added');
    console.log(`row: ${text.replace(/\s+/g, ' ').trim()}`);
    if (process.env.EXPECT_ATTACHED !== '0' && !/attached \(/.test(text)) await fail('device row did not reach the attached state');

    // Remove
    await row.click();
    await page.locator('.x-btn', { hasText: /^Remove$/ }).first().click();
    const confirm = page.locator('.x-message-box').first();
    await confirm.waitFor({ timeout: 10000 });
    await shot('5-remove-confirm');
    await confirm.locator('.x-btn', { hasText: /^Yes$/ }).click();
    await row.waitFor({ state: 'detached', timeout: 90000 });
    await page.waitForTimeout(1000);
    await shot('6-row-removed');
    if (consoleErrors.length) console.log('browser errors:\n' + consoleErrors.join('\n'));
    console.log('PASS');
} catch (e) {
    await fail(e.stack || String(e));
}
if (!keep) await browser.close();

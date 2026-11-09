// Headless check of the TB bus in PVE's own dialogs (qdev-host-pve 0.6.0) on a real node.
//
//   PVE_HOST=172.16.16.17 PVE_TICKET="$(ssh root@HOST 'perl -e "use PVE::AccessControl; print PVE::AccessControl::assemble_ticket(q{root@pam})"')" \
//   VMID=990 BRIDGE=vmbr1 USB_DEV=1a86:7523 CHROME=/path/to/chrome node tools/pve-tb-ui-test.mjs
//
// The VM must be a running q35 VM with a TB USB controller (qdevctl add VMID tbxhci0 --plugin tb-xhci)
// and its TB bus present.  Steps: Add > Network Device on the TB bus, Edit it in the same dialog
// (rate), Add > USB Device USB_DEV on the TB controller, Remove it, open the controller's dialog and
// Add > PCI Device with the TB bus (neither submitted), Remove the network device.  The VM's other
// devices stay.  Needs the playwright package next to this file (or PLAYWRIGHT=/path/node_modules/playwright/index.mjs).
import fs from 'node:fs';
const { chromium } = await import(process.env.PLAYWRIGHT || 'playwright');
const bridge = process.env.BRIDGE || 'vmbr0';
const usbDev = process.env.USB_DEV;
if (!usbDev) { console.error('USB_DEV=VENDOR:PRODUCT of a USB device of the node is required'); process.exit(2); }
const host = process.env.PVE_HOST, ticket = process.env.PVE_TICKET, vmid = process.env.VMID;
const shots = process.env.SHOTS || 'tb-shots';
fs.mkdirSync(shots, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROME });
const context = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1500, height: 950 } });
const page = await context.newPage();
const errors = [];
page.on('pageerror', (e) => errors.push(`pageerror: ${e.message}`));
page.on('console', (m) => { if (m.type() === 'error') errors.push(`console: ${m.text()}`); });
const shot = (n) => page.screenshot({ path: `${shots}/${n}.png` });
const step = (m) => console.log(`-- ${m}`);
const fail = async (why) => { await shot('failure'); console.error(`FAIL: ${why}`); console.error(errors.join('\n')); await browser.close(); process.exit(1); };
const win = () => page.evaluate(() => { const w = Ext.WindowManager.getActive(); return w && !w.destroyed && w.isVisible() ? `${w.$className}|${w.getTitle ? w.getTitle() : ''}` : null; });
const waitWin = async (re, t = 15000) => { const end = Date.now() + t; while (Date.now() < end) { const w = await win(); if (w && re.test(w)) return w; await page.waitForTimeout(200); } await fail(`window ${re} did not open (active: ${await win()})`); };
const waitNoWin = async (t = 90000) => { const end = Date.now() + t; while (Date.now() < end) { const w = await win(); if (!w || /MessageBox/.test(w)) return w; await page.waitForTimeout(300); } await fail(`window still open: ${await win()}`); };
const rows = () => page.evaluate(() => [...document.querySelectorAll('.x-grid-item')].map((r) => r.innerText.replace(/\s+/g, ' ').trim()).filter((t) => /\(tb[a-z]+\d+\)|^qdev-host/.test(t)));
const ids = async (re) => (await rows()).filter((r) => re.test(r)).map((r) => r.match(/\((tb[a-z]+\d+)\)/)[1]);
const esc = (x) => x.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
const waitRow = async (re, t = 60000) => { const end = Date.now() + t; while (Date.now() < end) { const r = await rows(); const hit = r.find((x) => re.test(x)); if (hit) return hit; await page.waitForTimeout(500); } await fail(`row ${re} not shown; rows: ${JSON.stringify(await rows())}`); };
const waitNoRow = async (re, t = 60000) => { const end = Date.now() + t; while (Date.now() < end) { if (!(await rows()).some((x) => re.test(x))) return; await page.waitForTimeout(500); } await fail(`row ${re} still shown`); };
const msgBox = async () => page.evaluate(() => { const w = Ext.WindowManager.getActive(); if (w && /MessageBox/.test(w.$className) && w.isVisible()) { const t = w.down('#msg') || w; const text = (w.el && w.el.dom.innerText) || ''; w.close(); return text.replace(/\s+/g, ' '); } return null; });
const addMenu = async (label) => {
    await page.locator('.x-btn', { hasText: /^Add$/ }).first().click();
    const item = page.locator('.x-menu-item', { hasText: label }).first();
    await item.waitFor({ timeout: 10000 });
    await item.click();
};
const rowClick = async (re, dbl) => { const r = page.locator('.x-grid-item', { hasText: re }).first(); await r.click(); if (dbl) await r.dblclick(); };
try {
    await page.goto(`https://${host}:8006/`, { waitUntil: 'load' });
    await page.evaluate(async (t) => {
        const r = await fetch('/api2/json/access/ticket', { method: 'POST', body: new URLSearchParams({ username: 'root@pam', password: t }) });
        const d = (await r.json()).data; Proxmox.Utils.setAuthData(d); Ext.state.Manager.set('GuiCap', d.cap);
    }, ticket);
    await page.reload({ waitUntil: 'load' });
    await page.waitForSelector('.x-tree-view', { timeout: 30000 });
    const vmNode = page.locator('.x-tree-node-text', { hasText: new RegExp(`^${vmid} \\(`) }).first();
    if (!(await vmNode.count())) {
        const nodeName = await page.evaluate(() => Proxmox.NodeName);
        await page.locator('.x-tree-view .x-grid-item', { hasText: nodeName }).first().locator('.x-tree-expander').click();
    }
    await vmNode.waitFor({ timeout: 30000 }); await vmNode.click();
    await page.locator('.x-treelist-item-text', { hasText: /^Hardware$/ }).first().click();
    await waitRow(/^USB Controller \(tb/);
    await shot('1-rows');
    step(`rows: ${JSON.stringify(await rows())}`);

    const netBefore = await ids(/^Network Device \(tb/);
    step('Add > Network Device on the TB bus');
    await addMenu('Network Device');
    await waitWin(/NetworkEdit/);
    await page.waitForTimeout(1500);
    const busChoices = await page.evaluate(() => { const c = Ext.WindowManager.getActive().down('#qdevBus'); return c ? c.getStore().getRange().map((r) => r.get('label')) : null; });
    if (!busChoices) await fail('no Bus selector in the network dialog');
    step(`bus choices: ${JSON.stringify(busChoices)}`);
    await page.evaluate((b) => { const w = Ext.WindowManager.getActive(); w.down('#qdevBus').setValue('tb'); w.down('[name=bridge]').setValue(b); w.down('[name=model]').setValue('virtio'); }, bridge);
    await page.waitForTimeout(300);
    const fwDisabled = await page.evaluate(() => Ext.WindowManager.getActive().down('[name=firewall]').isDisabled());
    step(`firewall checkbox disabled on TB: ${fwDisabled}`);
    await shot('2-net-dialog-tb');
    await page.evaluate(() => Ext.WindowManager.getActive().submit());
    await waitNoWin();
    const m1 = await msgBox(); if (m1) step(`message: ${m1}`);
    let net = null;
    for (let i = 0; i < 120 && !net; i++) { net = (await ids(/^Network Device \(tb.*attached/)).find((x) => !netBefore.includes(x)); if (!net) await page.waitForTimeout(500); }
    if (!net) await fail(`no new attached TB network row: ${JSON.stringify(await rows())}`);
    const netRe = new RegExp(`^Network Device \\(${esc(net)}\\)`);
    step(`row: ${await waitRow(netRe)}`);

    step('Edit the TB network row: dialog prefilled, change rate');
    await rowClick(netRe, true);
    await waitWin(/NetworkEdit/);
    await page.waitForTimeout(1500);
    const pre = await page.evaluate(() => { const w = Ext.WindowManager.getActive(); return { bus: w.down('#qdevBus') && w.down('#qdevBus').getValue(), busDisabled: w.down('#qdevBus') && w.down('#qdevBus').isDisabled(), bridge: w.down('[name=bridge]').getValue(), model: w.down('[name=model]').getValue(), mac: w.down('[name=macaddr]').getValue(), title: w.getTitle() }; });
    step(`prefilled: ${JSON.stringify(pre)}`);
    if (pre.bridge !== bridge || pre.model !== 'virtio' || pre.bus !== 'tb') await fail('edit dialog not prefilled from the TB device');
    await page.evaluate(() => { const w = Ext.WindowManager.getActive(); w.down('[name=rate]').setValue(50); });
    await shot('3-net-edit');
    await page.evaluate(() => Ext.WindowManager.getActive().submit());
    await waitNoWin();
    const m2 = await msgBox(); if (m2) step(`message: ${m2}`);
    step(`row: ${await waitRow(new RegExp(netRe.source + '.*rate=50.*attached'))}`);

    step('Add > USB Device on the TB controller');
    await addMenu('USB Device');
    await waitWin(/USBEdit/);
    await page.waitForTimeout(2500);
    const usbChoices = await page.evaluate(() => { const c = Ext.WindowManager.getActive().down('#qdevBus'); return c ? c.getStore().getRange().map((r) => `${r.get('value')}=${r.get('label')}`) : null; });
    step(`bus choices: ${JSON.stringify(usbChoices)}`);
    const usbBefore = await ids(/^USB Device \(tb/);
    await page.evaluate((dev) => { const w = Ext.WindowManager.getActive(); const c = w.down('#qdevBus'); c.setValue(c.getStore().getRange().map((r) => r.get('value')).find((v) => v.startsWith('tb:'))); w.down('radiofield[inputValue=hostdevice]').setValue(true); w.down('[name=hostdevice]').setValue(dev); }, usbDev);
    await page.waitForTimeout(500);
    await shot('4-usb-dialog-tb');
    await page.evaluate(() => Ext.WindowManager.getActive().submit());
    await waitNoWin();
    const m3 = await msgBox(); if (m3) step(`message: ${m3}`);
    let usb = null;
    for (let i = 0; i < 120 && !usb; i++) { usb = (await ids(/^USB Device \(tb.*attached/)).find((x) => !usbBefore.includes(x)); if (!usb) await page.waitForTimeout(500); }
    if (!usb) await fail(`no new attached TB USB row: ${JSON.stringify(await rows())}`);
    const usbRe = new RegExp(`^USB Device \\(${esc(usb)}\\)`);
    step(`row: ${await waitRow(usbRe)}`);
    await shot('5-rows-after-add');

    step('Remove the TB USB device');
    await rowClick(usbRe);
    await page.locator('.x-btn', { hasText: /^Remove$/ }).first().click();
    await waitWin(/MessageBox/);
    await shot('6-remove-confirm');
    await page.locator('.x-message-box .x-btn', { hasText: /^Yes$/ }).first().click();
    await waitNoRow(usbRe);

    step('Edit the TB USB controller in the qdev-host dialog (not submitted)');
    await rowClick(/^USB Controller \(tb/, true);
    await waitWin(/DeviceEdit/);
    await page.waitForTimeout(500);
    const ctl = await page.evaluate(() => { const w = Ext.WindowManager.getActive(); const f = w.down('[qdevArg=ports]'); return { plugin: w.down('[name=plugin]').getValue(), ports: f && f.getValue(), kmsHidden: w.down('[name=kms]').isHidden() }; });
    step(`controller dialog: ${JSON.stringify(ctl)}`);
    if (ctl.plugin !== 'tb-xhci' || !ctl.ports || !ctl.kmsHidden) await fail('USB controller dialog');
    await page.evaluate(() => Ext.WindowManager.getActive().close());

    step('Add > PCI Device: Bus selector (not submitted)');
    await addMenu('PCI Device');
    await waitWin(/PCIEdit/);
    await page.waitForTimeout(1500);
    const pciChoices = await page.evaluate(() => { const w = Ext.WindowManager.getActive(); const c = w.down('#qdevBus'); c.setValue('tb'); return { choices: c.getStore().getRange().map((r) => r.get('label')), xvga: w.down('[name=x-vga]') && w.down('[name=x-vga]').isDisabled() }; });
    step(`pci: ${JSON.stringify(pciChoices)}`);
    await shot('7-pci-dialog-tb');
    await page.evaluate(() => Ext.WindowManager.getActive().close());

    step('Remove the TB network device');
    await rowClick(netRe);
    await page.locator('.x-btn', { hasText: /^Remove$/ }).first().click();
    await waitWin(/MessageBox/);
    await page.locator('.x-message-box .x-btn', { hasText: /^Yes$/ }).first().click();
    await waitNoRow(netRe);
    await shot('8-final');
    step(`final rows: ${JSON.stringify(await rows())}`);
    if (errors.length) step(`page errors:\n${errors.join('\n')}`);
    console.log('PASS');
} catch (e) { await fail(e.stack || String(e)); }
await browser.close();

/*
 * qdev-host Hardware panel integration
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Browser-only overlay for the Proxmox VE 9 web UI: qdev-host devices appear
 * as rows of a VM's Hardware panel and can be added (Add > qdev-host Device)
 * and removed there.  The rows do not live in the VM config; they come from
 * the node's qdev-host service (qdev-host-api.service), which is called with
 * this session's PVE ticket in an Authorization header and authorises it
 * through the node's own /access/ticket.  Nothing here changes pveproxy.
 */
(function () {
    if (typeof Ext === 'undefined' || typeof PVE === 'undefined' || typeof Proxmox === 'undefined') { return; }
    const ENDPOINT = (typeof window !== 'undefined' && window.QDEV_HOST_ENDPOINT) || { port: 8017 };
    const ROW_PREFIX = 'qdev:';
    const ERROR_KEY = ROW_PREFIX + '!';
    const ID_RE = /^[A-Za-z][A-Za-z0-9_-]{0,31}$/;
    const GROUP = 32; // after PCI devices (30), before serial ports (35)

    // Behind QSM Direct's TCP 8006 gateway (qsm-pve-direct-gateway from exp230) the
    // node service is published under /qdev-host/ of the page's own origin, so a
    // browser that reaches only port 8006 can use it, with the certificate it has
    // already accepted for the page.  Only one answer means the gateway has no
    // such route: pveproxy itself answering the path (an older gateway passes
    // every path to it).  Then the panel uses the service's own port for the rest
    // of the page's life.  A failed or timed-out request, or an error page of
    // HAProxy (it sends no Server header), says nothing about the route: the
    // request is retried once and the next refresh uses the gateway again.
    const GATEWAY_PREFIX = '/qdev-host';
    let viaGateway = !!(typeof window !== 'undefined' && window.QSM_DIRECT_ENDPOINT && window.QSM_DIRECT_ENDPOINT.gateway);
    const portOrigin = () => {
        const address = new URL(location.origin || `https://${location.hostname}`);
        address.port = String(ENDPOINT.port);
        return address.origin;
    };
    const serviceOrigin = () => (viaGateway ? `${location.origin}${GATEWAY_PREFIX}` : portOrigin());
    const serverOf = (response) => (response && response.headers.get('Server')) || '';
    const fromService = (response) => !!response
        && (/^qdev-host-api\b/.test(serverOf(response)) || response.headers.get('X-QDev-Host') === '1');
    // pveproxy (Server: pve-api-daemon/...) or another web server answered instead of the service
    const otherServer = (response) => !!response && !fromService(response) && serverOf(response) !== '';
    const warn = (...args) => { if (typeof console !== 'undefined') { console.warn('qdev-host:', ...args); } };
    const request = async (method, path, params) => {
        const cookieName = (Proxmox.Setup && Proxmox.Setup.auth_cookie_name) || 'PVEAuthCookie';
        const ticket = Ext.util.Cookies.get(cookieName);
        const user = Proxmox.UserName;
        if (!ticket || !user) { throw new Error('not authenticated'); }
        const headers = { 'Authorization': `Bearer ${ticket}`, 'X-QDev-User': user };
        // Through the gateway the request is same-origin and goes with the page's own
        // credentials mode, so the browser sends it on the page's connections.  A page
        // opened by address while the certificate names the node (the user accepted the
        // warning) is otherwise unusable: Chrome keeps requests without credentials on
        // separate connections and does not apply the accepted warning to them, so their
        // TLS handshake fails and fetch reports "Failed to fetch".  The PVE cookie that
        // then comes along is ignored by the service, which authorises only the
        // Authorization header.  The service's own port is another origin: no cookie.
        const options = (gateway) => {
            const value = { method, mode: 'cors', cache: 'no-store', headers,
                credentials: gateway ? 'same-origin' : 'omit',
                signal: AbortSignal.timeout(method === 'GET' ? 15000 : 90000) };
            if (method !== 'GET' && params) {
                headers['Content-Type'] = 'application/json';
                value.body = JSON.stringify(params);
            }
            return value;
        };
        // Set when no answer of the service came back: a network error, a timeout, or,
        // through the gateway, a reply of pveproxy or HAProxy.  On the service's own
        // port only the service can answer (and its Server header is not visible to a
        // cross-origin script anyway).
        let failure = null;
        const TIMEOUT = 'no answer in time';
        const send = async () => {
            const gateway = viaGateway;
            const url = `${serviceOrigin()}${path}`;
            failure = null;
            try {
                const response = await fetch(url, options(gateway));
                if (gateway && !fromService(response)) {
                    failure = `HTTP ${response.status}${serverOf(response) ? ` from ${serverOf(response)}` : ''}`;
                    warn(`${method} ${url}: the node service did not answer (${failure})`);
                }
                return response;
            } catch (error) {
                failure = error && error.name === 'TimeoutError' ? TIMEOUT : String((error && error.message) || error);
                warn(`${method} ${url}: ${failure}`);
                return null;
            }
        };
        let response = await send();
        if (viaGateway && otherServer(response)) {
            // the gateway has no /qdev-host/ route, so the service has not seen the request
            warn(`the TCP 8006 gateway has no ${GATEWAY_PREFIX}/ route; using ${portOrigin()} from now on`);
            viaGateway = false;
            response = await send();
        } else if (method === 'GET' && failure && failure !== TIMEOUT) {
            response = await send(); // a dropped connection or a passing gateway error
        }
        if (failure) {
            const where = viaGateway ? `through the TCP 8006 gateway (${serviceOrigin()})` : `at ${serviceOrigin()}`;
            const error = new Error(`qdev-host node service did not answer ${where}: ${failure}`);
            error.unreachable = true;
            error.viaGateway = viaGateway;
            throw error;
        }
        const body = await response.json().catch(() => ({}));
        if (!response.ok) {
            const error = new Error(body.message || body.errors || `HTTP ${response.status}`);
            error.status = response.status;
            throw error;
        }
        return body.data;
    };
    const errorText = (error) => Ext.htmlEncode(error && error.message ? error.message : String(error));
    const showError = (error) => Ext.Msg.alert(gettext('Error'), errorText(error));
    // The latest node service answer per VM, for the Edit dialog that PVE opens itself.
    const latestInfo = {};
    // QEMU's own devices on the VM's TB bus (hot-plug PCIe root ports with Thunderbolt
    // ACPI properties): added in PVE's own PCI, Network and USB dialogs (Bus), the USB
    // controller as a qdev-host Device
    const TB_KINDS = {
        'tb-pci': { label: 'PCI Device', editor: 'PVE.qemu.PCIEdit', icon: 'cube', prefix: 'tbpci' },
        'tb-net': { label: 'Network Device', editor: 'PVE.qemu.NetworkEdit', icon: 'exchange', prefix: 'tbnet' },
        'tb-xhci': { label: 'USB Controller', editor: 'PVE.qdevHost.DeviceEdit', icon: 'usb', prefix: 'tbxhci' },
        'tb-usb': { label: 'USB Device', editor: 'PVE.qemu.USBEdit', icon: 'usb', prefix: 'tbusb' },
    };
    const TB_ORDER = ['tb-xhci', 'tb-usb', 'tb-pci', 'tb-net'];
    const stateText = (dev, info) => {
        if (dev.state === 'attached') { return gettext('attached') + (dev.pci ? ` (${dev.pci})` : ''); }
        if (dev.kind === 'tb') {
            if (info && !info.running) { return 'present from the VM start'; }
            return dev.needs_restart ? 'defined, the TB bus appears after the next VM start' : 'not attached';
        }
        if (dev.kind === 'vhost-user-gpu') {
            if (dev.needs_restart) { return 'defined, present from the next VM start'; }
            return dev.state === 'process' ? 'GPU process running' : 'starts with the VM';
        }
        if (dev.state === 'process') {
            return dev.needs_restart ? 'process running, root port appears after the next VM start'
                : 'process running, not attached';
        }
        return dev.needs_restart ? 'defined, attaches after the next VM start' : gettext('stopped');
    };
    const describe = (dev, info) => {
        if (dev.kind === 'tb') {
            const where = dev.plugin === 'tb-usb' ? `on ${dev.bus}` : `TB port ${dev.bus}`;
            return `${where}${dev.args ? `, ${dev.args}` : ''} — ${stateText(dev, info)}`;
        }
        let text = dev.plugin;
        if (dev.args) { text += `, ${dev.args}`; }
        if (dev.kms) { text += `, kms=${dev.kms}`; }
        if (dev.display === false && dev.kind !== 'vhost-user-gpu') { text += ', no Display1'; }
        if (dev.persistent) { text += ', persistent'; }
        text = `${text} — ${stateText(dev, info)}`;
        return dev.warning ? `${text} — ${dev.warning}: open and save it before the next start` : text;
    };

    const PORT_HINT = 'A PCIe root port (qdev-rp0) is added to the VM\'s <i>args</i> for hot-plug; '
        + 'it exists after the next VM start, until then the device stays defined but not attached.';
    const TB_HINT = 'The VM\'s TB bus (4 hot-plug PCIe ports with Thunderbolt ACPI properties) is added to its '
        + '<i>args</i>; it exists after the next VM start, until then the device is present from the next start.';
    const showMessages = (result) => {
        const messages = (result && result.messages) || [];
        if (messages.some((m) => /next VM start|VM is stopped|not attached|^note:|plugged again|failed|not released/.test(m))) {
            Ext.Msg.alert('qdev-host', messages.map(Ext.htmlEncode).join('<br>'));
        }
    };

    // ---- Add dialog ------------------------------------------------------
    Ext.define('PVE.qdevHost.DeviceEdit', {
        extend: 'Proxmox.window.Edit',
        subject: 'qdev-host Device',
        isCreate: true,
        isAdd: true,
        width: 560,
        url: '/qdev-host', // unused: submit() below talks to the node service
        qdevInfo: null,
        qdevVmid: null,
        qdevResult: null,
        fieldDefaults: { labelWidth: 130 },

        autoLoad: false,

        initComponent: function () {
            const me = this;
            // PVE's Edit button and double click open this class with confid "qdev:ID"
            const editId = me.confid && String(me.confid).startsWith(ROW_PREFIX) ? String(me.confid).slice(ROW_PREFIX.length) : null;
            if (editId) {
                me.isCreate = false;
                me.isAdd = false;
                me.qdevVmid = me.qdevVmid || Number(me.pveSelNode && me.pveSelNode.data && me.pveSelNode.data.vmid);
                me.qdevInfo = me.qdevInfo || latestInfo[me.qdevVmid];
            }
            const info = me.qdevInfo || {};
            const editDev = editId ? (info.devices || []).find((d) => d.id === editId) : null;
            if (editId && !editDev) {
                me.items = [{ xtype: 'displayfield', value: 'The device list is not loaded yet; close this window and try again.' }];
                me.callParent();
                return;
            }
            me.qdevEditDev = editDev;
            const editArgs = {};
            if (editDev && editDev.args) {
                editDev.args.split(',').forEach((kv) => { const i = kv.indexOf('='); if (i > 0) { editArgs[kv.slice(0, i)] = kv.slice(i + 1); } });
            }
            const plugins = (info.plugins || []).filter((p) => !p.dialog);
            const used = new Set((info.devices || []).map((d) => d.id));
            const freeId = (plugin) => {
                const base = String(plugin || 'qdev').replace(/[^A-Za-z0-9]/g, '').toLowerCase() || 'qdev';
                let n = 0;
                while (used.has(`${base}${n}`)) { n++; }
                return `${base}${n}`;
            };
            const argsPanel = Ext.create('Ext.container.Container', {
                layout: 'anchor', defaults: { anchor: '100%' }, items: [],
            });
            const buildArgs = (plugin) => {
                argsPanel.removeAll();
                const def = plugins.find((p) => p.name === plugin);
                const spec = (def && def.args) || [];
                const current = (name, fallback) => (editDev && editArgs[name] !== undefined ? editArgs[name] : fallback);
                spec.forEach((arg) => {
                    const common = { name: `arg_${arg.name}`, fieldLabel: arg.label || arg.name, qdevArg: arg.name,
                        qdevType: arg.type, emptyText: arg.hint || '', submitValue: false, allowBlank: true };
                    if (arg.hint) { common.autoEl = { 'data-qtip': arg.hint }; }
                    if (arg.type === 'int') {
                        argsPanel.add(Ext.apply(common, { xtype: 'proxmoxintegerfield', minValue: arg.min,
                            maxValue: arg.max, value: current(arg.name, arg.default) }));
                    } else if (arg.type === 'bool') {
                        const value = current(arg.name, arg.default ? '1' : '0');
                        argsPanel.add(Ext.apply(common, { xtype: 'proxmoxcheckbox', checked: value === true || value === '1' }));
                    } else if (arg.type === 'choice') {
                        // a choice may replace an older argument (`legacy`): show what that one meant
                        let value = current(arg.name, undefined);
                        if (value === undefined && editDev && arg.legacy && editArgs[arg.legacy.name] !== undefined) {
                            value = (arg.legacy.map || {})[editArgs[arg.legacy.name]];
                        }
                        argsPanel.add(Ext.apply(common, {
                            xtype: 'combobox', editable: false, queryMode: 'local', displayField: 'label', valueField: 'value',
                            value: value !== undefined ? value : arg.default,
                            store: { fields: ['value', 'label'], data: (arg.choices || []).map((c) => ({ value: c[0], label: c[1] })) },
                        }));
                    } else if (arg.type === 'render') {
                        argsPanel.add(Ext.apply(common, {
                            xtype: 'combobox', editable: true, queryMode: 'local', displayField: 'label', valueField: 'path',
                            value: current(arg.name, arg.default || ''),
                            store: { fields: ['path', 'label'], data: (info.render_nodes || []).map((n) => ({ path: n.path, label: `${n.path} (${n.driver}, ${n.pci})` })) },
                        }));
                    } else {
                        argsPanel.add(Ext.apply(common, { xtype: 'textfield', value: current(arg.name, arg.default || '') }));
                    }
                });
                const descr = me.down('#qdevPluginDescr');
                if (descr) { descr.setValue(def && def.description ? Ext.htmlEncode(def.description) : ''); }
                // fields a plugin kind does not use (a GPU of QEMU's display has no KMS output of its own)
                const fields = (def && def.fields) || { kms: true, display: true, persistent: true };
                ['kms', 'display', 'persistent'].forEach((name) => {
                    const field = me.down(`[name=${name}]`);
                    if (field) { field.setHidden(!fields[name]); field.setDisabled(!fields[name]); }
                });
                const portHint = me.down('#qdevPortHint');
                const tb = def && def.kind === 'tb';
                if (portHint) {
                    portHint.setHidden(!!editDev || (tb ? !!(info.tb && info.tb.in_args) : !!info.port_in_args));
                    portHint.setValue(tb ? TB_HINT : PORT_HINT);
                }
            };
            const first = editDev ? editDev.plugin : (plugins.length ? plugins[0].name : '');
            me.items = [
                {
                    xtype: 'proxmoxKVComboBox', name: 'plugin', fieldLabel: gettext('Plugin'),
                    comboItems: plugins.map((p) => [p.name, p.title && p.title !== p.name ? `${p.name} — ${p.title}` : p.name]),
                    value: first, allowBlank: false, disabled: !plugins.length || !!editDev,
                    listeners: {
                        change: (field, value) => {
                            buildArgs(value);
                            const idField = me.down('[name=id]');
                            if (idField && !idField.qdevTouched) { idField.suspendEvents(); idField.setValue(freeId(value)); idField.resumeEvents(); }
                        },
                    },
                },
                { xtype: 'displayfield', itemId: 'qdevPluginDescr', fieldLabel: ' ', labelSeparator: '', value: '' },
                {
                    xtype: 'textfield', name: 'id', fieldLabel: gettext('Device ID'), allowBlank: false,
                    regex: ID_RE, regexText: 'letters, digits, "-" and "_", starting with a letter (max 32)',
                    value: editDev ? editDev.id : freeId(first), readOnly: !!editDev,
                    listeners: { change: (field) => { field.qdevTouched = true; } },
                },
                argsPanel,
                {
                    xtype: 'combobox', name: 'kms', fieldLabel: 'KMS output', editable: true, queryMode: 'local',
                    displayField: 'label', valueField: 'spec', allowBlank: true,
                    emptyText: gettext('none') + ' (Display1 only)',
                    value: editDev ? (editDev.kms || '') : '',
                    store: { fields: ['spec', 'label'], data: (info.kms_outputs || []).map((o) => ({ spec: o.spec, label: `${o.spec} (${o.status})` })) },
                    autoEl: { 'data-qtip': 'Show the device scanout on a physical connector of this node: /dev/dri/cardN:CONNECTOR' },
                },
                {
                    xtype: 'proxmoxcheckbox', name: 'display', fieldLabel: 'Display1 export', checked: editDev ? editDev.display !== false : true,
                    boxLabel: 'export the scanout over org.qemu.Display1 (QSM Direct)',
                },
                {
                    xtype: 'proxmoxcheckbox', name: 'persistent', fieldLabel: gettext('Persistent'), checked: editDev ? !!editDev.persistent : false,
                    boxLabel: 'keep the device process running while the VM is stopped',
                },
                {
                    xtype: 'displayfield', fieldLabel: ' ', labelSeparator: '', userCls: 'pmx-hint', itemId: 'qdevPortHint',
                    hidden: !!info.port_in_args || !!editDev, value: PORT_HINT,
                },
            ];
            me.callParent();
            buildArgs(first);
        },

        submit: function () {
            const me = this;
            const form = me.formPanel.getForm();
            if (!form.isValid()) { return; }
            const field = (name) => me.down(`[name=${name}]`);
            const args = [];
            me.formPanel.query('[qdevArg]').forEach((f) => {
                const value = f.getValue();
                if (f.xtype === 'proxmoxcheckbox' && !value && !me.qdevEditDev) {
                    // a new device: an unticked box stays out unless the plugin's default is on
                    const def = (me.qdevInfo.plugins || []).find((p) => p.name === field('plugin').getValue());
                    const spec = def && (def.args || []).find((a) => a.name === f.qdevArg);
                    if (!spec || !spec.default) { return; }
                }
                if (value === '' || value === null || value === undefined) { return; }
                if (f.xtype === 'proxmoxcheckbox') { args.push(`${f.qdevArg}=${value ? 1 : 0}`); return; }
                args.push(`${f.qdevArg}=${value}`);
            });
            const body = {
                id: field('id').getValue(), plugin: field('plugin').getValue(), args: args.join(','),
                kms: field('kms').getValue() || null, display: !!field('display').getValue(),
                persistent: !!field('persistent').getValue(),
            };
            const edit = me.qdevEditDev;
            if (edit) { delete body.id; delete body.plugin; }
            me.setLoading(gettext('Please wait...'));
            const call = edit
                ? request('PUT', `/api/v1/vms/${me.qdevVmid}/devices/${encodeURIComponent(edit.id)}`, body)
                : request('POST', `/api/v1/vms/${me.qdevVmid}/devices`, body);
            call.then((result) => {
                me.setLoading(false);
                me.qdevResult = result;
                me.close();
                showMessages(result);
            }).catch((error) => {
                me.setLoading(false);
                showError(error);
            });
        },
    });

    // ---- Hardware panel rows ------------------------------------------
    Ext.define('PVE.qdevHost.HardwareViewOverlay', {
        override: 'PVE.qemu.HardwareView',
        initComponent: function () {
            const me = this;
            me.callParent();
            const vmid = Number(me.pveSelNode && me.pveSelNode.data && me.pveSelNode.data.vmid);
            if (!vmid || !me.rstore || !me.rows) { return; }
            const caps = Ext.state.Manager.get('GuiCap') || {};
            const mayEdit = !!(caps.vms && caps.vms['VM.Config.HWType']);
            me.qdevInfo = null;
            me.qdevError = null;
            let inflight = null;
            let shown = '';

            const clearRows = (store) => {
                const stale = [];
                store.each((record) => { if (String(record.getId()).startsWith(ROW_PREFIX)) { stale.push(record); } });
                if (stale.length) { store.remove(stale); }
                Object.keys(me.rows).forEach((key) => { if (key.startsWith(ROW_PREFIX)) { delete me.rows[key]; } });
            };
            const addRows = (store) => {
                clearRows(store);
                if (me.qdevInfo && Array.isArray(me.qdevInfo.devices)) {
                    me.qdevInfo.devices.forEach((dev, index) => {
                        const key = ROW_PREFIX + dev.id;
                        const tb = TB_KINDS[dev.plugin];
                        me.rows[key] = {
                            group: GROUP, order: tb ? 100 + TB_ORDER.indexOf(dev.plugin) * 50 + index : index,
                            header: tb ? `${tb.label} (${dev.id})` : `qdev-host (${dev.id})`,
                            iconCls: tb ? tb.icon : (dev.kms ? 'television' : 'microchip'),
                            never_delete: !mayEdit,
                            editor: mayEdit ? (tb ? tb.editor : 'PVE.qdevHost.DeviceEdit') : undefined,
                            renderer: (value) => Ext.htmlEncode(value),
                        };
                        store.add({ key, value: describe(dev, me.qdevInfo) });
                    });
                } else if (me.qdevError) {
                    me.rows[ERROR_KEY] = {
                        group: GROUP, header: 'qdev-host', iconCls: 'exclamation-triangle', never_delete: true,
                        renderer: (value, metaData) => {
                            if (me.qdevUnreachable && me.qdevUnreachableViaGateway) {
                                // the page's own origin and certificate: nothing to accept in the browser
                                metaData.tdAttr = `data-qtip="${Ext.htmlEncode('Retried at the next refresh. On the node: systemctl status qdev-host-api; qsm-pve-direct-gateway routes')}"`;
                            } else if (me.qdevUnreachable) {
                                metaData.tdAttr = `data-qtip="${Ext.htmlEncode('If the browser does not trust the certificate of ' + serviceOrigin() + ', open it once to accept it, then reload this page')}"`;
                                return `${Ext.htmlEncode(value)} — <a href="${Ext.htmlEncode(serviceOrigin())}/" target="_blank" rel="noopener">${gettext('open')} ${Ext.htmlEncode(serviceOrigin())}</a>`;
                            }
                            return Ext.htmlEncode(value);
                        },
                    };
                    store.add({ key: ERROR_KEY, value: me.qdevError });
                }
            };
            const refresh = () => {
                if (inflight) { return inflight; }
                inflight = request('GET', `/api/v1/vms/${vmid}`).then((info) => {
                    me.qdevInfo = info;
                    latestInfo[vmid] = info;
                    me.qdevError = null;
                    me.qdevUnreachable = false;
                }).catch((error) => {
                    me.qdevInfo = null;
                    me.qdevUnreachable = !!error.unreachable;
                    me.qdevUnreachableViaGateway = !!error.viaGateway;
                    me.qdevError = error.message;
                }).finally(() => {
                    inflight = null;
                    const addItem = me.down('#addQdev');
                    if (addItem) {
                        addItem.setDisabled(!mayEdit || !!(me.qdevInfo && me.qdevInfo.q35 === false) || !!me.qdevUnreachable);
                    }
                    const signature = JSON.stringify([me.qdevInfo && me.qdevInfo.devices, me.qdevError]);
                    if (signature !== shown && !me.destroyed) {
                        shown = signature;
                        me.reload();
                    }
                });
                return inflight;
            };
            // Ahead of the grid's DiffStore copy, which listens to the same event.
            me.rstore.on('load', (store, _records, success) => {
                if (success === false) { return; }
                addRows(store);
                refresh();
            }, null, { priority: 1000 });

            // Remove on a qdev row goes to the node service instead of the VM config.
            const removeBtn = me.query('button').find((button) => button.RESTMethod && button.dangerous);
            if (removeBtn) {
                const stockHandler = removeBtn.handler;
                removeBtn.handler = function () {
                    const record = me.getSelectionModel().getSelection()[0];
                    const key = record && record.data ? String(record.data.key) : '';
                    if (!key.startsWith(ROW_PREFIX)) { return stockHandler.apply(this, arguments); }
                    const id = key.slice(ROW_PREFIX.length);
                    Ext.MessageBox.defaultButton = 2;
                    Ext.Msg.show({
                        title: gettext('Confirm'), icon: Ext.Msg.WARNING, buttons: Ext.Msg.YESNO, defaultFocus: 'no',
                        message: Ext.String.format(gettext('Are you sure you want to remove entry {0}'),
                            `'${Ext.htmlEncode(record.get('key') && me.rows[key] ? me.rows[key].header : `qdev-host (${id})`)}'`)
                            + (/^tb-/.test(((me.qdevInfo && me.qdevInfo.devices) || []).filter((d) => d.id === id).map((d) => d.plugin)[0] || '')
                                ? '<br>The device is hot-unplugged from the running VM.'
                                : '<br>The device is hot-unplugged and its process stopped.'),
                        callback: (answer) => {
                            if (answer !== 'yes') { return; }
                            me.setLoading(gettext('Please wait...'));
                            request('DELETE', `/api/v1/vms/${vmid}/devices/${encodeURIComponent(id)}`)
                                .then(showMessages)
                                .catch(showError)
                                .finally(() => { me.setLoading(false); shown = ''; refresh(); });
                        },
                    });
                    return undefined;
                };
            }

            // Add > qdev-host Device
            const addBtn = me.query('button').find((button) => button.menu && button.menu.down && button.menu.down('#addPci'));
            if (addBtn) {
                addBtn.menu.add({
                    text: 'qdev-host Device', itemId: 'addQdev', iconCls: 'fa fa-fw fa-microchip black', disabled: !mayEdit,
                    handler: () => {
                        const open = () => {
                            if (!me.qdevInfo) { showError(new Error(me.qdevError || 'qdev-host node service unavailable')); return; }
                            Ext.create('PVE.qdevHost.DeviceEdit', {
                                autoShow: true, qdevInfo: me.qdevInfo, qdevVmid: vmid, pveSelNode: me.pveSelNode,
                                listeners: { destroy: () => { shown = ''; refresh(); } },
                            });
                        };
                        if (me.qdevInfo) { open(); } else { refresh().then(open); }
                    },
                });
            }
        },
    });
    // ---- Bus selector in PVE's own PCI, Network and USB dialogs ---------------
    // "Standard" keeps PVE's behaviour.  "TB bus" sends the device to qdev-host, which puts
    // it on a hot-plug port of the VM's TB bus.  The TB rows of the panel open the same
    // dialogs: their settings are handed to the dialog as if they were in the VM config.
    const tbVmid = (win) => Number(win.pveSelNode && win.pveSelNode.data && win.pveSelNode.data.vmid);
    const tbFreeId = (info, prefix, taken) => {
        const used = new Set((info.devices || []).map((d) => d.id).concat(taken || []));
        let n = 0;
        while (used.has(`${prefix}${n}`)) { n++; }
        return `${prefix}${n}`;
    };
    const TB_TEXT = {
        pci: 'Passed through on a hot-plug port of the VM\'s TB bus (qdev-host): added to and removed from the '
            + 'running VM, Thunderbolt ACPI properties for macOS. Primary GPU and the PCI-Express flag do not apply.',
        net: 'On a hot-plug port of the VM\'s TB bus (qdev-host): added to and removed from the running VM, '
            + 'Thunderbolt ACPI properties for macOS. Models: VirtIO, Intel E1000E, VMware vmxnet3. The PVE '
            + 'firewall does not filter it.',
        usb: 'On a USB controller of the VM\'s TB bus (qdev-host): added to and removed from the running VM, '
            + 'also for guests such as macOS that do not take PVE\'s own USB hot-plug.',
    };
    const tbBusChoices = (kind, info) => {
        if (kind !== 'usb') { return [['tb', 'TB bus (qdev-host, hot-plug)']]; }
        const ctrls = (info.devices || []).filter((d) => d.plugin === 'tb-xhci');
        if (!ctrls.length) { return [['tb:', 'TB bus, new USB controller (qdev-host, hot-plug)']]; }
        return ctrls.map((c) => [`tb:${c.id}`, `TB bus, USB controller ${c.id} (qdev-host, hot-plug)`]);
    };
    const tbEditDev = (win) => {
        const key = String(win.confid || '');
        if (!key.startsWith(ROW_PREFIX)) { return null; }
        const info = latestInfo[tbVmid(win)] || {};
        return (info.devices || []).find((d) => d.id === key.slice(ROW_PREFIX.length)) || { id: key.slice(ROW_PREFIX.length), missing: true };
    };
    // a TB device's args in the syntax of the dialog's VM config key
    const tbConfigValue = (kind, dev) => (kind === 'pci' ? `${dev.args},pcie=1` : dev.args);
    const tbFieldsFor = (win, kind, on) => {
        const names = { pci: ['x-vga', 'pcie'], net: ['firewall', 'disconnect'], usb: [] }[kind];
        names.forEach((name) => {
            const field = win.down(`[name=${name}]`);
            if (field) { field.setDisabled(on); }
        });
    };
    const tbAddSelector = (win, kind) => {
        const vmid = tbVmid(win);
        const info = latestInfo[vmid];
        const edit = win.qdevTbDev;
        if (!edit && !(info && info.local && info.q35)) { return; }
        const choices = edit
            ? [['tb', edit.plugin === 'tb-usb' ? `TB bus, USB controller ${edit.bus}` : `TB bus, port ${edit.bus}`]]
            : [['pve', gettext('Standard') + ' (PVE)']].concat(tbBusChoices(kind, info));
        const hint = Ext.create('Ext.form.field.Display', {
            itemId: 'qdevBusHint', userCls: 'pmx-hint', hidden: !edit, value: TB_TEXT[kind], padding: '0 0 6 0',
        });
        const combo = Ext.create('Ext.form.field.ComboBox', {
            itemId: 'qdevBus', fieldLabel: 'Bus', labelWidth: 100, submitValue: false, editable: false,
            queryMode: 'local', displayField: 'label', valueField: 'value', disabled: !!edit,
            value: choices[0][0], padding: '0 0 6 0',
            store: { fields: ['value', 'label'], data: choices.map((c) => ({ value: c[0], label: c[1] })) },
            listeners: {
                change: (field, value) => {
                    const on = value !== 'pve';
                    hint.setHidden(!on);
                    tbFieldsFor(win, kind, on);
                    if (on && info && info.tb && !info.tb.in_args) {
                        hint.setValue(`${TB_TEXT[kind]}<br>${TB_HINT}`);
                    }
                },
            },
        });
        win.formPanel.insert(0, [combo, hint]);
        if (edit) { tbFieldsFor(win, kind, true); }
    };
    const tbSubmit = (win, kind) => {
        const form = win.formPanel.getForm();
        if (!form.isValid()) { return; }
        const vmid = tbVmid(win);
        const info = latestInfo[vmid] || {};
        const values = win.getValues();
        const key = Object.keys(values).find((k) => k.startsWith(ROW_PREFIX) || /^(hostpci|net|usb)\d+$/.test(k));
        if (!key || !values[key]) { showError(new Error('the dialog gave no device settings')); return; }
        let args = values[key];
        if (kind === 'pci') {
            const p = PVE.Parser.parsePropertyString(args, 'host');
            if (p['x-vga'] && String(p['x-vga']) !== '0') { showError(new Error('Primary GPU needs the standard bus')); return; }
            delete p.pcie; delete p['x-vga'];
            const first = p.host !== undefined ? 'host' : 'mapping';
            args = [`${first}=${p[first]}`].concat(Object.keys(p).filter((k) => k !== first).map((k) => `${k}=${p[k]}`)).join(',');
        } else if (kind === 'net') {
            // the PVE firewall does not apply to TB bus devices
            args = args.split(',').filter((kv) => !/^firewall=/.test(kv)).join(',');
        }
        const edit = win.qdevTbDev;
        win.setLoading(gettext('Please wait...'));
        let call;
        if (edit) {
            call = request('PUT', `/api/v1/vms/${vmid}/devices/${encodeURIComponent(edit.id)}`, { args });
        } else {
            const bus = win.down('#qdevBus').getValue();
            const plugin = `tb-${kind}`;
            if (kind === 'usb') {
                const ctrl = bus.slice(3);
                const add = (ctrlId) => request('POST', `/api/v1/vms/${vmid}/devices`,
                    { id: tbFreeId(info, TB_KINDS[plugin].prefix), plugin, args, bus: ctrlId });
                if (ctrl) {
                    call = add(ctrl);
                } else {
                    const ctrlId = tbFreeId(info, TB_KINDS['tb-xhci'].prefix);
                    call = request('POST', `/api/v1/vms/${vmid}/devices`, { id: ctrlId, plugin: 'tb-xhci', args: '' })
                        .then((first) => add(ctrlId).then((second) => ({
                            messages: [].concat(first.messages || [], second.messages || []),
                        })));
                }
            } else {
                call = request('POST', `/api/v1/vms/${vmid}/devices`, { id: tbFreeId(info, TB_KINDS[plugin].prefix), plugin, args });
            }
        }
        call.then((result) => {
            win.setLoading(false);
            win.close();
            showMessages(result);
        }).catch((error) => {
            win.setLoading(false);
            showError(error);
        });
    };
    const tbOverride = (cls, kind) => {
        Ext.define(`PVE.qdevHost.${kind}EditOverlay`, {
            override: cls,
            initComponent: function () {
                const me = this;
                me.qdevTbDev = tbEditDev(me);
                me.callParent(arguments);
                if (me.qdevTbDev && me.qdevTbDev.missing) {
                    Ext.defer(() => { showError(new Error('The device list is not loaded yet; close this window and try again.')); me.close(); }, 1);
                    return;
                }
                tbAddSelector(me, kind);
            },
            load: function (options) {
                const me = this;
                const dev = me.qdevTbDev;
                if (dev && !dev.missing && options && options.success) {
                    const success = options.success;
                    options = Ext.apply({}, {
                        success: function (response, opts) {
                            response.result.data = Ext.apply({}, response.result.data, { [me.confid]: tbConfigValue(kind, dev) });
                            return success.call(this, response, opts);
                        },
                    }, options);
                }
                return me.callParent([options]);
            },
            submit: function () {
                const me = this;
                const bus = me.down('#qdevBus');
                if (me.qdevTbDev || (bus && bus.getValue() !== 'pve')) {
                    tbSubmit(me, kind);
                    return undefined;
                }
                return me.callParent(arguments);
            },
        });
    };
    tbOverride('PVE.qemu.PCIEdit', 'pci');
    tbOverride('PVE.qemu.NetworkEdit', 'net');
    tbOverride('PVE.qemu.USBEdit', 'usb');
})();

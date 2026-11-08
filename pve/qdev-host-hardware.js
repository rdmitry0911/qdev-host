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
    // browser that reaches only port 8006 can use it.  An older gateway lacks the
    // route: then pveproxy or HAProxy answers instead of the service, and the
    // panel falls back to the service's own port for the rest of the page's life.
    const GATEWAY_PREFIX = '/qdev-host';
    let viaGateway = !!(typeof window !== 'undefined' && window.QSM_DIRECT_ENDPOINT && window.QSM_DIRECT_ENDPOINT.gateway);
    const portOrigin = () => {
        const address = new URL(location.origin || `https://${location.hostname}`);
        address.port = String(ENDPOINT.port);
        return address.origin;
    };
    const serviceOrigin = () => (viaGateway ? `${location.origin}${GATEWAY_PREFIX}` : portOrigin());
    const fromService = (response) => !!response && /^qdev-host-api\b/.test(response.headers.get('Server') || '');
    const request = async (method, path, params) => {
        const cookieName = (Proxmox.Setup && Proxmox.Setup.auth_cookie_name) || 'PVEAuthCookie';
        const ticket = Ext.util.Cookies.get(cookieName);
        const user = Proxmox.UserName;
        if (!ticket || !user) { throw new Error('not authenticated'); }
        const headers = { 'Authorization': `Bearer ${ticket}`, 'X-QDev-User': user };
        const options = () => {
            const value = { method, mode: 'cors', cache: 'no-store', credentials: 'omit', headers,
                signal: AbortSignal.timeout(method === 'GET' ? 15000 : 90000) };
            if (method !== 'GET' && params) {
                headers['Content-Type'] = 'application/json';
                value.body = JSON.stringify(params);
            }
            return value;
        };
        const send = () => fetch(`${serviceOrigin()}${path}`, options()).catch(() => null);
        let response = await send();
        // Retry on the port only when the service cannot have seen the request:
        // another server answered, or a read failed before any reply.
        if (viaGateway && !fromService(response) && (response || method === 'GET')) {
            viaGateway = false;
            response = await send();
        }
        if (!response) {
            const error = new Error(`qdev-host node service at ${serviceOrigin()} is unreachable`);
            error.unreachable = true;
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
    const stateText = (dev) => {
        if (dev.state === 'attached') { return gettext('attached') + (dev.pci ? ` (${dev.pci})` : ''); }
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
    const describe = (dev) => {
        let text = dev.plugin;
        if (dev.args) { text += `, ${dev.args}`; }
        if (dev.kms) { text += `, kms=${dev.kms}`; }
        if (dev.display === false && dev.kind !== 'vhost-user-gpu') { text += ', no Display1'; }
        if (dev.persistent) { text += ', persistent'; }
        text = `${text} — ${stateText(dev)}`;
        return dev.warning ? `${text} — ${dev.warning}: open and save it before the next start` : text;
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
            const plugins = info.plugins || [];
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
                if (portHint) { portHint.setHidden(!!info.port_in_args || !!editDev); }
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
                    hidden: !!info.port_in_args || !!editDev,
                    value: 'A PCIe root port (qdev-rp0) is added to the VM\'s <i>args</i> for hot-plug; '
                        + 'it exists after the next VM start, until then the device stays defined but not attached.',
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
                const messages = (result && result.messages) || [];
                if (messages.some((m) => /next VM start|VM is stopped|not attached|^note:|plugged again/.test(m))) {
                    Ext.Msg.alert('qdev-host', messages.map(Ext.htmlEncode).join('<br>'));
                }
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
                        me.rows[key] = {
                            group: GROUP, order: index, header: `qdev-host (${dev.id})`,
                            iconCls: dev.kms ? 'television' : 'microchip',
                            never_delete: !mayEdit, editor: mayEdit ? 'PVE.qdevHost.DeviceEdit' : undefined,
                            renderer: (value) => Ext.htmlEncode(value),
                        };
                        store.add({ key, value: describe(dev) });
                    });
                } else if (me.qdevError) {
                    me.rows[ERROR_KEY] = {
                        group: GROUP, header: 'qdev-host', iconCls: 'exclamation-triangle', never_delete: true,
                        renderer: (value, metaData) => {
                            if (me.qdevUnreachable) {
                                metaData.tdAttr = `data-qtip="${Ext.htmlEncode('Open ' + serviceOrigin() + '/ once to accept the node certificate, then reload this page')}"`;
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
                        message: Ext.String.format(gettext('Are you sure you want to remove entry {0}'), `'qdev-host (${Ext.htmlEncode(id)})'`)
                            + '<br>The device is hot-unplugged and its process stopped.',
                        callback: (answer) => {
                            if (answer !== 'yes') { return; }
                            me.setLoading(gettext('Please wait...'));
                            request('DELETE', `/api/v1/vms/${vmid}/devices/${encodeURIComponent(id)}`)
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
})();

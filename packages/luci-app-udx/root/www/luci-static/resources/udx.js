'use strict';
'require baseclass';
'require rpc';
'require poll';
'require dom';
'require ui';
var getDeviceStatus = rpc.declare({
    reject : true,
    object : 'udx',
    method : 'status',
    expect : {'' : {}},
    filter : function(r) {
        if (!r || r.accepted === false || !r.board)
            throw new Error((r || {}).error || _("No valid device status was received"));
        return r;
    }
});
var getSystemInfo =
    rpc.declare({reject : true, object : 'system', method : 'info', expect : {'' : {}}});
function getStatus() {
    return Promise.all([ getDeviceStatus(), L.resolveDefault(getSystemInfo(), {}) ])
        .then(function(results) {
            var device = results[0], info = results[1];
            device.memory = info.memory || {};
            device.swap = info.swap || {};
            return device;
        });
}
var cellular = rpc.declare({
    reject : true,
    object : 'udx',
    method : 'cellular',
    params : [ 'operation', 'arguments' ],
    expect : {'' : {}}
});
function value(v, suffix) {
    if (v === null || v === undefined || v === '')
        return _("Not available");
    if (typeof v === 'number' && !isFinite(v))
        return _("Not available");
    if (typeof v === 'boolean')
        return v ? _("Yes") : _("No");
    if (Array.isArray(v))
        return v.length ? v.map(function(x) {
                               return value(x);
                           }).join('、') +
                              (suffix || '')
                        : _("Not available");
    if (typeof v === 'object')
        return _("Not available");
    return String(v) + (suffix || '');
}
function table(title, rows) {
    if (!rows.length)
        rows = [ [ _("Status"), _("Not available") ] ];
    return E('section', {class : 'cbi-section'}, [
        E('h3', {}, title), E('table', {class : 'table'}, rows.map(function(r) {
            return E('tr', {class : 'tr'}, [
                E('td', {class : 'td', style : 'width:38%'}, r[0]),
                E('td', {class : 'td', style : 'overflow-wrap:anywhere'},
                  r[1] instanceof Node ? r[1] : value(r[1], r[2]))
            ]);
        }))
    ]);
}
function state(row) {
    return row && row.available === true ? row : {};
}
function operationText(op) {
    if (!op || !op.state)
        return _("Not available");
    var names = {
        queued : _("Queued"),
        running : _("Running"),
        complete : _("Completed"),
        failed : _("Failed"),
        uncertain : _("Uncertain result")
    };
    var result = op.result || {}, text = names[op.state] || op.state;
    if (op.error)
        return text + ': ' + _(op.error);
    if (op.state === 'complete') {
        if (result.saved === true && result.applied === false)
            return _("Configuration saved but not applied: ") + value(result.waiting_for);
        if (result.authentication_readback_verified === true)
            return _("APN and authentication settings applied; readback matches");
        if (result.saved_id)
            return _("Profile saved, ID ") + result.saved_id;
        if (result.deleted_id)
            return _("Profile deleted, ID ") + result.deleted_id;
        var queryErrors = result.cell_query_errors || result.info_errors;
        if (queryErrors && Object.keys(queryErrors).length)
            return _("Query completed with missing fields; check the query errors");
        if (result.modem_submission_confirmed)
            return _("Modem confirmed SMS submission; delivery status is not available");
        if ([ 'nr_capability', 'physical_slot' ].indexOf(op.operation) >= 0 &&
            result.readback_verified !== true)
            return _("No matching readback; the transition is not yet confirmed");
        if (result.readback_verified === true)
            return _("Completed; settings readback matches");
        if (result.command_accepted && result.readback_verified === false)
            return _("Command accepted; cell-lock state cannot be read back");
        if (result.connected === false && result.waiting_for)
            return _("Not connected: ") + result.waiting_for;
        if (result.connected === true)
            return _("Data session established");
    }
    return text;
}
function notice(r) {
    var text =
        r.uncertain
            ? _("No result was received. Check the latest operation.") + (r.error ? _(r.error) : '')
            : (r.error ? _(r.error) : null) ||
                  (r.accepted === true ? _("Request queued. Wait for the actual result below.")
                                       : _("Request was not executed"));
    ui.addNotification(null, E('p', {}, text), r.error || r.uncertain ? 'warning' : 'info');
    return r;
}
function submit(op, args) {
    return cellular(op, args).then(notice).catch(function(e) {
        notice({uncertain : true, error : e.message});
        return {uncertain : true};
    });
}
function confirm(title, description, action) {
    ui.showModal(title, [
        E('p', {}, description),
        E('div', {class : 'right'},
          [
              E('button', {class : 'btn', click : ui.hideModal}, _("Cancel")), E('button', {
                  class : 'btn cbi-button-action',
                  click : function(ev) {
                      ev.target.disabled = true;
                      ui.hideModal();
                      action();
                  }
              },
                                                                                 _("Confirm"))
          ])
    ]);
}
function live(title, render, onStatus) {
    var box = E('div', {}, E('p', {}, _("Reading device status…")));
    function update() {
        return getStatus()
            .then(function(s) {
                if (onStatus)
                    onStatus(s);
                dom.content(box, render(s));
            })
            .catch(function(e) {
                dom.content(box, E('p', {class : 'alert-message warning'},
                                   _("Device status is not available: ") + e.message));
            });
    }
    poll.add(update, 5);
    update();
    return E('div', {}, [ stylesheet(), E('h2', {}, title), box ]);
}
function mib(v) {
    return typeof v === 'number' ? (v / 1048576).toFixed(1) + ' MiB' : _("Not available");
}
function stylesheet() {
    return E(
        'style', {},
        '.udx-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:1em}.udx-metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:1em;margin:1em 0}.udx-metric{display:flex;flex-direction:column;gap:.4em}.udx-metric strong{font-size:1.3em;font-variant-numeric:tabular-nums}.udx-metric-label,.udx-subtle{opacity:.75}.udx-actions{display:flex;flex-wrap:wrap;gap:.5em;margin:.8em 0}.udx-bands{display:flex;flex-wrap:wrap;gap:.6em}.udx-band{display:inline-flex;align-items:center;gap:.4em;padding:.4em .6em;border:1px solid #9996;border-radius:3px}.udx-table-scroll{overflow-x:auto}.udx-console{white-space:pre-wrap;overflow-wrap:anywhere;max-height:28em;overflow:auto;font-family:monospace}.udx-sim-active{border-left:3px solid currentColor;padding-left:1em}');
}
function section(title, content) {
    return E('section', {class : 'cbi-section'},
             [ E('h3', {}, title), E('div', {class : 'cbi-section-node'}, content) ]);
}
function metric(label, reading, unit) {
    return E(
        'div', {class : 'udx-metric'},
        [ E('span', {class : 'udx-metric-label'}, label), E('strong', {}, value(reading, unit)) ]);
}
function tabs(items) {
    var holder = E('div'), buttons = [];
    function select(index) {
        buttons.forEach(function(b, i) {
            b.className = i === index ? 'cbi-tab' : 'cbi-tab-disabled';
        });
        dom.content(holder, items[index].content);
    }
    var bar = E('ul', {class : 'cbi-tabmenu'}, items.map(function(item, i) {
        var b = E('li', {class : i === 0 ? 'cbi-tab' : 'cbi-tab-disabled'},
                  E('a', {
                      href : '#',
                      click : function(ev) {
                          if (ev.preventDefault)
                              ev.preventDefault();
                          select(i);
                      }
                  },
                    item.title));
        buttons.push(b);
        return b;
    }));
    select(0);
    return E('div', {}, [ bar, holder ]);
}
function telemetryNote(c, name, now) {
    var t = c.telemetry || {}, q = (t.queries || {})[name] || {};
    if (t.enabled === false)
        return E('p', {class : 'udx-subtle'},
                 _("Background queries are paused. Showing the latest cached data."));
    if (!q.attempt_uptime)
        return E('p', {class : 'udx-subtle'},
                 _("Collecting information in the background. This page updates automatically."));
    var age = typeof q.updated_uptime === 'number' ? Math.max(0, Math.floor(now - q.updated_uptime))
                                                   : null;
    return E('p', {class : q.error ? 'alert-message warning' : 'udx-subtle'},
             (age !== null ? _('Updated %d seconds ago; ').format(age) : '') +
                 (q.error ? _("Query failed: ") + q.error
                  : q.state === 'partial'
                      ? _("The modem omitted some fields. Available information has been retained.")
                      : _("Automatic background refresh")));
}
function simState(s) {
    return ({
               READY : _("Ready"),
               NOT_READY : _("Not ready"),
               REFRESHING : _("Reading"),
               'SIM PIN' : _("PIN required"),
               'SIM PUK' : _("PUK required"),
               'NOT INSERTED' : _("No SIM inserted")
           })[s] ||
           s || _("Reading");
}
function radioName(c) {
    var a = (c.operator || {}).act;
    if (a === 11)
        return '5G NR';
    if (a === 13)
        return '5G NSA';
    if (a === 7)
        return '4G LTE';
    return ({
               nr_only : '5G NR',
               lte_only : '4G LTE',
               nr_lte : _("5G / 4G automatic"),
               nr_lte_wcdma_gsm : _("Automatic")
           })[c.mode] ||
           c.mode || _("Reading");
}
function reading(c, key) {
    if (c[key] !== null && c[key] !== undefined && c[key] !== '')
        return value(c[key]);
    var e = (c.info_errors || {})[key];
    return e ? (e === 'Modem returned OK without information' ? _("Not provided by the modem")
                                                              : _("Query failed: ") + e)
             : _("Reading");
}
function signalText(c) {
    var signal = c.signal || {};
    if (typeof signal.rssi_dbm === 'number')
        return value(signal.rssi_dbm, ' dBm (RSSI)');
    var serving = (c.cells || [])
                      .filter(function(cell) {
                          return cell.serving && typeof cell.rsrp === 'number';
                      })
                      .sort(function(a, b) {
                          return a.rat === 'nr' ? -1 : b.rat === 'nr' ? 1 : 0;
                      });
    if (serving.length)
        return value(serving[0].rsrp, ' dBm (RSRP)');
    return signal.csq === 99 ? _('RSSI not reported by the modem (CSQ 99)') : _('Reading');
}
return baseclass.extend({
    stylesheet : stylesheet,
    section : section,
    metric : metric,
    tabs : tabs,
    telemetryNote : telemetryNote,
    simState : simState,
    radioName : radioName,
    reading : reading,
    signalText : signalText,
    nrCapabilityName : function(mode) {
        return ({sa_nsa : 'SA + NSA', nsa_only : _('NSA only'), sa_only : _('SA only')})[mode] ||
               _('Reading');
    },
    getStatus : getStatus,
    cellular : cellular,
    smsStatus :
        rpc.declare({reject : true, object : 'udx', method : 'sms_status', expect : {'' : {}}}),
    apnStatus :
        rpc.declare({reject : true, object : 'udx', method : 'apn_status', expect : {'' : {}}}),
    smsHistoryStatus : rpc.declare(
        {reject : true, object : 'udx', method : 'sms_history_status', expect : {'' : {}}}),
    smsReceipt : rpc.declare({
        reject : true,
        object : 'udx',
        method : 'sms_receipt',
        params : [ 'request_id' ],
        expect : {'' : {}}
    }),
    usb : rpc.declare({
        reject : true,
        object : 'udx',
        method : 'usb',
        params : [ 'profile', 'acm' ],
        expect : {'' : {}}
    }),
    radioTransition : function(t) {
        if (!t || !t.state)
            return _("Not available");
        var names = {
            idle : _("Idle"),
            switching : _("Switching; the cellular connection is temporarily interrupted"),
            complete : _("Transition finished; check the readback in the latest operation"),
            failed : _("Switch failed"),
            uncertain : _("Switch result is uncertain")
        };
        return (names[t.state] || t.state) + (t.error ? ': ' + t.error : '');
    },
    networkAddresses : function(d, family) {
        var kernel = state(d.cellular_kernel), netifd = state(d.cellular_netifd),
            network = netifd.network || {},
            key = family === 'ipv6' ? 'ipv6_global_addresses' : 'ipv4_addresses';
        if (family === 'ipv6' && kernel.available)
            return kernel[key] || [];
        return Array.isArray(network[key]) ? network[key] : [];
    },
    value : value,
    table : table,
    state : state,
    live : live,
    mib : mib,
    operationText : operationText,
    notice : notice,
    submit : submit,
    confirm : confirm,
    supports : function(c, op) {
        return Array.isArray(c.supported_operations) && c.supported_operations.indexOf(op) >= 0;
    },
    input : function(name, label, placeholder) {
        return E('label', {class : 'cbi-value'}, [
            E('span', {class : 'cbi-value-title'}, label), E('input', {
                name : name,
                class : 'cbi-input-text',
                placeholder : placeholder || '',
                style : 'max-width:24em'
            })
        ]);
    },
    val : function(box, name) {
        return box.querySelector('[name="' + name + '"]').value.trim();
    },
    registration : function(r) {
        return !r                       ? _("Not available")
               : r.registered === true  ? (r.roaming ? _("Registered, roaming") : _("Registered"))
               : r.registered === false ? _("Not registered")
                                        : _("Not available");
    },
    operationTable : function(d) {
        var op = state(d.cellular_operation);
        return table(_("Latest cellular operation"), [
            [ _("Operation"), op.operation ], [ _("Result"), operationText(op) ],
            [ _("Operation ID"), op.id ]
        ]);
    }
});

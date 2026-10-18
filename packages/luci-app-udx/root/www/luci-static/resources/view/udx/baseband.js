'use strict';
'require view';
'require udx';
return view.extend({
    render : function() {
        var current = {}, busy = true, buttons = [], automatic = E('input', {type : 'checkbox'}),
            roaming = E('input', {type : 'checkbox'}), initialized = false;
        function action(label, op, args, description) {
            var button = E('button', {
                class : 'cbi-button',
                disabled : true,
                click : function() {
                    if (busy)
                        return;
                    var value = typeof args === 'function' ? args() : args;
                    udx.confirm(label, description, function() {
                        busy = true;
                        button.disabled = true;
                        udx.submit(op, value);
                    });
                }
            },
                           label);
            buttons.push({button : button, operation : op});
            return button;
        }
        var on = action(
                _("Enable cellular radio"), 'radio', {enabled : true},
                _("Turn on cellular radio and reconnect according to the connection policy.")),
            off = action(
                _("Airplane mode"), 'radio', {enabled : false},
                _("Turn off cellular radio. The current data connection will be interrupted."));
        var connect = action(_("Connect data"), 'connect', {automatic : false},
                             _("Establish a data connection using the current APN.")),
            disconnect = action(_("Disconnect data"), 'disconnect', {},
                                _("Disconnect the current cellular data session."));
        var policy = action(_("Save connection policy"), 'policy', function() {
            return {automatic_connect : automatic.checked, roaming : roaming.checked};
        }, _("Save automatic connection and roaming preferences."));
        var info = E('button', {
            class : 'cbi-button',
            disabled : true,
            click : function() {
                info.disabled = true;
                udx.submit('modem_info', {});
            }
        },
                     _("Read modem and SIM information"));
        var presets = [
            [ 'auto', _("5G / 4G automatic"), _("Automatically select NR or LTE") ],
            [ 'nr_only', _("5G only"), _("Use NR networks") ],
            [ 'lte_only', _("4G only"), _("Use LTE networks") ],
            [ 'nsa_only', _("NSA only"), _("LTE anchor with non-standalone NR") ]
        ];
        var live = udx.live(
            _("Network management"),
            function(d) {
                var c = udx.state(d.cellular), r = udx.state(d.runtime),
                    slot = c.physical_sim || {}, session = c.data_session || {},
                    profile = c.profile || {},
                    activePreset = !c.mode                                       ? null
                                   : c.mode === 'lte_only'                       ? 'lte_only'
                                   : c.mode === 'nr_only'                        ? 'nr_only'
                                   : (c.nr_capability || {}).mode === 'nsa_only' ? 'nsa_only'
                                                                                 : 'auto';
                var cards = presets.map(function(x) {
                    return E('div', {class : 'cbi-section-node'}, [
                        E('strong', {}, x[1]), E('p', {class : 'udx-subtle'}, x[2]),
                        E('button', {
                            class : activePreset === x[0] ? 'cbi-button cbi-button-positive'
                                                          : 'cbi-button',
                            disabled : (busy || !udx.supports(c, 'network_preset')) || null,
                            click : function() {
                                udx.confirm(
                                    _("Change network mode"),
                                    _("Switch to \"") + x[1] +
                                        _("\". The cellular connection may be interrupted briefly."),
                                    function() {
                                        udx.submit('network_preset', {mode : x[0]});
                                    });
                            }
                        },
                          activePreset === x[0] ? _("Current mode") : _("Apply"))
                    ]);
                });
                var sims = (slot.choices || []).map(function(n) {
                    var selected = n === slot.slot;
                    return E(
                        'div',
                        {class : selected ? 'cbi-section-node udx-sim-active' : 'cbi-section-node'},
                        [
                            E('h4', {}, 'SIM ' + (n + 1)),
                            E('p', {},
                              selected ? _("Active · ") + udx.simState((c.sim || {}).state)
                                       : _("Not selected; this slot has not been queried")),
                            selected ? E('p', {}, 'ICCID ' + udx.reading(c, 'iccid')) : '',
                            E('button', {
                                class : selected ? 'cbi-button cbi-button-positive' : 'cbi-button',
                                disabled :
                                    (busy || selected || !udx.supports(c, 'physical_slot')) || null,
                                click : function() {
                                    udx.confirm(
                                        _("Switch SIM slot"),
                                        _("Switch to SIM ") + (n + 1) +
                                            _(". The connection will be interrupted and SIM information will be read again."),
                                        function() {
                                            udx.submit('physical_slot', {slot : n});
                                        });
                                }
                            },
                              selected ? _("Active slot") : _("Use this slot"))
                        ]);
                });
                return [
    E('div',{class:'udx-grid'},[
     udx.table(_("Cellular status"),[[_("Cellular radio"),c.cfun===1?_("On"):c.cfun===0?_("Airplane mode"):_("Reading")],[_("Data connection"),c.state==='pdp_active'?_("Connected"):c.state],[_("Mobile network operator"),(c.operator||{}).name],[_("SIM status"),udx.simState((c.sim||{}).state)],[_("Currently roaming"),c.roaming],[_("Runtime modem status"),r.baseband]]),
     udx.table(_("Current network"),[[_("Access technology"),udx.radioName(c)],[_("5G capability"),udx.nrCapabilityName((c.nr_capability||{}).mode)],[_("LTE registration"),udx.registration(c.registration)],[_("NR registration"),udx.registration(c.nr_registration)],[_("Signal strength"),udx.signalText(c)],[_("Latest error"),c.last_error||_("None")]])
    ]),
    udx.section(_("Network mode"),[E('div',{class:'udx-grid'},cards),udx.telemetryNote(c,'advanced_refresh',d.sample_uptime)]),
    udx.section(_("5G access capability"),[E('div',{class:'udx-actions'},Object.keys((c.nr_capability||{}).choices||{}).map(function(name){var cap=c.nr_capability,choice=cap.choices[name];return E('button',{class:cap.mode===name?'cbi-button cbi-button-positive':'cbi-button',disabled:(busy||!udx.supports(c,'nr_capability'))||null,click:function(){udx.confirm(_("Change 5G access mode"),_("Switch to \"")+udx.nrCapabilityName(name)+_("\". The cellular connection may be interrupted briefly."),function(){udx.submit('nr_capability',{mode:name});});}},udx.nrCapabilityName(name)+(cap.mode===name?_(" (current)"):''));})),E('p',{class:'udx-subtle'},udx.radioTransition(c.radio_transition))]),
    udx.section(_("SIM slots"),[E('div',{class:'udx-grid'},sims.length?sims:[E('p',{},slot.error?_("Slot query failed: ")+slot.error:_("Reading SIM slot information in the background."))]),E('p',{class:'udx-subtle'},_("Physical slots and the active logical SIM are shown separately. Card presence is not inferred for an unselected slot.")),udx.telemetryNote(c,'radio_options',d.sample_uptime)]),
    udx.table(_("SIM and data connection"),[['IMEI',udx.reading(c,'imei')],['ICCID',udx.reading(c,'iccid')],['IMSI',udx.reading(c,'imsi')],['APN',profile.apn||_("Selected automatically by the carrier")],[_("PDP type"),profile.pdp_type],['IPv4',session.ipv4],[_("IPv6 (actual kernel addresses)"),udx.networkAddresses(d,'ipv6')],['DNS',session.dns],[_("Data interface"),session.interface]]),
    E('details',{},[E('summary',{},_("Extended modem information and query diagnostics")),udx.table(_("Extended information"),[[_("Manufacturer"),udx.reading(c,'manufacturer')],[_("Model"),udx.reading(c,'model')],[_("Revision"),udx.reading(c,'revision')],[_("SIM query error"),c.sim_error||_("None")],[_("Collection status"),(c.telemetry||{}).enabled===false?_("Paused"):_("Periodic background queries")]]),udx.telemetryNote(c,'modem_info',d.sample_uptime)]),
    udx.operationTable(d)
   ];
            },
            function(d) {
                current = udx.state(d.cellular);
                var op = udx.state(d.cellular_operation);
                busy = !current.available || current.state === 'uncertain' ||
                       [ 'queued', 'running' ].indexOf(op.state) >= 0 ||
                       [ 'switching', 'uncertain' ].indexOf(
                           (current.radio_transition || {}).state) >= 0;
                buttons.forEach(function(x) {
                    x.button.disabled = busy || !udx.supports(current, x.operation);
                });
                info.disabled = busy || !udx.supports(current, 'modem_info');
                if (!initialized && current.available &&
                    typeof current.automatic_connect === 'boolean' &&
                    typeof current.roaming_allowed === 'boolean') {
                    automatic.checked = current.automatic_connect === true;
                    roaming.checked = current.roaming_allowed === true;
                    initialized = true;
                }
            });
        return E('div',{},[live,udx.section(_("Connection settings"),[E('div',{class:'udx-actions'},[on,off,connect,disconnect]),E('div',{class:'cbi-value'},[E('label',{class:'cbi-value-title'},_("Connect automatically at boot")),E('div',{class:'cbi-value-field'},automatic)]),E('div',{class:'cbi-value'},[E('label',{class:'cbi-value-title'},_("Allow roaming")),E('div',{class:'cbi-value-field'},roaming)]),E('div',{class:'udx-actions'},[policy,info,E('a',{class:'cbi-button',href:L.url('admin','udx','apn')},_("APN settings"))])])]);
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});

'use strict';
'require view';
'require udx';
return view.extend({
    render : function() {
        var showIdentity = false;
        function identity(v) {
            return v ? (showIdentity ? v : '••••••••' + String(v).slice(-4)) : _("Reading");
        }
        return udx.live(_("System monitor"), function(d) {
            var b = d.board || {}, c = udx.state(d.cellular), r = udx.state(d.runtime),
                m = d.memory || {}, u = d.usb || {}, s = c.data_session || {};
            var cells = (c.cells || [])
                            .filter(function(x) {
                                return x.serving;
                            })
                            .sort(function(a, b) {
                                return a.rat === 'nr' ? -1 : 1;
                            }),
                cell = cells[0] || {}, q = c.qos || {};
            var band = cell.band || ((cell.band_candidates || []).length
                                         ? cell.band_candidates.join(' / ') + _(" (unconfirmed)")
                                         : null);
            return [
    udx.section(_("Current network"),[
     E('div',{class:'udx-metrics'},[udx.metric(_("Network type"),udx.radioName(c)),udx.metric(_("Band"),band),udx.metric('ARFCN',cell.arfcn),udx.metric('PCI',cell.pci),udx.metric('RSRP',cell.rsrp,' dBm'),udx.metric('RSRQ',cell.rsrq,' dB'),udx.metric('SINR',cell.sinr,' dB')]),
     udx.telemetryNote(c,'serving_refresh',d.sample_uptime),
     E('div',{class:'udx-actions'},[E('a',{class:'cbi-button',href:L.url('admin','udx','baseband')},_("Network management")),E('a',{class:'cbi-button',href:L.url('admin','udx','cellular')},_("Bands and cells"))])
    ]),
    E('div',{class:'udx-grid'},[
     udx.table(_("Modem and SIM"),[[_("Mobile network operator"),(c.operator||{}).name],[_("SIM status"),udx.simState((c.sim||{}).state)],[_("Signal strength"),udx.signalText(c)],['QCI',q.qci],[_("Negotiated downlink rate"),udx.value(q.downlink_kbps===undefined?null:q.downlink_kbps/1000,' Mbps')],[_("Negotiated uplink rate"),udx.value(q.uplink_kbps===undefined?null:q.uplink_kbps/1000,' Mbps')],['IMEI',identity(c.imei)],['ICCID',identity(c.iccid)],[_("Identity information"),E('button',{class:'cbi-button',click:function(){showIdentity=!showIdentity;}},showIdentity?_("Hide"):_("Show"))]]),
     udx.table(_("System status"),[[_("Device"),b.model],[_("Kernel"),b.kernel],[_("Uptime"),Math.floor(d.sample_uptime),_(" seconds")],[_("Total memory"),udx.mib(m.total)],[_("Available memory"),udx.mib(m.available)],[_("Page cache"),udx.mib(m.cached)]].concat((d.thermal||[]).map(function(t){return [t.name,udx.value(t.temperature_c,' °C')];})))
    ]),
    udx.table(_("Connection status"),[['IPv4',s.ipv4],[_("IPv6 (actual kernel addresses)"),udx.networkAddresses(d,'ipv6')],[_("USB network"),u.profile],[_("USB connection"),u.state],[_("Current USB speed"),u.speed],[_("LTE registration"),udx.registration(c.registration)],[_("NR registration"),udx.registration(c.nr_registration)]]),
    udx.table(_("Platform runtime status"),[[_("Core service"),r.core],[_("Modem service"),r.baseband],[_("CP/NV error"),(r.cpnv_error===0||r.cpnv_error==='0')?_("None"):r.cpnv_error],[_("Runtime read error"),r.available?(r.error||_("None")):(d.runtime||{}).error],[_("Latest cellular error"),c.last_error||_("None")]].concat((d.cpu||[]).map(function(x){return ['CPU '+x.policy,udx.value(x.current_khz===null?null:x.current_khz/1000,' MHz')+' / '+udx.value(x.governor)];}))),
    E('div',{class:'udx-actions'},[['usb',_("USB management")],['sensors',_("Temperature, DDR and memory")],['boot',_("Boot and storage")]].map(function(x){return E('a',{class:'cbi-button',href:L.url('admin','udx',x[0])},x[1]);}))
   ];
        });
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});

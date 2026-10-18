'use strict';
'require view';
'require udx';
'require ui';
return view.extend({
    render : function() {
        var profile = E('select', {class : 'cbi-input-select'},
                        [ 'ncm', 'ncm-software' ].map(function(p) {
                            return E('option', {value : p},
                                     p === 'ncm' ? _("NCM · hardware acceleration")
                                                 : _("NCM · software processing"));
                        })),
            acm = E('input', {type : 'checkbox'}), pending = false, initialized = false;
        var apply = E('button', {
            class : 'cbi-button cbi-button-action',
            click : function() {
                if (pending)
                    return;
                var selected = profile.value, consoleEnabled = acm.checked;
                udx.confirm(
                    _("Apply USB configuration"),
                    _("Switching between hardware and software processing saves the configuration and reboots the device. Changing ACM alone reconnects USB. Both processing modes use NCM. The ACM console requires authentication."),
                    function() {
                        pending = true;
                        apply.disabled = true;
                        udx.usb(selected, consoleEnabled)
                            .then(udx.notice)
                            .catch(function(e) {
                                udx.notice({uncertain : true, error : e.message});
                            })
                            .finally(function() {
                                pending = false;
                            });
                    });
            }
        },
                      _("Apply USB configuration"));
        return E('div', {}, [
            udx.live(
                _("USB management"),
                function(d) {
                    var u = d.usb || {};
                    return [ udx.table(_("Actual state"), [
                        [ _("Applied configuration"), u.profile ],
                        [
                            _("ACM console"), u.acm === '1'   ? _("Enabled")
                                              : u.acm === '0' ? _("Disabled")
                                                              : null
                        ],
                        [ 'UDC', u.udc ], [ _("Connection status"), u.state ],
                        [ _("Current speed"), u.speed ],
                        [
                            _("Hardware acceleration"),
                            u.offload_state && /running=1/.test(u.offload_state) ? _("Running")
                                                                                 : _("Not running")
                        ],
                        [ _("Pending request"), u.pending || _("None") ],
                        [ _("Last apply exit code"), u.result ], [ _("Device MAC"), u.dev_mac ],
                        [ _("Host MAC"), u.host_mac ], [ _("Serial number"), u.serial ]
                    ]) ];
                },
                function(d) {
                    var u = d.usb || {};
                    apply.disabled = pending || !!u.pending || u.available !== true;
                    if (!initialized && u.profile) {
                        profile.value = u.profile;
                        acm.checked = u.acm === '1';
                        initialized = true;
                    }
                }),
            E('section', {class : 'cbi-section'},
              [
                  E('h3', {}, _("USB configuration")), profile,
                  E('label', {style : 'margin-left:1em'},
                    [ acm, _(" Enable the authenticated ACM console") ]),
                  E('p', {},
                    _("Hardware-accelerated NCM is the default. Software processing is also available; switching processing mode requires a reboot. Windows 11, macOS and Linux use their built-in network drivers. Check the applied configuration and exit code after reboot and reconnection.")),
                  apply
              ])
        ]);
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});

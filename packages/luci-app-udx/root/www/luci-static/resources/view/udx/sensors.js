'use strict';
'require view';
'require udx';
return view.extend({
    render : function() {
        return udx.live(_("Temperature, DDR and memory"), function(d) {
            var m = d.memory || {};
            return [
                udx.table(_("Temperature"), (d.thermal || []).map(function(t) {
                    return [ t.name, udx.value(t.temperature_c, ' °C') ];
                })),
                udx.table('CPU', (d.cpu || []).map(function(c) {
                    return [
                        c.policy,
                        udx.value(c.current_khz === null ? null : c.current_khz / 1000, ' MHz') +
                            ' / ' + udx.value(c.governor)
                    ];
                })),
                udx.table(_("Memory"),
                          [
                              [ _("Total memory"), udx.mib(m.total) ],
                              [ _("Available memory"), udx.mib(m.available) ],
                              [ _("Free memory"), udx.mib(m.free) ],
                              [ _("Page cache"), udx.mib(m.cached) ],
                              [ _("Swap"), udx.mib((d.swap || {}).total) ]
                          ]),
                E('p', {},
                  _("DDR frequency is coordinated by the PM_SYS firmware. The AP vote is not the current frequency."))
            ].concat((d.ddr || []).length ? d.ddr.map(function(x) {
                return udx.table('DDR · ' + x.name, [
                    [
                        _("Current frequency"),
                        udx.value(x.current_hz === null ? null : x.current_hz / 1000000, ' MHz')
                    ],
                    [ _("Available frequencies (Hz)"), x.available_frequencies ],
                    [ _("Frequency scaling policy"), x.governor ],
                    [ _("AP vote (Hz)"), x.ap_vote_hz ], [ _("CP vote (Hz)"), x.cp_vote_hz ],
                    [ _("Firmware DFS"), x.firmware_dfs_enabled ],
                    [ _("Automatic firmware frequency scaling"), x.firmware_auto_enabled ]
                ]);
            })
                                          : [ udx.table('DDR', [ [ _("Status"), null ] ]) ]);
        });
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});

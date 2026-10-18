'use strict';
'require view';
'require udx';
return view.extend({
    render : function() {
        return udx.live(_("Boot and storage"), function(d) {
            var b = d.boot || {}, r = udx.state(d.runtime);
            return [
                udx.table(_("Boot source"),
                          [
                              [ _("Boot policy"), b.policy ], [ _("Boot target"), b.target ],
                              [ _("Root filesystem marker"), b.root_marker ],
                              [ _("Runtime core status"), r.core ],
                              [ _("Shutdown preparation"), r.quiesce ],
                              [ _("Shutdown preparation error"), r.quiesce_error ]
                          ]),
                udx.table(_("Mounted filesystems"), (d.storage || []).map(function(v) {
                    return [
                        v.mount,
                        [
                            v.source, v.type, v.readonly ? _("Read-only") : _("Writable"),
                            udx.mib(v.free_bytes) + _(" available / ") + udx.mib(v.total_bytes)
                        ].join(' / ')
                    ];
                })),
                udx.table(_("UBI volumes"), (d.ubi || []).map(function(v) {
                    return [
                        v.name || v.device,
                        v.device + ' / ' + udx.value(v.type) + ' / ' + udx.mib(v.data_bytes)
                    ];
                }))
            ];
        });
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});

'use strict';
'require view';
'require udx';
return view.extend({
    render : function() {
        var current = {}, busy = true, selected = {lte : [], nr : []}, dirty = false,
            pending = null;
        var rat = E('select', {name : 'rat', class : 'cbi-input-select'},
                    [ E('option', {value : 'lte'}, 'LTE'), E('option', {value : 'nr'}, 'NR') ]),
            arfcn = E('input', {
                name : 'arfcn',
                class : 'cbi-input-text',
                type : 'number',
                min : 0,
                placeholder : 'EARFCN / NR-ARFCN'
            }),
            pci = E('input', {
                name : 'pci',
                class : 'cbi-input-text',
                type : 'number',
                min : 0,
                max : 1007,
                placeholder : 'PCI'
            });
        function submit(op, args) {
            busy = true;
            return udx.submit(op, args).then(function(r) {
                if (r.accepted && r.operation_ref)
                    pending = r.operation_ref;
            });
        }
        function lockCell(r, a, p) {
            if (busy)
                return;
            udx.confirm(
                _("Lock cell"),
                _("Lock ") + r.toUpperCase() + ' / ARFCN ' + a + ' / PCI ' + p +
                    _(". The connection may be interrupted briefly. The modem does not provide cell-lock readback."),
                function() {
                    submit('cell_lock', {rat : r, arfcn : a, pci : p});
                });
        }
        var refresh = E('button', {
            class : 'cbi-button',
            disabled : true,
            click : function() {
                refresh.disabled = true;
                submit('advanced_refresh', {});
            }
        },
                        _("Refresh band status"));
        var cellsRefresh = E('button', {
            class : 'cbi-button',
            disabled : true,
            click : function() {
                cellsRefresh.disabled = true;
                submit('cells_refresh', {});
            }
        },
                             _("Read serving and neighboring cells"));
        var custom = E('button', {
            class : 'cbi-button cbi-button-action',
            disabled : true,
            click : function() {
                var a = Number(arfcn.value), p = Number(pci.value);
                if (!arfcn.value || !pci.value || !Number.isInteger(a) || a < 0 ||
                    !Number.isInteger(p) || p < 0 || p > (rat.value === 'nr' ? 1007 : 503)) {
                    udx.notice({
                        error : _(
                            "Enter a valid channel number and PCI. LTE PCI is 0–503; NR PCI is 0–1007.")
                    });
                    return;
                }
                lockCell(rat.value, a, p);
            }
        },
                       _("Lock the specified cell"));
        function cellTable(title, rows) {
            var headers = [
                _("Radio technology"), _("Band"), 'ARFCN', 'PCI', 'RSRP (dBm)', 'RSRQ (dB)',
                'SINR (dB)', _("Operation")
            ];
            return udx.section(
                title,
                E('div', {class : 'udx-table-scroll'}, E('table', {class : 'table'}, [
                      E('tr', {class : 'tr table-titles'}, headers.map(function(h) {
                          return E('th', {class : 'th'}, h);
                      })),
                      rows.length
                          ? rows.map(function(x) {
                                var band = x.band ? x.band + (x.band_source === 'arfcn-inferred'
                                                                  ? _(" (inferred)")
                                                                  : '')
                                                  : (x.band_candidates || []).join(' / ') ||
                                                        _("Not reported");
                                return E(
                                    'tr', {class : 'tr'},
                                    [
                                        x.rat.toUpperCase(), band, x.arfcn, x.pci, x.rsrp, x.rsrq,
                                        x.sinr
                                    ]
                                        .map(function(v) {
                                            return E('td', {class : 'td'}, udx.value(v));
                                        })
                                        .concat([ E(
                                            'td', {class : 'td'},
                                            E('button', {
                                                class : 'cbi-button',
                                                disabled :
                                                    (busy || !udx.supports(current, 'cell_lock')) ||
                                                        null,
                                                click : function() {
                                                    lockCell(x.rat, x.arfcn, x.pci);
                                                }
                                            },
                                              _("Lock"))) ]));
                            })
                          : E('tr', {class : 'tr'},
                              E('td', {class : 'td', colspan : 8},
                                (current.telemetry || {}).queries
                                    ? _("No cells of this type were reported in this sample.")
                                    : _("Reading cell information in the background.")))
                  ])));
        }
        var live = udx.live(
            _("Advanced network"),
            function(d) {
                var c = udx.state(d.cellular), choices = c.band_choices || {},
                    limits = c.band_limits || {}, cells = c.cells || [];
                var groups = [
                    [
                        'LTE FDD', 'lte', (choices.lte || []).filter(function(x) {
                            return Number(x.slice(1)) < 33;
                        })
                    ],
                    [
                        'LTE TDD', 'lte', (choices.lte || []).filter(function(x) {
                            return Number(x.slice(1)) >= 33;
                        })
                    ],
                    [ '5G NR', 'nr', choices.nr || [] ]
                ];
                var bandGroups = groups.map(function(group) {
                    return E('div', {class : 'cbi-section-node'}, [
                        E('h4', {}, group[0]),
                        E('div', {class : 'udx-bands'}, group[2].map(function(name) {
                            var box = E('input', {
                                type : 'checkbox',
                                name : 'band_' + name,
                                disabled : (busy) || null
                            });
                            box.checked = selected[group[1]].indexOf(name) >= 0;
                            box.addEventListener('change', function() {
                                dirty = true;
                                selected[group[1]] = selected[group[1]].filter(function(x) {
                                    return x !== name;
                                });
                                if (box.checked)
                                    selected[group[1]].push(name);
                            });
                            return E('label', {class : 'udx-band'}, [ box, name ]);
                        })),
                        group[2].length
                            ? ''
                            : E('p', {class : 'udx-subtle'}, _("Reading supported bands."))
                    ]);
                });
                var count = selected.lte.length + selected.nr.length;
                var apply = E('button', {
                    class : 'cbi-button cbi-button-action',
                    disabled : (busy || !count || !choices.lte) || null,
                    click : function() {
                        udx.confirm(
                            _("Apply band restrictions"),
                            _("Apply the selected ") + count +
                                _(" bands. A radio technology with no bands selected remains unrestricted. The cellular connection may be interrupted briefly."),
                            function() {
                                submit('band_lock',
                                       {lte : selected.lte.slice(), nr : selected.nr.slice()});
                            });
                    }
                },
                              _('Apply selected bands (%d)').format(count));
                var unlock = E('button', {
                    class : 'cbi-button',
                    disabled : (busy || !choices.lte) || null,
                    click : function() {
                        udx.confirm(_("Remove band restrictions"),
                                    _("Restore automatic LTE and NR band selection."), function() {
                                        submit('unlock', {scope : 'bands'});
                                    });
                    }
                },
                               _("Remove band restrictions"));
                return [
                    udx.section(
                        _("Band locking"),
                        [
                            E('div', {class : 'udx-grid'}, bandGroups),
                            E('div', {class : 'udx-actions'}, [ apply, unlock ]),
                            E('p', {class : 'udx-subtle'},
                              [ 'lte', 'nr' ]
                                  .map(function(r) {
                                      var v = limits[r];
                                      return r.toUpperCase() + ': ' +
                                             (!v ? _("Reading")
                                              : v.unrestricted
                                                  ? _("Unrestricted")
                                                  : (v.bands || []).join(', ') ||
                                                        _("The modem reported unlisted mask bits"));
                                  })
                                  .join('; ')),
                            udx.telemetryNote(c, 'advanced_refresh', d.sample_uptime)
                        ]),
                    cellTable(_("Serving cells"), cells.filter(function(x) {
                        return x.serving;
                    })),
                    cellTable(_("Neighboring cells"), cells.filter(function(x) {
                        return !x.serving;
                    })),
                    udx.telemetryNote(c, 'serving_refresh', d.sample_uptime),
                    E('p', {class : 'udx-subtle'},
                      _("Reported bands are distinguished from bands inferred from channel numbers. The same NR channel may belong to n77 or n78; ambiguous bands are not assigned arbitrarily.")),
                    Object.keys(c.cell_query_errors || {}).length
                        ? udx.table(_("Cell query diagnostics"),
                                    Object.keys(c.cell_query_errors).map(function(k) {
                                        return [ k, c.cell_query_errors[k] ];
                                    }))
                        : '',
                    E('div', {class : 'udx-actions'},
                      [
                          E('button', {
                              class : 'cbi-button',
                              disabled : (busy) || null,
                              click : function() {
                                  udx.confirm(_("Remove cell restrictions"),
                                              _("Restore automatic cell selection."), function() {
                                                  submit('unlock', {scope : 'cells'});
                                              });
                              }
                          },
                            _("Remove cell restrictions")),
                          E('a', {class : 'cbi-button', href : L.url('admin', 'udx', 'at')},
                            _("AT command debugging"))
                      ]),
                    udx.operationTable(d)
                ];
            },
            function(d) {
                current = udx.state(d.cellular);
                var op = udx.state(d.cellular_operation);
                if (pending && op.id === pending.id && op.boot_id === pending.boot_id &&
                    op.broker_id === pending.broker_id &&
                    [ 'complete', 'failed', 'uncertain' ].indexOf(op.state) >= 0) {
                    if (op.state === 'complete' &&
                        (op.operation === 'band_lock' || op.operation === 'unlock'))
                        dirty = false;
                    pending = null;
                }
                busy = !current.available || current.state === 'uncertain' || !!pending ||
                       [ 'queued', 'running' ].indexOf(op.state) >= 0;
                if (!dirty) {
                    ['lte', 'nr'].forEach(function(r) {
                        var l = (current.band_limits || {})[r];
                        selected[r] = l && !l.unrestricted ? (l.bands || []).slice() : [];
                    });
                }
                refresh.disabled = busy || !udx.supports(current, 'advanced_refresh');
                cellsRefresh.disabled = busy || !udx.supports(current, 'cells_refresh');
                custom.disabled = busy || !udx.supports(current, 'cell_lock');
            });
        return E('div', {}, [
            live, E('div', {class : 'udx-actions'}, [ refresh, cellsRefresh ]),
            udx.section(_("Custom cell lock"),
                        [
                            E('div', {class : 'cbi-value'},
                              [
                                  E('span', {class : 'cbi-value-title'}, _("Radio technology")),
                                  E('div', {class : 'cbi-value-field'}, rat)
                              ]),
                            E('div', {class : 'cbi-value'},
                              [
                                  E('span', {class : 'cbi-value-title'}, _("Channel number")),
                                  E('div', {class : 'cbi-value-field'}, arfcn)
                              ]),
                            E('div', {class : 'cbi-value'},
                              [
                                  E('span', {class : 'cbi-value-title'}, 'PCI'),
                                  E('div', {class : 'cbi-value-field'}, pci)
                              ]),
                            custom
                        ])
        ]);
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});

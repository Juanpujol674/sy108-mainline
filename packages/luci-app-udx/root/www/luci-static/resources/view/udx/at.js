'use strict';
'require view';
'require udx';
'require dom';
return view.extend({
    render : function() {
        var busy = true, submitting = false, pending = null, history = [], current = {},
            command = E('input', {
                class : 'cbi-input-text',
                name : 'at_command',
                placeholder : _("Enter an AT command, for example AT+CSQ"),
                maxlength : 256,
                style : 'width:100%'
            }),
            timeout = E('input', {
                class : 'cbi-input-text',
                type : 'number',
                value : 15,
                min : 1,
                max : 120,
                style : 'width:6em'
            }),
            output = E('pre', {class : 'udx-console'}, _("Waiting for a command.")),
            historyBox = E('div');
        function renderHistory() {
            dom.content(historyBox, history.length ? history.map(function(row) {
                return E('details', {}, [
                    E('summary', {},
                      row.time + '  ' + row.command + '  ' +
                          (row.success     ? 'OK'
                           : row.uncertain ? _("Uncertain result")
                                           : 'ERROR')),
                    E('pre', {class : 'udx-console'},
                      row.response || row.error || _("No response body"))
                ]);
            })
                                                   : E('p', {class : 'udx-subtle'},
                                                       _("No command history yet.")));
        }
        function send() {
            if (busy || pending)
                return;
            var text = command.value.trim(), seconds = Number(timeout.value);
            if (!/^AT[\x20-\x7e]*$/.test(text) || /[;\r\n]/.test(text) || text.length > 256 ||
                !Number.isInteger(seconds) || seconds < 1 || seconds > 120) {
                udx.notice({error : _("Enter one AT command with a timeout of 1–120 seconds.")});
                return;
            }
            busy = true;
            submitting = true;
            sendButton.disabled = true;
            dom.content(output, _("Executing ") + text + ' …');
            udx.cellular('at_debug', {command : text, timeout : seconds})
                .then(function(r) {
                    submitting = false;
                    if (!r.accepted || !r.operation_ref) {
                        busy = false;
                        dom.content(output, r.error || _("Request was not accepted"));
                        return;
                    }
                    pending = r.operation_ref;
                })
                .catch(function(e) {
                    submitting = false;
                    dom.content(
                        output,
                        _("The request result was not received. Check the latest operation.\n") +
                            e.message);
                });
        }
        var sendButton =
            E('button', {class : 'cbi-button cbi-button-action', disabled : true, click : send},
              _("Send command"));
        command.addEventListener('keydown', function(ev) {
            if (ev.key === 'Enter') {
                ev.preventDefault();
                send();
            }
        });
        var live =
            udx.live(_("AT debugging"),
                     function(d) {
                         return [ udx.telemetryNote(udx.state(d.cellular), 'signal_refresh',
                                                    d.sample_uptime) ];
                     },
                     function(d) {
                         current = udx.state(d.cellular);
                         var op = udx.state(d.cellular_operation);
                         if (pending && op.id === pending.id && op.boot_id === pending.boot_id &&
                             op.broker_id === pending.broker_id &&
                             [ 'complete', 'failed', 'uncertain' ].indexOf(op.state) >= 0) {
                             var result = op.result || {};
                             result.command = result.command || command.value;
                             result.error = result.error || op.error;
                             result.uncertain = op.state === 'uncertain';
                             result.time = new Date().toLocaleTimeString();
                             history.unshift(result);
                             history = history.slice(0, 20);
                             pending = null;
                             dom.content(output, (result.response || _("No response body")) +
                                                     (result.error ? '\n' + result.error : '') +
                                                     _("\nElapsed time: ") +
                                                     udx.value(result.elapsed_ms === undefined
                                                                   ? null
                                                                   : result.elapsed_ms.toFixed(1),
                                                               ' ms'));
                             renderHistory();
                         }
                         busy = submitting || !current.available || current.state === 'uncertain' ||
                                !!pending || [ 'queued', 'running' ].indexOf(op.state) >= 0;
                         sendButton.disabled = busy || !udx.supports(current, 'at_debug');
                     });
        renderHistory();
        return E('div', {}, [
            live,
            udx.section(_("Send command"),
                        [
                            E('div', {class : 'cbi-value'},
                              [
                                  E('label', {class : 'cbi-value-title'}, _("AT command")),
                                  E('div', {class : 'cbi-value-field'}, command)
                              ]),
                            E('div', {class : 'cbi-value'},
                              [
                                  E('label', {class : 'cbi-value-title'}, _("Timeout (seconds)")),
                                  E('div', {class : 'cbi-value-field'},
                                    [ timeout, ' ', sendButton ])
                              ]),
                            E('div', {class : 'udx-actions'},
                              [
                                  [ 'IMEI', 'AT+CGSN' ], [ 'ICCID', 'AT+CCID' ],
                                  [ _("Signal"), 'AT+CSQ' ],
                                  [ _("Negotiated rate"), 'AT+CGEQOSRDP' ],
                                  [ _("Registration status"), 'AT+CEREG?' ]
                              ].map(function(x) {
                                  return E('button', {
                                      class : 'cbi-button',
                                      click : function() {
                                          command.value = x[1];
                                          send();
                                      }
                                  },
                                           x[0]);
                              })),
                            E('p', {class : 'udx-subtle'},
                              _("Commands share one control channel with background queries. Configuration commands may change network state. Factory identity and AT channel framing cannot be changed through this interface."))
                        ]),
            udx.section(_("Response"), output),
            udx.section(_("Command history"),
                        [
                            E('button', {
                                class : 'cbi-button',
                                click : function() {
                                    history = [];
                                    renderHistory();
                                    dom.content(output, _("Waiting for a command."));
                                }
                            },
                              _("Clear history")),
                            historyBox
                        ])
        ]);
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});

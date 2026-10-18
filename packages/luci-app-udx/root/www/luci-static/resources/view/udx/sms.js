'use strict';
'require view';
'require udx';
'require poll';
'require dom';
'require ui';
return view.extend({
    render : function() {
        var pending = null, operationBusy = true, send, refresh,
            storageKey = 'udx-sms-pending-request', number = E('input', {
                                                        class : 'cbi-input-text',
                                                        placeholder : _("Recipient number"),
                                                        inputmode : 'tel',
                                                        maxlength : 16
                                                    }),
            body = E('textarea', {
                class : 'cbi-input-text',
                rows : 4,
                maxlength : 402,
                placeholder : _("Message text, up to 402 BMP Unicode characters")
            }),
            messages = E('div'), result = E('p');
        try {
            var saved = sessionStorage.getItem(storageKey);
            if (/^[0-9a-f]{32}$/.test(saved || ''))
                pending = saved;
        } catch (e) {
        }
        function remember(id) {
            pending = id;
            try {
                if (id)
                    sessionStorage.setItem(storageKey, id);
                else
                    sessionStorage.removeItem(storageKey);
            } catch (e) {
            }
        }
        var current = {}, settingsInitialized = false, historyBox = E('div'), sentBox = E('div'),
            lastHistory = null,
            search =
                E('input',
                  {class : 'cbi-input-text', placeholder : _("Search number or message text")}),
            historyResult = E('p'), historyButtons = [];
        var maxInbox = E('input', {
            type : 'number',
            name : 'max_count',
            min : 10,
            max : 150,
            value : 50,
            class : 'cbi-input-text'
        }),
            maxSent = E('input', {
                type : 'number',
                name : 'max_sent_count',
                min : 1,
                max : 50,
                value : 10,
                class : 'cbi-input-text'
            }),
            direct = E('input', {type : 'checkbox', name : 'direct_receive'});
        function supported(op) {
            return udx.supports(current, op);
        }
        function archiveAction(op, args) {
            if (operationBusy || !supported(op))
                return;
            operationBusy = true;
            refreshButtons();
            udx.submit(op, args).then(function(r) {
                dom.content(historyResult,
                            r.accepted === true
                                ? _("Request queued. Check the status below for the result.")
                                : r.error || _("Request result is not available"));
            });
        }
        function refreshButtons() {
            send.disabled = !!pending || operationBusy;
            refresh.disabled = operationBusy;
            historyButtons.forEach(function(x) {
                x.button.disabled = operationBusy || !supported(x.operation);
            });
        }
        function historyButton(label, op, action) {
            var b = E('button', {class : 'cbi-button', disabled : true, click : action}, label);
            historyButtons.push({button : b, operation : op});
            return b;
        }
        var refreshHistory = historyButton(_("Refresh local history"), 'sms_history', function() {
            archiveAction('sms_history', {});
        });
        var saveSettings = historyButton(_("Save SMS settings"), 'sms_settings', function() {
            var a = Number(maxInbox.value), b = Number(maxSent.value);
            if (!Number.isInteger(a) || a < 10 || a > 150 || !Number.isInteger(b) || b < 1 ||
                b > 50) {
                udx.notice({error : _("The inbox limit is 10–150 and the outbox limit is 1–50.")});
                return;
            }
            udx.confirm(
                _("Save SMS settings"),
                _("Settings are stored in OpenWrt UCI. Lowering a limit removes the oldest local history. SIM messages and submission receipts are retained."),
                function() {
                    archiveAction(
                        'sms_settings',
                        {max_count : a, max_sent_count : b, direct_receive : direct.checked});
                });
        });
        function date(value) {
            return typeof value === 'number' ? new Date(value * 1000).toLocaleString()
                                             : value || _("Time is not available");
        }
        function renderHistory(data) {
            lastHistory = data;
            var h = udx.state(data);
            if (!h.available) {
                dom.content(
                    historyBox,
                    E('p', {},
                      data.error || _("Local history is not available. Refresh it first.")));
                return;
            }
            if (!settingsInitialized && h.settings) {
                maxInbox.value = h.settings.max_count;
                maxSent.value = h.settings.max_sent_count;
                direct.checked = h.settings.direct_receive === true;
                settingsInitialized = true;
            }
            function entry(m, folder) {
                var inbox = folder === 'inbox', state = {
                    sending : _("Submitting"),
                    submitted : _("Modem confirmed submission"),
                    failed : _("Failed"),
                    uncertain : _("Uncertain result")
                },
                    actions = [];
                if (inbox && !m.read)
                    actions.push(E('button', {
                        class : 'cbi-button',
                        disabled : (operationBusy || !supported('sms_mark_read')) || null,
                        click : function() {
                            archiveAction('sms_mark_read', {id : m.id});
                        }
                    },
                                   _("Mark as read")));
                actions.push(E('button', {
                    class : 'cbi-button cbi-button-negative',
                    disabled : (operationBusy || !supported('sms_delete_history')) || null,
                    click : function() {
                        udx.confirm(
                            _("Delete local history"),
                            _("Delete local history only. SIM messages and submission receipts are retained."),
                            function() {
                                archiveAction('sms_delete_history', {folder : folder, id : m.id});
                            });
                    }
                },
                               _("Delete local record")));
                var info =
                    inbox ? (m.read ? _("Read") : _("Unread")) + ' / ' +
                                (m.source === 'direct' ? _("Direct reception") : _("SIM archive"))
                          : (state[m.state] || _("Status is not available")) + '; ' +
                                (m.delivery_confirmed === true
                                     ? _("Delivery confirmed")
                                     : _("Delivery status is not available"));
                if (m.concat)
                    info += m.concat.complete
                                ? _("; multipart message merged (") + m.concat.total + _(" parts)")
                                : _("; multipart message received ") + (m.concat.received || 1) +
                                      '/' + m.concat.total + _(" parts");
                if (!inbox && (m.parts || []).length)
                    info += _("; confirmed ") + m.parts.length + _(" parts");
                return E('section', {class : 'cbi-section'}, [
                    E('strong', {}, m.peer || m.number || _("Number is not available")),
                    E('p', {}, date(m.timestamp || m.received_at)), E('p', {}, info),
                    E('p', {style : 'white-space:pre-wrap;overflow-wrap:anywhere'},
                      m.text === null || m.text === undefined
                          ? (m.decode_error || _("Message text is not available"))
                          : m.text),
                    m.error ? E('p', {class : 'alert-message warning'}, m.error) : '',
                    E('div', {}, actions)
                ]);
            }
            var term = search.value.trim().toLowerCase();
            function matches(m) {
                return !term || ((m.peer || m.number || '') + ' ' + (m.text || ''))
                                        .toLowerCase()
                                        .indexOf(term) >= 0;
            }
            var inbox = (h.inbox || []).filter(matches), sent = (h.sent || []).filter(matches);
            dom.content(historyBox, [
                E('p', {class : 'udx-subtle'},
                  _("Inbox: ") + inbox.length +
                      _(" messages. Multipart messages are shown as complete messages.")),
                inbox.length ? E('div', {}, inbox.slice().reverse().map(function(m) {
                    return entry(m, 'inbox');
                }))
                             : E('p', {}, _("No matching received messages."))
            ]);
            dom.content(sentBox, [
                E('p', {class : 'udx-subtle'},
                  _("Outbox: ") + sent.length +
                      _(" messages. Submission and carrier delivery reports are shown separately.")),
                sent.length ? E('div', {}, sent.slice().reverse().map(function(m) {
                    return entry(m, 'sent');
                }))
                            : E('p', {}, _("No matching sent messages."))
            ]);
        }
        function update() {
            return Promise
                .all([
                    udx.smsStatus(), udx.getStatus(),
                    pending ? udx.smsReceipt(pending) : Promise.resolve(null),
                    udx.smsHistoryStatus()
                ])
                .then(function(all) {
                    var inbox = udx.state(all[0]), op = udx.state(all[1].cellular_operation),
                        cell = udx.state(all[1].cellular), receipt = all[2];
                    current = cell;
                    operationBusy = !cell.available || cell.state === 'uncertain' ||
                                    [ 'queued', 'running' ].indexOf(op.state) >= 0;
                    if (receipt) {
                        var progress = receipt.submission_progress || {},
                            parts = progress.confirmed_parts || [];
                        dom.content(result,
                                    _("Request ") + pending + ': ' +
                                        (receipt.available
                                             ? udx.operationText(receipt)
                                             : receipt.error || _("Send result is not available")) +
                                        (parts.length
                                             ? _("; confirmed ") + parts.length + _(" parts")
                                             : '') +
                                        _(". Check the send receipt for updates."));
                        if (receipt.available &&
                            [ 'complete', 'failed' ].indexOf(receipt.state) >= 0)
                            remember(null);
                    }
                    refreshButtons();
                    renderHistory(all[3]);
                    var last = udx.state(all[1].cellular_operation);
                    if (last.operation &&
                        /^sms_(history|settings|mark_read|delete_history)$/.test(last.operation))
                        dom.content(historyResult, udx.operationText(last));
                    dom.content(messages, [
                        E('h3', {}, _("SIM messages")),
                        E('p', {},
                          inbox.available
                              ? _("Reading the SIM inbox updates unread flags.")
                              : _("The current SMS cache is not available. Refresh the inbox."))
                    ].concat((inbox.messages || []).map(function(m) {
                        var label = m.concat ? _("Parts: ") + m.concat.part + '/' + m.concat.total +
                                                   _(", reference ") + m.concat.reference
                                             : '';
                        return E('section', {class : 'cbi-section'}, [
                            E('strong', {}, m.peer || _("Number is not available")),
                            E('p', {},
                              [ m.timestamp || _("Time is not available"), label ]
                                  .filter(Boolean)
                                  .join(' / ')),
                            E('p', {style : 'white-space:pre-wrap;overflow-wrap:anywhere'},
                              m.text === null || m.text === undefined
                                  ? (m.decode_error || _("Message text is not available"))
                                  : m.text),
                            E('button', {
                                class : 'cbi-button cbi-button-negative',
                                disabled : (operationBusy) || null,
                                click : function() {
                                    udx.confirm(
                                        _("Delete message"),
                                        _("Delete this selected message from the SIM?"),
                                        function() {
                                            udx.submit(
                                                'sms_delete',
                                                {index : m.index, pdu_sha256 : m.pdu_sha256});
                                        });
                                }
                            },
                              _("Delete"))
                        ]);
                    })));
                })
                .catch(function(e) {
                    dom.content(result, _("Device status is not available: ") + e.message);
                });
        }
        send = E('button', {
            class : 'cbi-button cbi-button-action',
            click : function() {
                if (pending || operationBusy)
                    return;
                var n = number.value.trim(), text = body.value;
                if (!/^\+?[0-9]{3,15}$/.test(n) || !text || text.length > 402 ||
                    /[\uD800-\uDFFF]/.test(text)) {
                    udx.notice({
                        error : _(
                            "Enter a 3–15 digit number and up to 402 BMP Unicode characters. Emoji are not supported.")
                    });
                    return;
                }
                udx.confirm(
                    _("Send SMS"),
                    _("Recipient: ") + n +
                        _(". Wait for modem submission confirmation after sending. Delivery is confirmed separately."),
                    function() {
                        var bytes = new Uint8Array(16);
                        crypto.getRandomValues(bytes);
                        var id = Array.from(bytes)
                                     .map(function(x) {
                                         return x.toString(16).padStart(2, '0');
                                     })
                                     .join('');
                        remember(id);
                        refreshButtons();
                        dom.content(result, _("Submitting SMS, request ") + id);
                        udx.cellular('sms_send', {number : n, text : text, request_id : id})
                            .then(function(r) {
                                udx.notice(r);
                                if (r.accepted === false && !r.uncertain) {
                                    remember(null);
                                    refreshButtons();
                                } else if (r.state === 'complete' || r.state === 'failed') {
                                    dom.content(result, udx.operationText(r));
                                    remember(null);
                                    refreshButtons();
                                }
                            })
                            .catch(function(e) {
                                dom.content(
                                    result,
                                    _("Submission result is uncertain. Check the send receipt.") +
                                        e.message);
                            });
                    });
            }
        },
                 _("Send"));
        refresh = E('button', {
            class : 'cbi-button',
            click : function() {
                refresh.disabled = true;
                udx.submit('sms_refresh', {});
            }
        },
                    _("Refresh inbox"));
        var clear = E('button', {
            class : 'cbi-button',
            click : function() {
                if (!pending)
                    return;
                udx.confirm(
                    _("Stop waiting"),
                    _("Sending continues on the device. Check its receipt before sending another message."),
                    function() {
                        remember(null);
                        refreshButtons();
                        dom.content(
                            result,
                            _("Stopped waiting on this page. The original send result remains available in the device receipt."));
                    });
            }
        },
                      _("Stop waiting on this page"));
        search.addEventListener('input', function() {
            if (lastHistory)
                renderHistory(lastHistory);
        });
        refreshButtons();
        poll.add(update, 3);
        update();
        return E(
            'div', {}, [
                udx.stylesheet(), E('h2', {}, _("SMS management")),
                E('div', {class : 'cbi-value'},
                  [
                      E('span', {class : 'cbi-value-title'}, _("Search messages")),
                      E('div', {class : 'cbi-value-field'}, search)
                  ]),
                udx.tabs([
                    {
                        title : _("Inbox"),
                        content : E('div', {},
                                    [
                                        refreshHistory, historyBox,
                                        E('details', {},
                                          [
                                              E('summary', {}, _("Messages stored on the SIM")),
                                              refresh, messages
                                          ])
                                    ])
                    },
                    {title : _("Outbox"), content : sentBox}, {
                        title : _("Send SMS"),
                        content :
                            udx
                                .section(_("New message"),
                                         [
                                             E('div', {class : 'cbi-value'},
                                               [
                                                   E('label', {class : 'cbi-value-title'},
                                                     _("Recipient number")),
                                                   E('div', {class : 'cbi-value-field'}, number)
                                               ]),
                                             E('div', {class : 'cbi-value'},
                                               [
                                                   E('label', {class : 'cbi-value-title'},
                                                     _("Message text")),
                                                   E('div', {class : 'cbi-value-field'}, body)
                                               ]),
                                             E('div', {class : 'udx-actions'}, [ send, clear ]),
                                             result
                                         ])
                    },
                    {
                        title : _("SMS settings"),
                        content :
                            udx
                                .section(_("History and reception"),
                                         [
                                             E('div', {class : 'cbi-value'},
                                               [
                                                   E('label', {class : 'cbi-value-title'},
                                                     _("Inbox limit")),
                                                   E('div', {class : 'cbi-value-field'}, maxInbox)
                                               ]),
                                             E('div', {class : 'cbi-value'},
                                               [
                                                   E('label', {class : 'cbi-value-title'},
                                                     _("Outbox limit")),
                                                   E('div', {class : 'cbi-value-field'}, maxSent)
                                               ]),
                                             E('label', {},
                                               [ direct, _(" Receive directly on this device") ]),
                                             E('p', {},
                                               _("Direct reception uses modem notifications and stores messages on the device. When disabled, messages are read from SIM storage.")),
                                             saveSettings, historyResult
                                         ])
                    }
                ])
            ]);
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});

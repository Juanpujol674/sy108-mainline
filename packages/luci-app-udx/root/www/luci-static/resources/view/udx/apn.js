'use strict';
'require view';
'require udx';
'require poll';
'require dom';
return view.extend({
    render : function() {
        var cache = {}, current = {}, busy = true, editId = 0, signature = '', initialized = false,
            buttons = [], summary = E('div'), templates = E('div'), operation = E('div');
        var mode = E('select', {name : 'mode'},
                     [
                         E('option', {value : 'auto'}, _("Automatic; the network selects the APN")),
                         E('option', {value : 'manual'}, _("Select a profile manually"))
                     ]),
            selected = E('select', {name : 'template_id'}),
            name = E('input',
                     {name : 'name', class : 'cbi-input-text', placeholder : _("Profile name")}),
            apn = E('input', {name : 'apn', class : 'cbi-input-text', placeholder : 'APN'}),
            username = E('input', {
                name : 'username',
                class : 'cbi-input-text',
                placeholder : _("Authentication username"),
                autocomplete : 'off'
            }),
            password = E('input', {
                name : 'password',
                type : 'password',
                class : 'cbi-input-text',
                placeholder : _("Leave blank to keep the current password"),
                autocomplete : 'new-password'
            }),
            clear = E('input', {type : 'checkbox', name : 'clear_password'}),
            pdp = E('select', {name : 'pdp_type'}, [ 'IPV4V6', 'IP', 'IPV6' ].map(function(v) {
                return E('option', {value : v}, v);
            })),
            auth = E('select', {name : 'auth_method'}, [ 'none', 'pap', 'chap' ].map(function(v) {
                return E('option', {value : v},
                         v === 'none' ? _("No authentication") : v.toUpperCase());
            }));
        function allowed(op) {
            return !busy && udx.supports(current, op);
        }
        function submit(op, args, title) {
            if (!allowed(op))
                return;
            udx.confirm(
                title,
                _("Apply the APN configuration. The data connection may be interrupted. Check the device readback for the result."),
                function() {
                    busy = true;
                    buttons.forEach(function(b) {
                        b.node.disabled = true;
                    });
                    udx.submit(op, args);
                });
        }
        function action(title, op, collect) {
            var node = E('button', {
                class : 'cbi-button cbi-button-action',
                disabled : true,
                click : function() {
                    try {
                        submit(op, collect(), title);
                    } catch (e) {
                        udx.notice({error : e.message});
                    }
                }
            },
                         title);
            buttons.push({node : node, op : op});
            return node;
        }
        function edit(row) {
            editId = row ? row.id : 0;
            name.value = row ? row.name : '';
            apn.value = row ? row.apn : '';
            username.value = row ? row.username : '';
            pdp.value = row ? row.pdp_type : 'IPV4V6';
            auth.value = row ? row.auth_method : 'none';
            password.value = '';
            clear.checked = false;
        }
        var apply = action(_("Apply APN mode"), 'apn_select',
                           function() {
                               var id = mode.value === 'auto' ? 0 : Number(selected.value);
                               if (mode.value === 'manual' && (!Number.isInteger(id) || id < 1))
                                   throw new Error(_("Select a profile"));
                               return {mode : mode.value, template_id : id};
                           }),
            save = action(_("Save profile"), 'apn_template_save',
                          function() {
                              return {
                                  id : editId,
                                  name : name.value.trim(),
                                  apn : apn.value.trim(),
                                  pdp_type : pdp.value,
                                  username : username.value,
                                  password : clear.checked ? ''
                                                           : password.value || (editId ? null : ''),
                                  auth_method : auth.value
                              };
                          }),
            refresh = E('button', {
                class : 'cbi-button',
                disabled : true,
                click : function() {
                    if (allowed('apn_templates'))
                        udx.submit('apn_templates', {});
                }
            },
                        _("Read APN profiles"));
        function update() {
            return Promise.all([ udx.getStatus(), udx.apnStatus() ])
                .then(function(rows) {
                    var d = rows[0], op = udx.state(d.cellular_operation);
                    current = udx.state(d.cellular);
                    cache = udx.state(rows[1]);
                    busy = !current.available || current.state === 'uncertain' ||
                           [ 'queued', 'running' ].indexOf(op.state) >= 0;
                    buttons.forEach(function(b) {
                        b.node.disabled = !allowed(b.op);
                    });
                    refresh.disabled = !allowed('apn_templates');
                    var list = cache.templates || [], next = JSON.stringify(list.map(function(t) {
                        return [ t.id, t.name, t.apn ];
                    }));
                    if (next !== signature) {
                        var old = selected.value;
                        while (selected.firstChild)
                            selected.removeChild(selected.firstChild);
                        list.forEach(function(t) {
                            selected.appendChild(
                                E('option', {value : String(t.id)}, t.name + ' / ' + t.apn));
                        });
                        selected.value = list.some(function(t) {
                            return String(t.id) === old;
                        })
                                             ? old
                                             : String(cache.selected_template_id || '');
                        signature = next;
                    }
                    if (cache.available && !initialized) {
                        mode.value = cache.mode;
                        selected.value = String(cache.selected_template_id || '');
                        initialized = true;
                    }
                    var profile = cache.profile || {}, credentials = cache.auth || profile;
                    dom.content(summary,cache.available?[
    udx.table(_("Current APN configuration"),[[_("Mode"),cache.mode==='auto'?_("Automatic"):cache.mode==='manual'?_("Manual"):null],[_("Selected profile"),cache.mode==='auto'?_('Not used in automatic mode'):cache.selected_template_id],['APN',cache.mode==='auto'?_("Selected by the network"):profile.apn],[_("PDP type"),profile.pdp_type],[_("Authentication method"),credentials.auth_method==='none'?_('No authentication'):credentials.auth_method],[_("Username"),credentials.username||_('None')],[_("Password is set"),credentials.password_set],[_("Cache status"),_('Retrieved')]]),
    udx.telemetryNote(current,'apn_templates',d.sample_uptime)
   ]:E('p',{class:rows[1].stale?'alert-message warning':'udx-subtle'},rows[1].stale?_('APN configuration cache is stale.'):_('Reading APN configuration in the background.')));
                    dom.content(operation, udx.operationTable(d));
                    dom.content(
                        templates,
                        [ E('h3', {}, _("Local profiles")) ].concat(
                            list.length
                                ? list.map(function(t) {
                                      var protectedTemplate = cache.mode === 'manual' &&
                                                              cache.selected_template_id === t.id;
                                      return udx.table(t.name,[['APN',t.apn],['PDP',t.pdp_type],[_("Authentication"),t.auth_method==='none'?_('No authentication'):t.auth_method],[_("Username"),t.username||_('None')],[_("Password is set"),t.password_set],[_("Operation"),E('div',{},[E('button',{class:'cbi-button',click:function(){edit(t);}},_("Edit")),E('button',{class:'cbi-button cbi-button-negative',disabled:(protectedTemplate||!allowed('apn_template_delete'))||null,click:function(){submit('apn_template_delete',{id:t.id},_("Delete profile"));}},protectedTemplate?_("Currently in use; change the mode or profile first"):_("Delete"))])]]);
                                  })
                                : [ E('p', {},
                                      cache.available
                                          ? _("No profiles yet. Create a new profile.")
                                          : _('Reading APN profiles in the background.')) ]));
                })
                .catch(function(e) {
                    dom.content(summary, E('p', {class : 'alert-message warning'}, e.message));
                });
        }
        function field(title, node) {
            return E('div', {class : 'cbi-value'}, [
                E('label', {class : 'cbi-value-title'}, title),
                E('div', {class : 'cbi-value-field'}, node)
            ]);
        }
        poll.add(update, 5);
        update();
        return E('div', {}, [
            udx.stylesheet(), E('h2', {}, _("APN settings")), summary, refresh,
            E('section', {class : 'cbi-section'},
              [ E('h3', {}, _("Automatic or manual mode")), mode, selected, apply ]),
            E('section', {class : 'cbi-section'},
              [
                  E('h3', {}, _("Edit profile")),
                  E('button', {
                      class : 'cbi-button',
                      click : function() {
                          edit(null);
                      }
                  },
                    _("New profile")),
                  field(_("Profile name"), name), field('APN', apn), field(_("PDP type"), pdp),
                  field(_("Authentication method"), auth), field(_("Username"), username),
                  field(_("Password"), password),
                  E('label', {}, [ clear, _(" Clear the existing password") ]),
                  E('p', {},
                    _("When editing an existing profile, leave the password blank to retain it. Select \"Clear the existing password\" to remove it. Internet data uses CID 1.")),
                  save
              ]),
            templates, operation
        ]);
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});

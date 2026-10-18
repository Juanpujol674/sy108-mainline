/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *names[] = {"sa_nsa", "nsa_only", "sa_only"},
		  *labels[] = {"SA + NSA", "NSA only", "SA only"};
static const int readings[] = {66, 130, 258}, writes[] = {132, 260, 516};
static const uint32_t slots[] = {66051, 16777731};
static int mode_index(const char *mode)
{
	for (int i = 0; i < 3; i++)
		if (text_equal(mode, names[i]))
			return i;
	return -1;
}
int mode_validate(const char *operation, J *args)
{
	if (!strcmp(operation, "radio_options"))
		return 0;
	if (!strcmp(operation, "nr_capability"))
		return mode_index(str(args, "mode")) >= 0
			   ? 0
			   : errorf(CELL_INVALID, "Unknown NR capability");
	if (!strcmp(operation, "physical_slot")) {
		int64_t slot = json_object_get_int64(get(args, "slot"));
		return json_object_is_type(get(args, "slot"), json_type_int) &&
			       (slot == 0 || slot == 1)
			   ? 0
			   : errorf(CELL_INVALID, "Physical SIM slot must be 0 or 1");
	}
	return errorf(CELL_INVALID, "Unknown radio option");
}
static J *read_nr(struct controller *c)
{
	J *fields = controller_query(c, "AT+SPCAPABILITY=51,0", 10, "+SPCAPABILITY:");
	if (!fields)
		return NULL;
	int64_t raw;
	bool valid =
	    json_object_array_length(fields) == 3 &&
	    text_equal(json_object_get_string(json_object_array_get_idx(fields, 0)), "51") &&
	    text_equal(json_object_get_string(json_object_array_get_idx(fields, 1)), "0") &&
	    integer_text(json_object_get_string(json_object_array_get_idx(fields, 2)), 0,
			 UINT32_MAX, &raw);
	json_object_put(fields);
	if (!valid) {
		errorf(CELL_INVALID, "Invalid NR capability readback");
		return NULL;
	}
	const char *mode = NULL;
	J *choices = json_object_new_object();
	for (size_t i = 0; i < 3; i++) {
		if (raw == readings[i])
			mode = names[i];
		J *row = json_object_new_object();
		string(row, "label", labels[i]);
		integer(row, "readback", readings[i]);
		integer(row, "write_value", writes[i]);
		json_object_object_add(choices, names[i], row);
	}
	J *row = json_object_new_object();
	string(row, "mode", mode);
	integer(row, "raw", raw);
	json_object_object_add(row, "choices", choices);
	string(row, "source", "stock-app-demo");
	real(row, "uptime", mono());
	json_object_object_add(c->status, "nr_capability", json_object_get(row));
	return row;
}
static J *read_slot(struct controller *c)
{
	J *fields = controller_query(c, "AT+SPCONFIGSIMSLOT?", 5, "+SPCONFIGSIMSLOT:");
	if (!fields)
		return NULL;
	int64_t raw;
	bool valid = json_object_array_length(fields) == 1 &&
		     integer_text(json_object_get_string(json_object_array_get_idx(fields, 0)), 0,
				  UINT32_MAX, &raw);
	json_object_put(fields);
	if (!valid) {
		errorf(CELL_INVALID, "Invalid physical SIM slot readback");
		return NULL;
	}
	J *row = json_object_new_object(), *choices = json_object_new_array();
	json_object_array_add(choices, json_object_new_int(0));
	json_object_array_add(choices, json_object_new_int(1));
	integer(row, "slot", (raw >> 24) & 1);
	integer(row, "raw", raw);
	json_object_object_add(row, "choices", choices);
	integer(row, "logical_sim_count", 1);
	string(row, "source", "stock-single-sim-api");
	real(row, "uptime", mono());
	json_object_object_add(c->status, "physical_sim", json_object_get(row));
	return row;
}
static int transition(struct controller *c, const char *operation, const char *state, J *target,
		      const char *error)
{
	J *row = json_object_new_object();
	string(row, "operation", operation);
	string(row, "state", state);
	json_object_object_add(row, "target", json_object_get(target));
	real(row, "uptime", mono());
	if (error)
		string(row, "error", error);
	json_object_object_add(c->status, "radio_transition", row);
	return controller_publish(c);
}
static int radio(struct controller *c)
{
	J *row = controller_query(c, "AT+CFUN?", 5, "+CFUN:");
	if (!row)
		return -1;
	int64_t state;
	bool valid =
	    json_object_array_length(row) == 1 &&
	    integer_text(json_object_get_string(json_object_array_get_idx(row, 0)), 0, 1, &state);
	json_object_put(row);
	if (!valid)
		return errorf(CELL_INVALID, "Unsupported radio state for capability change");
	integer(c->status, "cfun", state);
	return (int)state;
}
static int command(struct controller *c, const char *text, double timeout)
{
	char *raw = controller_at(c, text, timeout);
	if (!raw)
		return -1;
	free(raw);
	return 0;
}
static int invalidate_sim(struct controller *c)
{
	c->telemetry_due[0] = 0;
	c->telemetry_due[2] = 0;
	c->telemetry_due[3] = 0;
	const char *names[] = {"iccid",	       "imsi",	  "operator", "modem_info_uptime",
			       "cells_uptime", "attached"};
	for (size_t i = 0; i < 6; i++)
		json_object_object_add(c->status, names[i], NULL);
	J *sim = json_object_new_object();
	string(sim, "state", "REFRESHING");
	json_object_object_add(c->status, "sim", sim);
	const char *regs[] = {"registration", "nr_registration"};
	for (size_t i = 0; i < 2; i++) {
		J *r = json_object_new_object();
		boolean(r, "registered", false);
		json_object_object_add(c->status, regs[i], r);
	}
	json_object_object_add(c->status, "cells", json_object_new_array());
	json_object_object_add(c->status, "cell_query_errors", json_object_new_object());
	json_object_object_del(c->status, "sim_error");
	json_object_object_del(c->status, "band_limits");
	J *errors = get(c->status, "info_errors");
	if (errors) {
		json_object_object_del(errors, "iccid");
		json_object_object_del(errors, "imsi");
	}
	return controller_publish(c);
}
static int refresh_sim(struct controller *c)
{
	char *state = controller_sim(c);
	if (!state)
		return -1;
	bool ready = !strcmp(state, "READY");
	J *row = json_object_new_object();
	string(row, "state", state);
	free(state);
	json_object_object_add(c->status, "sim", row);
	if (ready)
		json_object_object_del(c->status, "sim_error");
	if (controller_publish(c) < 0)
		return -1;
	return ready ? 1 : 0;
}
static J *restore(struct controller *c, int rf, bool wanted)
{
	int actual = radio(c);
	if (actual < 0)
		return NULL;
	if (rf && !actual) {
		if (controller_radio_on(c) < 0)
			return NULL;
	} else if (!rf && actual) {
		if (command(c, "AT+SFUN=5", 50) < 0)
			return NULL;
		actual = radio(c);
		if (actual < 0)
			return NULL;
		if (actual) {
			errorf(CELL_MODEM, "Radio-off restoration readback mismatch");
			return NULL;
		}
	}
	c->want_data = wanted && rf;
	if (c->want_data)
		return controller_connect(c, 10);
	J *out = json_object_new_object();
	boolean(out, "connected", false);
	return out;
}
J *mode_execute(struct controller *c, const char *operation, J *args)
{
	if (transport_admit(&c->transport) < 0)
		return NULL;
	if (!strcmp(operation, "radio_options")) {
		for (size_t i = 0; i < 2; i++) {
			J *row = i ? read_slot(c) : read_nr(c);
			if (!row) {
				if (cell_error_kind != CELL_MODEM &&
				    cell_error_kind != CELL_INVALID)
					return NULL;
				row = json_object_new_object();
				json_object_object_add(row, "choices",
						       i ? json_object_new_array()
							 : json_object_new_object());
				string(row, "error", cell_error);
				real(row, "uptime", mono());
				json_object_object_add(c->status,
						       i ? "physical_sim" : "nr_capability",
						       json_object_get(row));
			}
			json_object_put(row);
		}
		if (!get(c->status, "radio_transition")) {
			J *r = json_object_new_object();
			string(r, "state", "idle");
			json_object_object_add(c->status, "radio_transition", r);
		}
		if (controller_publish(c) < 0)
			return NULL;
		J *out = json_object_new_object();
		json_object_object_add(out, "nr_capability",
				       json_object_get(get(c->status, "nr_capability")));
		json_object_object_add(out, "physical_sim",
				       json_object_get(get(c->status, "physical_sim")));
		return out;
	}
	bool nr = !strcmp(operation, "nr_capability");
	J *before = nr ? read_nr(c) : read_slot(c);
	if (!before)
		return NULL;
	J *target = get(args, nr ? "mode" : "slot");
	if (json_object_equal(get(before, nr ? "mode" : "slot"), target)) {
		if (transition(c, operation, "complete", target, NULL) < 0) {
			json_object_put(before);
			return NULL;
		}
		J *result = json_object_new_object();
		boolean(result, "unchanged", true);
		boolean(result, "readback_verified", true);
		json_object_object_add(result, "before", json_object_get(before));
		json_object_object_add(result, "after", before);
		return result;
	}
	int rf = radio(c);
	if (rf < 0) {
		json_object_put(before);
		return NULL;
	}
	bool wanted = c->want_data;
	c->want_data = false;
	J *after = NULL, *result = NULL;
	if (transition(c, operation, "switching", target, NULL) < 0)
		goto fail;
	J *disconnected = controller_disconnect(c, false);
	if (!disconnected)
		goto fail;
	json_object_put(disconnected);
	char wire[80];
	if (nr) {
		if (rf) {
			if (command(c, "AT+SFUN=5", 50) < 0)
				goto fail;
			int actual = radio(c);
			if (actual < 0)
				goto fail;
			if (actual) {
				errorf(CELL_MODEM,
				       "Radio did not go offline before NR capability change");
				goto fail;
			}
		}
		int index = mode_index(str(args, "mode"));
		snprintf(wire, sizeof(wire), "AT+SPCAPABILITY=51,1,%d", writes[index]);
		if (command(c, wire, 10) < 0)
			goto fail;
		after = read_nr(c);
		if (!after)
			goto fail;
		if (json_object_get_int(get(after, "raw")) != readings[index]) {
			errorf(CELL_MODEM,
			       "NR capability readback mismatch; setting not confirmed");
			goto fail;
		}
		boolean(after, "write_readback_verified", true);
	} else {
		if (invalidate_sim(c) < 0)
			goto fail;
		c->initialized = false;
		c->ims_initialized = false;
		if (command(c, "AT+SFUN=3", 5) < 0 || refresh_sim(c) < 0)
			goto fail;
		delay_ms(2000);
		int slot = json_object_get_int(target);
		snprintf(wire, sizeof(wire), "AT+SPCONFIGSIMSLOT=%u", slots[slot]);
		if (command(c, wire, 5) < 0 || command(c, "AT+SFUN=2", 5) < 0)
			goto fail;
		int ready = refresh_sim(c);
		if (ready < 0)
			goto fail;
		after = read_slot(c);
		if (!after)
			goto fail;
		if (json_object_get_int(get(after, "slot")) != slot) {
			errorf(CELL_MODEM,
			       "Physical SIM mapping readback mismatch; setting not confirmed");
			goto fail;
		}
		boolean(after, "write_readback_verified", true);
		if (ready && info_sim_identity(c) < 0)
			goto fail;
	}
	result = restore(c, rf, wanted);
	if (!result)
		goto fail;
	if (transition(c, operation, "complete", target, NULL) < 0)
		goto fail;
	boolean(result, "readback_verified", true);
	json_object_object_add(result, "before", before);
	json_object_object_add(result, "after", json_object_get(after));
	json_object_object_add(result, nr ? "nr_capability" : "physical_sim", after);
	return result;
fail:;
	char saved[512];
	snprintf(saved, sizeof(saved), "%s", cell_error);
	enum cell_error_kind kind = cell_error_kind;
	c->want_data = c->transport.poisoned ? false : wanted;
	transition(c, operation, c->transport.poisoned ? "uncertain" : "failed", target, saved);
	json_object_put(before);
	json_object_put(after);
	json_object_put(result);
	errorf(kind, "%s", saved);
	return NULL;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int debug_validate(J *args)
{
	const char *command = str(args, "command");
	int seconds = json_object_get_int(get(args, "timeout"));
	if (!command || strncmp(command, "AT", 2) || strlen(command) > 256 ||
	    !json_object_is_type(get(args, "timeout"), json_type_int) || seconds < 1 ||
	    seconds > 120)
		return errorf(CELL_INVALID, "Use one AT command and a timeout of 1 to 120 seconds");
	char upper[257];
	size_t used = 0;
	bool quoted = false;
	for (size_t i = 0; command[i]; i++) {
		unsigned char ch = command[i];
		if (ch < 32 || ch > 126 || ch == ';')
			return errorf(CELL_INVALID,
				      "AT debug accepts one printable command without separators");
		if (ch == '"')
			quoted = !quoted;
		if (ch != ' ' || quoted)
			upper[used++] = (char)toupper(ch);
	}
	upper[used] = 0;
	const char *blocked[] = {
	    "AT+SPIMEI=", "AT+SPNV", "AT+CMGS=", "AT+CMGW=", "AT+CGDATA=", "ATD",
	    "ATO",	  "ATZ",     "AT&F",	 "ATQ",	     "ATV",	   "ATE"};
	for (size_t i = 0; i < sizeof(blocked) / sizeof(*blocked); i++)
		if (!strncmp(upper, blocked[i], strlen(blocked[i])))
			return errorf(CELL_INVALID, "This command requires a dedicated operation; "
						    "identity and AT framing changes are excluded");
	return 0;
}
J *debug_execute(struct controller *c, J *args)
{
	if (debug_validate(args) < 0 || transport_admit(&c->transport) < 0)
		return NULL;
	const char *command = str(args, "command");
	double started = mono();
	char *partial = NULL;
	char *raw = transport_debug_command(&c->transport, command,
					    json_object_get_int(get(args, "timeout")), &partial);
	J *out = json_object_new_object();
	string(out, "command", command);
	string(out, "response", raw ? raw : partial ? partial : "");
	boolean(out, "success", raw != NULL);
	boolean(out, "uncertain", c->transport.poisoned);
	string(out, "error", raw ? NULL : cell_error);
	real(out, "elapsed_ms", (mono() - started) * 1000);
	string(out, "boot_id", c->transport.boot);
	boolean(out, "replayed", false);
	free(raw);
	free(partial);
	return out;
}

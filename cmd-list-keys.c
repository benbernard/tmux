/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <string.h>

#include "tmux.h"

/*
 * List key bindings.
 */

enum cmd_retval	 cmd_list_keys_exec(struct cmd *, struct cmd_q *);

enum cmd_retval	 cmd_list_keys_table(struct cmd *, struct cmd_q *);
enum cmd_retval	 cmd_list_keys_commands(struct cmd *, struct cmd_q *);

const struct cmd_entry cmd_list_keys_entry = {
	"list-keys", "lsk",
	"t:", 0, 0,
	"[-t key-table]",
	0,
	cmd_list_keys_exec
};

const struct cmd_entry cmd_list_commands_entry = {
	"list-commands", "lscm",
	"", 0, 0,
	"",
	0,
	cmd_list_keys_exec
};

enum cmd_retval
cmd_list_keys_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args			*args = self->args;
	struct key_table		*table;
	struct key_binding		*bd;
	const char			*key;
	char				 tmp[BUFSIZ];
	size_t				 used;
	int				 hasrepeat, maxtablewidth, tablewidth, maxkeywidth, keywidth;

	if (self->entry == &cmd_list_commands_entry)
		return (cmd_list_keys_commands(self, cmdq));

	if (args_has(args, 't'))
		return (cmd_list_keys_table(self, cmdq));

	hasrepeat = 0;
	maxtablewidth = 0;
	maxkeywidth = 0;

	RB_FOREACH(table, key_tables, &key_tables) {
		RB_FOREACH(bd, key_bindings, &(table->key_bindings)) {
			key = key_string_lookup_key(bd->key);
			if (key == NULL)
				continue;

			if (bd->can_repeat)
				hasrepeat = 1;

			tablewidth = strlen(table->name);
			if (tablewidth > maxtablewidth)
				maxtablewidth = tablewidth;

			keywidth = strlen(key);
			if (keywidth > maxkeywidth)
				maxkeywidth = keywidth;
		}
	}

	RB_FOREACH(table, key_tables, &key_tables) {
		RB_FOREACH(bd, key_bindings, &(table->key_bindings)) {
			key = key_string_lookup_key(bd->key);
			if (key == NULL)
				continue;

			used = xsnprintf(tmp, sizeof tmp, "%s-T %-*s %-*s ",
				(hasrepeat ? (bd->can_repeat ? "-r " : "   ") : ""),
				(int) maxtablewidth, table->name,
				(int) maxkeywidth, key);
			if (used >= sizeof tmp)
				continue;

			cmd_list_print(bd->cmdlist, tmp + used, (sizeof tmp) - used);
			cmdq_print(cmdq, "bind-key %s", tmp);
		}
	}

	return (CMD_RETURN_NORMAL);
}

enum cmd_retval
cmd_list_keys_table(struct cmd *self, struct cmd_q *cmdq)
{
	struct args			*args = self->args;
	const char			*tablename;
	const struct mode_key_table	*mtab;
	struct mode_key_binding		*mbind;
	const char			*key, *cmdstr, *mode;
	int			 	 width, keywidth, any_mode;

	tablename = args_get(args, 't');
	if ((mtab = mode_key_findtable(tablename)) == NULL) {
		cmdq_error(cmdq, "unknown key table: %s", tablename);
		return (CMD_RETURN_ERROR);
	}

	width = 0;
	any_mode = 0;
	RB_FOREACH(mbind, mode_key_tree, mtab->tree) {
		key = key_string_lookup_key(mbind->key);
		if (key == NULL)
			continue;

		if (mbind->mode != 0)
			any_mode = 1;

		keywidth = strlen(key);
		if (keywidth > width)
			width = keywidth;
	}

	RB_FOREACH(mbind, mode_key_tree, mtab->tree) {
		key = key_string_lookup_key(mbind->key);
		if (key == NULL)
			continue;

		mode = "";
		if (mbind->mode != 0)
			mode = "c";
		cmdstr = mode_key_tostring(mtab->cmdstr, mbind->cmd);
		if (cmdstr != NULL) {
			cmdq_print(cmdq, "bind-key -%st %s%s %*s %s%s%s%s",
			    mode, any_mode && *mode == '\0' ? " " : "",
			    mtab->name, (int) width, key, cmdstr,
			    mbind->arg != NULL ? " \"" : "",
			    mbind->arg != NULL ? mbind->arg : "",
			    mbind->arg != NULL ? "\"": "");
		}
	}

	return (CMD_RETURN_NORMAL);
}

enum cmd_retval
cmd_list_keys_commands(unused struct cmd *self, struct cmd_q *cmdq)
{
	const struct cmd_entry	**entryp;
	const struct cmd_entry	 *entry;

	for (entryp = cmd_table; *entryp != NULL; entryp++) {
		entry = *entryp;
		if (entry->alias == NULL) {
			cmdq_print(cmdq, "%s %s", entry->name, entry->usage);
			continue;
		}
		cmdq_print(cmdq, "%s (%s) %s", entry->name, entry->alias,
		    entry->usage);
	}

	return (CMD_RETURN_NORMAL);
}

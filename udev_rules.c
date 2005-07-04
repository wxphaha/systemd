/*
 * udev_rules.c
 *
 * Userspace devfs
 *
 * Copyright (C) 2003 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (C) 2003-2005 Kay Sievers <kay.sievers@vrfy.org>
 *
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation version 2 of the License.
 * 
 *	This program is distributed in the hope that it will be useful, but
 *	WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *	General Public License for more details.
 * 
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, write to the Free Software Foundation, Inc.,
 *	675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include "libsysfs/sysfs/libsysfs.h"
#include "list.h"
#include "udev_libc_wrapper.h"
#include "udev.h"
#include "udev_utils.h"
#include "udev_version.h"
#include "logging.h"
#include "udev_rules.h"
#include "udev_db.h"


/* compare string with pattern (supports * ? [0-9] [!A-Z]) */
static int strcmp_pattern(const char *p, const char *s)
{
	if (s[0] == '\0') {
		while (p[0] == '*')
			p++;
		return (p[0] != '\0');
	}
	switch (p[0]) {
	case '[':
		{
			int not = 0;
			p++;
			if (p[0] == '!') {
				not = 1;
				p++;
			}
			while ((p[0] != '\0') && (p[0] != ']')) {
				int match = 0;
				if (p[1] == '-') {
					if ((s[0] >= p[0]) && (s[0] <= p[2]))
						match = 1;
					p += 3;
				} else {
					match = (p[0] == s[0]);
					p++;
				}
				if (match ^ not) {
					while ((p[0] != '\0') && (p[0] != ']'))
						p++;
					if (p[0] == ']')
						return strcmp_pattern(p+1, s+1);
				}
			}
		}
		break;
	case '*':
		if (strcmp_pattern(p, s+1))
			return strcmp_pattern(p+1, s);
		return 0;
	case '\0':
		if (s[0] == '\0') {
			return 0;
		}
		break;
	default:
		if ((p[0] == s[0]) || (p[0] == '?'))
			return strcmp_pattern(p+1, s+1);
		break;
	}
	return 1;
}

/* extract possible {attr} and move str behind it */
static char *get_format_attribute(char **str)
{
	char *pos;
	char *attr = NULL;

	if (*str[0] == '{') {
		pos = strchr(*str, '}');
		if (pos == NULL) {
			err("missing closing brace for format");
			return NULL;
		}
		pos[0] = '\0';
		attr = *str+1;
		*str = pos+1;
		dbg("attribute='%s', str='%s'", attr, *str);
	}
	return attr;
}

/* extract possible format length and move str behind it*/
static int get_format_len(char **str)
{
	int num;
	char *tail;

	if (isdigit(*str[0])) {
		num = (int) strtoul(*str, &tail, 10);
		if (num > 0) {
			*str = tail;
			dbg("format length=%i", num);
			return num;
		} else {
			err("format parsing error '%s'", *str);
		}
	}
	return -1;
}

static int get_key(char **line, char **key, char **value)
{
	char *linepos;
	char *temp;

	linepos = *line;
	if (!linepos)
		return -1;

	if (strchr(linepos, '\\')) {
		dbg("escaped characters are not supported, skip");
		return -1;
	}

	/* skip whitespace */
	while (isspace(linepos[0]))
		linepos++;

	/* get the key */
	*key = linepos;
	while (1) {
		linepos++;
		if (linepos[0] == '\0')
			return -1;
		if (isspace(linepos[0]))
			break;
		if (linepos[0] == '=')
			break;
	}

	/* terminate key */
	linepos[0] = '\0';
	linepos++;

	/* skip whitespace */
	while (isspace(linepos[0]))
		linepos++;

	/* get the value*/
	if (linepos[0] == '"') {
		linepos++;
		temp = strchr(linepos, '"');
		if (!temp) {
			dbg("missing closing quote");
			return -1;
		}
		dbg("value is quoted");
		temp[0] = '\0';
	} else if (linepos[0] == '\'') {
		linepos++;
		temp = strchr(linepos, '\'');
		if (!temp) {
			dbg("missing closing quote");
			return -1;
		}
		dbg("value is quoted");
		temp[0] = '\0';
	} else if (linepos[0] == '\0') {
		dbg("value is empty");
	} else {
		temp = linepos;
		while (temp[0] && !isspace(temp[0]))
			temp++;
		temp[0] = '\0';
	}
	*value = linepos;

	return 0;
}

static int import_keys_into_env(struct udevice *udev, const char *buf, size_t bufsize)
{
	char line[LINE_SIZE];
	const char *bufline;
	char *linepos;
	char *variable;
	char *value;
	size_t cur;
	size_t count;
	int lineno;

	/* loop through the whole buffer */
	lineno = 0;
	cur = 0;
	while (cur < bufsize) {
		count = buf_get_line(buf, bufsize, cur);
		bufline = &buf[cur];
		cur += count+1;
		lineno++;

		if (count >= sizeof(line)) {
			err("line too long, conf line skipped %s, line %d", udev_config_filename, lineno);
			continue;
		}

		/* eat the whitespace */
		while ((count > 0) && isspace(bufline[0])) {
			bufline++;
			count--;
		}
		if (count == 0)
			continue;

		/* see if this is a comment */
		if (bufline[0] == COMMENT_CHARACTER)
			continue;

		strlcpy(line, bufline, count+1);

		linepos = line;
		if (get_key(&linepos, &variable, &value) == 0) {
			dbg("import '%s=%s'", variable, value);
			name_list_key_add(&udev->env_list, variable, value);
			setenv(variable, value, 1);
		}
	}

	return 0;
}

static int import_file_into_env(struct udevice *udev, const char *filename)
{
	char *buf;
	size_t bufsize;

	if (file_map(filename, &buf, &bufsize) != 0) {
		err("can't open '%s'", filename);
		return -1;
	}
	import_keys_into_env(udev, buf, bufsize);
	file_unmap(buf, bufsize);

	return 0;
}

static int import_program_into_env(struct udevice *udev, const char *program)
{
	char result[1024];
	size_t reslen;

	if (execute_program(program, udev->subsystem, result, sizeof(result), &reslen) != 0)
		return -1;
	return import_keys_into_env(udev, result, reslen);
}

/* finds the lowest positive N such that <name>N isn't present in the udevdb
 * if <name> doesn't exist, 0 is returned, N otherwise
 */
static int find_free_number(struct udevice *udev, const char *name)
{
	char devpath[PATH_SIZE];
	char filename[PATH_SIZE];
	int num = 0;

	strlcpy(filename, name, sizeof(filename));
	while (1) {
		dbg("look for existing node '%s'", filename);
		if (udev_db_search_name(devpath, sizeof(devpath), filename) != 0) {
			dbg("free num=%d", num);
			return num;
		}

		num++;
		if (num > 1000) {
			info("find_free_number gone crazy (num=%d), aborted", num);
			return -1;
		}
		snprintf(filename, sizeof(filename), "%s%d", name, num);
		filename[sizeof(filename)-1] = '\0';
	}
}

static int find_sysfs_attribute(struct sysfs_class_device *class_dev, struct sysfs_device *sysfs_device,
				const char *name, char *value, size_t len)
{
	struct sysfs_attribute *tmpattr;

	dbg("look for device attribute '%s'", name);
	if (class_dev) {
		dbg("look for class attribute '%s/%s'", class_dev->path, name);
		tmpattr = sysfs_get_classdev_attr(class_dev, name);
		if (tmpattr)
			goto attr_found;
	}
	if (sysfs_device) {
		dbg("look for devices attribute '%s/%s'", sysfs_device->path, name);
		tmpattr = sysfs_get_device_attr(sysfs_device, name);
		if (tmpattr)
			goto attr_found;
	}
	return -1;

attr_found:
	strlcpy(value, tmpattr->value, len);
	remove_trailing_char(value, '\n');

	dbg("found attribute '%s'", tmpattr->path);
	return 0;
}

static void apply_format(struct udevice *udev, char *string, size_t maxsize,
			 struct sysfs_class_device *class_dev, struct sysfs_device *sysfs_device)
{
	char temp[PATH_SIZE];
	char temp2[PATH_SIZE];
	char *head, *tail, *pos, *cpos, *attr, *rest;
	int len;
	int i;
	unsigned int next_free_number;
	struct sysfs_class_device *class_dev_parent;
	enum subst_type {
		SUBST_UNKNOWN,
		SUBST_DEVPATH,
		SUBST_ID,
		SUBST_KERNEL_NUMBER,
		SUBST_KERNEL_NAME,
		SUBST_MAJOR,
		SUBST_MINOR,
		SUBST_RESULT,
		SUBST_SYSFS,
		SUBST_ENUM,
		SUBST_PARENT,
		SUBST_TEMP_NODE,
		SUBST_ROOT,
		SUBST_MODALIAS,
		SUBST_ENV,
	};
	static const struct subst_map {
		char *name;
		char fmt;
		enum subst_type type;
	} map[] = {
		{ .name = "devpath",		.fmt = 'p',	.type = SUBST_DEVPATH },
		{ .name = "id",			.fmt = 'b',	.type = SUBST_ID },
		{ .name = "number",		.fmt = 'n',	.type = SUBST_KERNEL_NUMBER },
		{ .name = "kernel",		.fmt = 'k',	.type = SUBST_KERNEL_NAME },
		{ .name = "major",		.fmt = 'M',	.type = SUBST_MAJOR },
		{ .name = "minor",		.fmt = 'm',	.type = SUBST_MINOR },
		{ .name = "result",		.fmt = 'c',	.type = SUBST_RESULT },
		{ .name = "sysfs",		.fmt = 's',	.type = SUBST_SYSFS },
		{ .name = "enum",		.fmt = 'e',	.type = SUBST_ENUM },
		{ .name = "parent",		.fmt = 'P',	.type = SUBST_PARENT },
		{ .name = "tempnode",		.fmt = 'N',	.type = SUBST_TEMP_NODE },
		{ .name = "root",		.fmt = 'r',	.type = SUBST_ROOT },
		{ .name = "modalias",		.fmt = 'A',	.type = SUBST_MODALIAS },
		{ .name = "env",		.fmt = 'E',	.type = SUBST_ENV },
		{}
	};
	enum subst_type type;
	const struct subst_map *subst;

	head = string;
	while (1) {
		len = -1;
		while (head[0] != '\0') {
			if (head[0] == '$') {
				/* substitute named variable */
				if (head[1] == '\0')
					break;
				if (head[1] == '$') {
					strlcpy(temp, head+2, sizeof(temp));
					strlcpy(head+1, temp, maxsize);
					head++;
					continue;
				}
				head[0] = '\0';
				for (subst = map; subst->name; subst++) {
					if (strncasecmp(&head[1], subst->name, strlen(subst->name)) == 0) {
						type = subst->type;
						tail = head + strlen(subst->name)+1;
						dbg("will substitute format name '%s'", subst->name);
						goto found;
					}
				}
			}
			else if (head[0] == '%') {
				/* substitute format char */
				if (head[1] == '\0')
					break;
				if (head[1] == '%') {
					strlcpy(temp, head+2, sizeof(temp));
					strlcpy(head+1, temp, maxsize);
					head++;
					continue;
				}
				head[0] = '\0';
				tail = head+1;
				len = get_format_len(&tail);
				for (subst = map; subst->name; subst++) {
					if (tail[0] == subst->fmt) {
						type = subst->type;
						tail++;
						dbg("will substitute format char '%c'", subst->fmt);
						goto found;
					}
				}
			}
			head++;
		}
		break;
found:
		attr = get_format_attribute(&tail);
		strlcpy(temp, tail, sizeof(temp));
		dbg("format=%i, string='%s', tail='%s', class_dev=%p, sysfs_dev=%p",
		    type ,string, tail, class_dev, sysfs_device);

		switch (type) {
		case SUBST_DEVPATH:
			strlcat(string, udev->devpath, maxsize);
			dbg("substitute devpath '%s'", udev->devpath);
			break;
		case SUBST_ID:
			strlcat(string, udev->bus_id, maxsize);
			dbg("substitute bus_id '%s'", udev->bus_id);
			break;
		case SUBST_KERNEL_NAME:
			strlcat(string, udev->kernel_name, maxsize);
			dbg("substitute kernel name '%s'", udev->kernel_name);
			break;
		case SUBST_KERNEL_NUMBER:
			strlcat(string, udev->kernel_number, maxsize);
			dbg("substitute kernel number '%s'", udev->kernel_number);
			break;
		case SUBST_MAJOR:
			sprintf(temp2, "%d", major(udev->devt));
			strlcat(string, temp2, maxsize);
			dbg("substitute major number '%s'", temp2);
			break;
		case SUBST_MINOR:
			sprintf(temp2, "%d", minor(udev->devt));
			strlcat(string, temp2, maxsize);
			dbg("substitute minor number '%s'", temp2);
			break;
		case SUBST_RESULT:
			if (udev->program_result[0] == '\0')
				break;
			/* get part part of the result string */
			i = 0;
			if (attr != NULL)
				i = strtoul(attr, &rest, 10);
			if (i > 0) {
				dbg("request part #%d of result string", i);
				cpos = udev->program_result;
				while (--i) {
					while (cpos[0] != '\0' && !isspace(cpos[0]))
						cpos++;
					while (isspace(cpos[0]))
						cpos++;
				}
				if (i > 0) {
					err("requested part of result string not found");
					break;
				}
				strlcpy(temp2, cpos, sizeof(temp2));
				/* %{2+}c copies the whole string from the second part on */
				if (rest[0] != '+') {
					cpos = strchr(temp2, ' ');
					if (cpos)
						cpos[0] = '\0';
				}
				strlcat(string, temp2, maxsize);
				dbg("substitute part of result string '%s'", temp2);
			} else {
				strlcat(string, udev->program_result, maxsize);
				dbg("substitute result string '%s'", udev->program_result);
			}
			break;
		case SUBST_SYSFS:
			if (attr == NULL) {
				dbg("missing attribute");
				break;
			}
			if (find_sysfs_attribute(class_dev, sysfs_device, attr, temp2, sizeof(temp2)) != 0) {
				struct sysfs_device *parent_device;

				dbg("sysfs attribute '%s' not found, walk up the physical devices", attr);
				parent_device = sysfs_get_device_parent(sysfs_device);
				while (parent_device) {
					dbg("looking at '%s'", parent_device->path);
					if (find_sysfs_attribute(NULL, parent_device, attr, temp2, sizeof(temp2)) == 0)
						break;
					parent_device = sysfs_get_device_parent(parent_device);
				}
				if (!parent_device)
					break;
			}
			/* strip trailing whitespace of sysfs value */
			i = strlen(temp2);
			while (i > 0 && isspace(temp2[i-1]))
				temp2[--i] = '\0';
			replace_untrusted_chars(temp2);
			strlcat(string, temp2, maxsize);
			dbg("substitute sysfs value '%s'", temp2);
			break;
		case SUBST_ENUM:
			next_free_number = find_free_number(udev, string);
			if (next_free_number > 0) {
				sprintf(temp2, "%d", next_free_number);
				strlcat(string, temp2, maxsize);
			}
			break;
		case SUBST_PARENT:
			if (!class_dev)
				break;
			class_dev_parent = sysfs_get_classdev_parent(class_dev);
			if (class_dev_parent != NULL) {
				struct udevice udev_parent;

				dbg("found parent '%s', get the node name", class_dev_parent->path);
				udev_init_device(&udev_parent, NULL, NULL, NULL);
				/* lookup the name in the udev_db with the DEVPATH of the parent */
				if (udev_db_get_device(&udev_parent, &class_dev_parent->path[strlen(sysfs_path)]) == 0) {
					strlcat(string, udev_parent.name, maxsize);
					dbg("substitute parent node name'%s'", udev_parent.name);
				} else
					dbg("parent not found in database");
				udev_cleanup_device(&udev_parent);
			}
			break;
		case SUBST_TEMP_NODE:
			if (udev->tmp_node[0] == '\0') {
				dbg("create temporary device node for callout");
				snprintf(udev->tmp_node, sizeof(udev->tmp_node), "%s/.tmp-%u-%u",
					 udev_root, major(udev->devt), minor(udev->devt));
				udev->tmp_node[sizeof(udev->tmp_node)-1] = '\0';
				udev_make_node(udev, udev->tmp_node, udev->devt, 0600, 0, 0);
			}
			strlcat(string, udev->tmp_node, maxsize);
			dbg("substitute temporary device node name '%s'", udev->tmp_node);
			break;
		case SUBST_ROOT:
			strlcat(string, udev_root, maxsize);
			dbg("substitute udev_root '%s'", udev_root);
			break;
		case SUBST_MODALIAS:
			if (find_sysfs_attribute(NULL, sysfs_device, "modalias", temp2, sizeof(temp2)) != 0)
				break;
			strlcat(string, temp2, maxsize);
			dbg("substitute MODALIAS '%s'", temp2);
			break;
		case SUBST_ENV:
			if (attr == NULL) {
				dbg("missing attribute");
				break;
			}
			pos = getenv(attr);
			if (pos == NULL)
				break;
			strlcat(string, pos, maxsize);
			dbg("substitute env '%s=%s'", attr, pos);
			break;
		default:
			err("unknown substitution type=%i", type);
			break;
		}
		/* possibly truncate to format-char specified length */
		if (len != -1) {
			head[len] = '\0';
			dbg("truncate to %i chars, subtitution string becomes '%s'", len, head);
		}
		strlcat(string, temp, maxsize);
	}
}

static int match_key(const char *key, const char *key_val, enum key_operation key_op, const char *val)
{
	int match;

	if (key_op == KEY_OP_UNSET)
		return 0;

	dbg("check for %s '%s' <-> '%s'", key, key_val, val);
	match = (strcmp_pattern(key_val, val) == 0);
	if (match && (key_op != KEY_OP_NOMATCH)) {
		dbg("%s key is matching (matching value)", key);
		return 0;
	}
	if (!match && (key_op == KEY_OP_NOMATCH)) {
		dbg("%s key is matching, (non matching value)", key);
		return 0;
	}

	dbg("%s key is not matching", key);
	return -1;
}

static int match_rule(struct udevice *udev, struct udev_rule *rule,
		      struct sysfs_class_device *class_dev, struct sysfs_device *sysfs_device)
{
	struct sysfs_device *parent_device = sysfs_device;

	if (match_key("ACTION", rule->action, rule->action_operation, udev->action))
		goto exit;

	if (match_key("KERNEL", rule->kernel_name, rule->kernel_operation, udev->kernel_name))
		goto exit;

	if (match_key("SUBSYSTEM", rule->subsystem, rule->subsystem_operation, udev->subsystem))
		goto exit;

	if (match_key("DEVPATH", rule->devpath, rule->devpath_operation, udev->devpath))
		goto exit;

	if (rule->modalias_operation != KEY_OP_UNSET) {
		char value[NAME_SIZE];

		if (find_sysfs_attribute(NULL, sysfs_device, "modalias", value, sizeof(value)) != 0) {
			dbg("MODALIAS value not found");
			goto exit;
		}
		if (match_key("MODALIAS", rule->modalias, rule->modalias_operation, value))
			goto exit;
	}

	if (rule->env_pair_count) {
		int i;

		for (i = 0; i < rule->env_pair_count; i++) {
			struct key_pair *pair;
			const char *value;

			pair = &rule->env_pair[i];
			value = getenv(pair->name);
			if (!value) {
				dbg("ENV{'%s'} is not found", pair->name);
				goto exit;
			}
			dbg("check %i ENV keys", rule->env_pair_count);
			if (match_key(pair->name, pair->value, pair->operation, value))
				goto exit;
		}
		dbg("all %i ENV keys matched", rule->env_pair_count);
	}

	/* walk up the chain of physical devices and find a match */
	while (1) {
		/* check for matching driver */
		if (rule->driver_operation != KEY_OP_UNSET) {
			if (parent_device == NULL) {
				dbg("device has no sysfs_device");
				goto exit;
			}
			if (match_key("DRIVER", rule->driver, rule->driver_operation, parent_device->driver_name))
				goto try_parent;
		}

		/* check for matching bus value */
		if (rule->bus_operation != KEY_OP_UNSET) {
			if (parent_device == NULL) {
				dbg("device has no sysfs_device");
				goto exit;
			}
			if (match_key("BUS", rule->bus, rule->bus_operation, parent_device->bus))
				goto try_parent;
		}

		/* check for matching bus id */
		if (rule->id_operation != KEY_OP_UNSET) {
			if (parent_device == NULL) {
				dbg("device has no sysfs_device");
				goto exit;
			}
			if (match_key("ID", rule->id, rule->id_operation, parent_device->bus_id))
				goto try_parent;
		}

		/* check for matching sysfs pairs */
		if (rule->sysfs_pair_count) {
			int i;

			for (i = 0; i < rule->sysfs_pair_count; i++) {
				struct key_pair *pair;
				char value[VALUE_SIZE];
				size_t len;

				pair = &rule->sysfs_pair[i];
				if (find_sysfs_attribute(class_dev, parent_device, pair->name, value, sizeof(value)) != 0)
					goto try_parent;

				/* strip trailing whitespace of value, if not asked to match for it */
				len = strlen(pair->value);
				if (len && !isspace(pair->value[len-1])) {
					len = strlen(value);
					while (len > 0 && isspace(value[len-1]))
						value[--len] = '\0';
					dbg("removed %zi trailing whitespace chars from '%s'", strlen(value)-len, value);
				}

				dbg("check %i SYSFS keys", rule->sysfs_pair_count);
				if (match_key(pair->name, pair->value, pair->operation, value))
					goto try_parent;
			}
			dbg("all %i SYSFS keys matched", rule->sysfs_pair_count);
		}

		/* found matching physical device  */
		break;
try_parent:
		dbg("try parent sysfs device");
		parent_device = sysfs_get_device_parent(parent_device);
		if (parent_device == NULL)
			goto exit;
		dbg("look at sysfs_device->path='%s'", parent_device->path);
		dbg("look at sysfs_device->bus_id='%s'", parent_device->bus_id);
	}

	/* import variables from file into environment */
	if (rule->import_operation != KEY_OP_UNSET) {
		char import[PATH_SIZE];
		int rc = -1;

		strlcpy(import, rule->import, sizeof(import));
		apply_format(udev, import, sizeof(import), class_dev, sysfs_device);
		dbg("check for IMPORT import='%s'", import);
		if (rule->import_exec) {
			dbg("run executable file import='%s'", import);
			rc = import_program_into_env(udev, import);
		} else {
			dbg("import file import='%s'", import);
			rc = import_file_into_env(udev, import);
		}
		if (rc) {
			dbg("IMPORT failed");
			if (rule->import_operation != KEY_OP_NOMATCH)
				goto exit;
		} else
			dbg("IMPORT '%s' imported", rule->import);
		dbg("IMPORT key is true");
	}

	/* execute external program */
	if (rule->program_operation != KEY_OP_UNSET) {
		char program[PATH_SIZE];
		char result[PATH_SIZE];

		strlcpy(program, rule->program, sizeof(program));
		apply_format(udev, program, sizeof(program), class_dev, sysfs_device);
		dbg("check for PROGRAM program='%s", program);
		if (execute_program(program, udev->subsystem, result, sizeof(result), NULL) != 0) {
			dbg("PROGRAM is not matching");
			if (rule->program_operation != KEY_OP_NOMATCH)
				goto exit;
		} else {
			dbg("PROGRAM matches");
			remove_trailing_char(result, '\n');
			replace_untrusted_chars(result);
			dbg("result is '%s'", result);
			strlcpy(udev->program_result, result, sizeof(udev->program_result));
			dbg("PROGRAM returned successful");
			if (rule->program_operation == KEY_OP_NOMATCH)
				goto exit;
		}
		dbg("PROGRAM key is true");
	}

	/* check for matching result of external program */
	if (match_key("RESULT", rule->result, rule->result_operation, udev->program_result))
		goto exit;

	/* rule matches */
	return 0;

exit:
	return -1;
}

int udev_rules_get_name(struct udevice *udev, struct sysfs_class_device *class_dev)
{
	struct sysfs_class_device *class_dev_parent;
	struct sysfs_device *sysfs_device = NULL;
	struct udev_rule *rule;

	dbg("class_dev->name='%s'", class_dev->name);

	/* Figure out where the "device"-symlink is at.  For char devices this will
	 * always be in the class_dev->path.  On block devices, only the main block
	 * device will have the device symlink in it's path. All partition devices
	 * need to look at the symlink in its parent directory.
	 */
	class_dev_parent = sysfs_get_classdev_parent(class_dev);
	if (class_dev_parent != NULL) {
		dbg("given class device has a parent, use this instead");
		sysfs_device = sysfs_get_classdev_device(class_dev_parent);
	} else {
		sysfs_device = sysfs_get_classdev_device(class_dev);
	}

	if (sysfs_device) {
		dbg("found devices device: path='%s', bus_id='%s', bus='%s'",
		    sysfs_device->path, sysfs_device->bus_id, sysfs_device->bus);
		strlcpy(udev->bus_id, sysfs_device->bus_id, sizeof(udev->bus_id));
	}

	dbg("udev->kernel_name='%s'", udev->kernel_name);

	/* look for a matching rule to apply */
	udev_rules_iter_init();
	while (1) {
		rule = udev_rules_iter_next();
		if (rule == NULL)
			break;

		if (udev->name_set && rule->name_operation != KEY_OP_UNSET) {
			dbg("node name already set, rule ignored");
			continue;
		}

		dbg("process rule");
		if (match_rule(udev, rule, class_dev, sysfs_device) == 0) {
			/* apply options */
			if (rule->ignore_device) {
				info("configured rule in '%s[%i]' applied, '%s' is ignored",
				     rule->config_file, rule->config_line, udev->kernel_name);
				udev->ignore_device = 1;
				return 0;
			}
			if (rule->ignore_remove) {
				udev->ignore_remove = 1;
				dbg("remove event should be ignored");
			}
			/* apply all_partitions option only at a main block device */
			if (rule->partitions && udev->type == DEV_BLOCK && udev->kernel_number[0] == '\0') {
				udev->partitions = rule->partitions;
				dbg("creation of partition nodes requested");
			}

			/* apply permissions */
			if (!udev->mode_final && rule->mode != 0000) {
				if (rule->mode_operation == KEY_OP_ASSIGN_FINAL)
					udev->mode_final = 1;
				udev->mode = rule->mode;
				dbg("applied mode=%#o to '%s'", udev->mode, udev->kernel_name);
			}
			if (!udev->owner_final && rule->owner[0] != '\0') {
				if (rule->owner_operation == KEY_OP_ASSIGN_FINAL)
					udev->owner_final = 1;
				strlcpy(udev->owner, rule->owner, sizeof(udev->owner));
				apply_format(udev, udev->owner, sizeof(udev->owner), class_dev, sysfs_device);
				dbg("applied owner='%s' to '%s'", udev->owner, udev->kernel_name);
			}
			if (!udev->group_final && rule->group[0] != '\0') {
				if (rule->group_operation == KEY_OP_ASSIGN_FINAL)
					udev->group_final = 1;
				strlcpy(udev->group, rule->group, sizeof(udev->group));
				apply_format(udev, udev->group, sizeof(udev->group), class_dev, sysfs_device);
				dbg("applied group='%s' to '%s'", udev->group, udev->kernel_name);
			}

			/* collect symlinks */
			if (!udev->symlink_final && rule->symlink_operation != KEY_OP_UNSET) {
				char temp[PATH_SIZE];
				char *pos, *next;

				if (rule->symlink_operation == KEY_OP_ASSIGN_FINAL)
					udev->symlink_final = 1;
				if (rule->symlink_operation == KEY_OP_ASSIGN || rule->symlink_operation == KEY_OP_ASSIGN_FINAL) {
					struct name_entry *name_loop;
					struct name_entry *temp_loop;

					info("reset symlink list");
					list_for_each_entry_safe(name_loop, temp_loop, &udev->symlink_list, node) {
						list_del(&name_loop->node);
						free(name_loop);
					}
				}
				if (rule->symlink[0] != '\0') {
					info("configured rule in '%s[%i]' applied, added symlink '%s'",
					     rule->config_file, rule->config_line, rule->symlink);
					strlcpy(temp, rule->symlink, sizeof(temp));
					apply_format(udev, temp, sizeof(temp), class_dev, sysfs_device);

					/* add multiple symlinks separated by spaces */
					pos = temp;
					next = strchr(temp, ' ');
					while (next) {
						next[0] = '\0';
						info("add symlink '%s'", pos);
						name_list_add(&udev->symlink_list, pos, 0);
						pos = &next[1];
						next = strchr(pos, ' ');
					}
					info("add symlink '%s'", pos);
					name_list_add(&udev->symlink_list, pos, 0);
				}
			}

			/* set name, later rules with name set will be ignored */
			if (rule->name_operation != KEY_OP_UNSET) {
				udev->name_set = 1;
				if (rule->name[0] == '\0') {
					info("configured rule in '%s[%i]' applied, node handling for '%s' supressed",
					     rule->config_file, rule->config_line, udev->kernel_name);
				} else {
					strlcpy(udev->name, rule->name, sizeof(udev->name));
					apply_format(udev, udev->name, sizeof(udev->name), class_dev, sysfs_device);
					strlcpy(udev->config_file, rule->config_file, sizeof(udev->config_file));
					udev->config_line = rule->config_line;

					info("configured rule in '%s:%i' applied, '%s' becomes '%s'",
					     rule->config_file, rule->config_line, udev->kernel_name, rule->name);
					if (udev->type != DEV_NET)
						dbg("name, '%s' is going to have owner='%s', group='%s', mode=%#o partitions=%i",
						    udev->name, udev->owner, udev->group, udev->mode, udev->partitions);
				}
			}

			if (!udev->run_final && rule->run_operation != KEY_OP_UNSET) {
				char program[PATH_SIZE];

				if (rule->run_operation == KEY_OP_ASSIGN_FINAL)
					udev->run_final = 1;
				if (rule->run_operation == KEY_OP_ASSIGN || rule->run_operation == KEY_OP_ASSIGN_FINAL) {
					struct name_entry *name_loop;
					struct name_entry *temp_loop;

					info("reset run list");
					list_for_each_entry_safe(name_loop, temp_loop, &udev->run_list, node) {
						list_del(&name_loop->node);
						free(name_loop);
					}
				}
				if (rule->run[0] != '\0') {
					strlcpy(program, rule->run, sizeof(program));
					apply_format(udev, program, sizeof(program), class_dev, sysfs_device);
					dbg("add run '%s'", program);
					name_list_add(&udev->run_list, program, 0);
				}
			}

			if (rule->last_rule) {
				dbg("last rule to be applied");
				break;
			}
		}
	}

	if (udev->name[0] == '\0') {
		strlcpy(udev->name, udev->kernel_name, sizeof(udev->name));
		info("no rule found, use kernel name '%s'", udev->name);
	}

	if (udev->tmp_node[0] != '\0') {
		dbg("removing temporary device node");
		unlink_secure(udev->tmp_node);
		udev->tmp_node[0] = '\0';
	}

	return 0;
}

int udev_rules_get_run(struct udevice *udev, struct sysfs_device *sysfs_device)
{
	struct udev_rule *rule;

	/* look for a matching rule to apply */
	udev_rules_iter_init();
	while (1) {
		rule = udev_rules_iter_next();
		if (rule == NULL)
			break;

		dbg("process rule");
		if (rule->name_operation != KEY_OP_UNSET || rule->symlink_operation != KEY_OP_UNSET ||
		    rule->mode != 0000 || rule->owner[0] != '\0' || rule->group[0] != '\0') {
			dbg("skip rule that names a device");
			continue;
		}

		if (match_rule(udev, rule, NULL, sysfs_device) == 0) {
			if (rule->ignore_device) {
				info("configured rule in '%s[%i]' applied, '%s' is ignored",
				     rule->config_file, rule->config_line, udev->kernel_name);
				udev->ignore_device = 1;
				return 0;
			}

			if (!udev->run_final && rule->run_operation != KEY_OP_UNSET) {
				char program[PATH_SIZE];

				if (rule->run_operation == KEY_OP_ASSIGN || rule->run_operation == KEY_OP_ASSIGN_FINAL) {
					struct name_entry *name_loop;
					struct name_entry *temp_loop;

					info("reset run list");
					list_for_each_entry_safe(name_loop, temp_loop, &udev->run_list, node) {
						list_del(&name_loop->node);
						free(name_loop);
					}
				}
				if (rule->run[0] != '\0') {
					strlcpy(program, rule->run, sizeof(program));
					apply_format(udev, program, sizeof(program), NULL, sysfs_device);
					dbg("add run '%s'", program);
					name_list_add(&udev->run_list, program, 0);
				}
				if (rule->run_operation == KEY_OP_ASSIGN_FINAL)
					break;
			}

			if (rule->last_rule) {
				dbg("last rule to be applied");
				break;
			}
		}
	}

	return 0;
}

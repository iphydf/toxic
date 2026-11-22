/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2026 The TokTok team.
 */

/*
 * Reads and writes toxic's config file for chat.tox.toxic.ToxicConfig, with the same libconfig
 * toxic uses. Text crosses JNI as UTF-8 bytes, and lists as NUL-terminated fields.
 */
#define _GNU_SOURCE

#include <jni.h>
#include <libconfig.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The type ToxicConfig.update passes for a setting to remove. */
#define REMOVE CONFIG_TYPE_NONE

static void throw_illegal_argument(JNIEnv *env, const char *message)
{
    const jclass cls = (*env)->FindClass(env, "java/lang/IllegalArgumentException");

    if (cls != NULL) {
        (*env)->ThrowNew(env, cls, message);
    }
}

/* Returns a NUL-terminated copy of bytes, or NULL if out of memory. */
static char *c_string(JNIEnv *env, jbyteArray bytes, jsize *length)
{
    *length = (*env)->GetArrayLength(env, bytes);
    char *text = malloc((size_t)*length + 1);

    if (text != NULL) {
        (*env)->GetByteArrayRegion(env, bytes, 0, *length, (jbyte *)text);
        text[*length] = '\0';
    }

    return text;
}

/* Reads UTF-8 bytes into cfg, or throws libconfig's error. */
static bool read_bytes(JNIEnv *env, config_t *cfg, jbyteArray bytes)
{
    jsize length;
    char *text = c_string(env, bytes, &length);

    if (text == NULL) {
        throw_illegal_argument(env, "out of memory");
        return false;
    }

    const bool ok = config_read_string(cfg, text) == CONFIG_TRUE;
    free(text);

    if (!ok) {
        char message[256];
        snprintf(message, sizeof(message), "line %d: %s", config_error_line(cfg), config_error_text(cfg));
        throw_illegal_argument(env, message);
    }

    return ok;
}

/* Runs write on an in-memory file and returns what it wrote, as Java bytes. */
static jbyteArray output_of(JNIEnv *env, const config_t *cfg, void (*write)(const config_t *cfg, FILE *out))
{
    char *data = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&data, &size);

    if (out == NULL) {
        return NULL;
    }

    write(cfg, out);
    fclose(out);
    const jbyteArray bytes = (*env)->NewByteArray(env, (jsize)size);

    if (bytes != NULL) {
        (*env)->SetByteArrayRegion(env, bytes, 0, (jsize)size, (const jbyte *)data);
    }

    free(data);
    return bytes;
}

static void put_field(FILE *out, const char *field)
{
    fputs(field, out);
    fputc('\0', out);
}

/* Lists the groups and values under setting: path, type, line and value for each. Arrays and
 * lists (like blocked_words) aren't listed. */
static void list_settings(FILE *out, const config_setting_t *setting, const char *prefix)
{
    const int count = config_setting_length(setting);

    for (int i = 0; i < count; ++i) {
        const config_setting_t *child = config_setting_get_elem(setting, i);
        const char *name = config_setting_name(child);

        if (name == NULL) {
            continue;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s%s%s", prefix, prefix[0] != '\0' ? "." : "", name);
        const int type = config_setting_type(child);
        char number[64] = "";
        const char *value = number;

        switch (type) {
            case CONFIG_TYPE_GROUP:
                break;

            case CONFIG_TYPE_INT:
                snprintf(number, sizeof(number), "%d", config_setting_get_int(child));
                break;

            case CONFIG_TYPE_INT64:
                snprintf(number, sizeof(number), "%lld", config_setting_get_int64(child));
                break;

            case CONFIG_TYPE_FLOAT:
                snprintf(number, sizeof(number), "%g", config_setting_get_float(child));
                break;

            case CONFIG_TYPE_BOOL:
                value = config_setting_get_bool(child) ? "true" : "false";
                break;

            case CONFIG_TYPE_STRING:
                value = config_setting_get_string(child);
                break;

            default:
                continue;
        }

        char numbers[32];
        put_field(out, path);
        snprintf(numbers, sizeof(numbers), "%d", type);
        put_field(out, numbers);
        snprintf(numbers, sizeof(numbers), "%u", config_setting_source_line(child));
        put_field(out, numbers);
        put_field(out, value);

        if (type == CONFIG_TYPE_GROUP) {
            list_settings(out, child, path);
        }
    }
}

static void list_config(const config_t *cfg, FILE *out)
{
    list_settings(out, config_root_setting(cfg), "");
}

/* Returns every group and value in a config file, or throws if it has a syntax error. */
JNIEXPORT jbyteArray JNICALL Java_chat_tox_toxic_ToxicConfig_parse(JNIEnv *env, jclass cls, jbyteArray text)
{
    (void)cls;
    config_t cfg;
    config_init(&cfg);
    const jbyteArray result = read_bytes(env, &cfg, text) ? output_of(env, &cfg, list_config) : NULL;
    config_destroy(&cfg);
    return result;
}

/* Returns the group that a dotted path's last name belongs in. Adds missing groups if create is
 * true. */
static config_setting_t *parent_group(config_t *cfg, char *path, char **name, bool create)
{
    config_setting_t *group = config_root_setting(cfg);
    char *part = path;
    char *dot;

    while ((dot = strchr(part, '.')) != NULL) {
        *dot = '\0';
        config_setting_t *member = config_setting_get_member(group, part);

        if (member == NULL && create) {
            member = config_setting_add(group, part, CONFIG_TYPE_GROUP);
        }

        if (member == NULL || config_setting_type(member) != CONFIG_TYPE_GROUP) {
            return NULL;
        }

        group = member;
        part = dot + 1;
    }

    *name = part;
    return group;
}

/* Removes a setting, and the groups that it leaves empty. */
static void remove_setting(config_t *cfg, char *path)
{
    char *name;
    config_setting_t *group = parent_group(cfg, path, &name, false);

    if (group == NULL || config_setting_remove(group, name) != CONFIG_TRUE) {
        return;
    }

    while (group != config_root_setting(cfg) && config_setting_length(group) == 0) {
        config_setting_t *parent = config_setting_parent(group);
        config_setting_remove(parent, config_setting_name(group));
        group = parent;
    }
}

/* Whether strtoll or strtod read all of text (end is where it stopped). */
static bool whole(const char *text, const char *end)
{
    return end != text && *end == '\0';
}

/* Sets a setting to a value written as text, or returns false if the value isn't of its type. */
static bool set_setting(config_t *cfg, char *path, int type, const char *value)
{
    char *name;
    config_setting_t *group = parent_group(cfg, path, &name, true);

    if (group == NULL) {
        return false;
    }

    config_setting_t *setting = config_setting_get_member(group, name);

    if (setting != NULL && config_setting_type(setting) != type) {
        config_setting_remove(group, name);
        setting = NULL;
    }

    if (setting == NULL) {
        setting = config_setting_add(group, name, type);
    }

    if (setting == NULL) {
        return false;
    }

    char *end = NULL;

    switch (type) {
        case CONFIG_TYPE_INT: {
            const long number = strtol(value, &end, 10);
            return whole(value, end) && config_setting_set_int(setting, (int)number) == CONFIG_TRUE;
        }

        case CONFIG_TYPE_INT64: {
            const long long number = strtoll(value, &end, 10);
            return whole(value, end) && config_setting_set_int64(setting, number) == CONFIG_TRUE;
        }

        case CONFIG_TYPE_FLOAT: {
            const double number = strtod(value, &end);
            return whole(value, end) && config_setting_set_float(setting, number) == CONFIG_TRUE;
        }

        case CONFIG_TYPE_BOOL:
            return config_setting_set_bool(setting, strcmp(value, "true") == 0) == CONFIG_TRUE;

        case CONFIG_TYPE_STRING:
            return config_setting_set_string(setting, value) == CONFIG_TRUE;

        default:
            return false;
    }
}

/* Returns the next NUL-terminated field before end, or NULL. */
static char *next_field(char **cursor, const char *end)
{
    if (*cursor >= end) {
        return NULL;
    }

    char *field = *cursor;
    *cursor += strlen(field) + 1;
    return field;
}

/*
 * Returns the config file text with changes made to it: path, type and value for each, where a
 * type of REMOVE removes the setting. Throws if the text has a syntax error or a value isn't of its
 * type.
 */
JNIEXPORT jbyteArray JNICALL Java_chat_tox_toxic_ToxicConfig_update(JNIEnv *env, jclass cls, jbyteArray text,
        jbyteArray changes)
{
    (void)cls;
    config_t cfg;
    config_init(&cfg);
    jsize length;
    char *fields = c_string(env, changes, &length);

    if (fields == NULL || !read_bytes(env, &cfg, text)) {
        free(fields);
        config_destroy(&cfg);
        return NULL;
    }

    char *cursor = fields;
    const char *end = fields + length;
    bool ok = true;

    while (ok) {
        char *path = next_field(&cursor, end);
        char *type = next_field(&cursor, end);
        char *value = next_field(&cursor, end);

        if (path == NULL || type == NULL || value == NULL) {
            break;
        }

        if (atoi(type) == REMOVE) {
            remove_setting(&cfg, path);
        } else if (!set_setting(&cfg, path, atoi(type), value)) {
            char message[256];
            snprintf(message, sizeof(message), "%s: \"%s\" isn't a valid value", path, value);
            throw_illegal_argument(env, message);
            ok = false;
        }
    }

    const jbyteArray result = ok ? output_of(env, &cfg, config_write) : NULL;
    free(fields);
    config_destroy(&cfg);
    return result;
}

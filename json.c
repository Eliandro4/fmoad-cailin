#include "json.h"
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_KEY_LEN 256
#define MAX_STRING_LEN 4096

typedef enum {
    JSON_STRING,
    JSON_OBJECT,
    JSON_ARRAY
} json_type;

struct json_value {
    json_type type;
    union {
        struct {
            char *str;
        } string;
        struct {
            char keys[MAX_KEY_LEN];
            json_value **values;
            size_t count;
            size_t cap;
        } object;
        struct {
            json_value **items;
            size_t count;
            size_t cap;
        } array;
    } u;
};

static void json_skip_whitespace(const char **p)
{
    while (isspace((unsigned char)**p))
        (*p)++;
}

static int json_parse_string(const char **p, char *out, size_t outlen)
{
    if (**p != '"')
        return -1;
    (*p)++;
    size_t i = 0;
    while (**p && **p != '"' && i < outlen - 1) {
        if (**p == '\\') {
            (*p)++;
            switch (**p) {
                case '"':  out[i++] = '"';  (*p)++; break;
                case '\\': out[i++] = '\\'; (*p)++; break;
                case '/':  out[i++] = '/';  (*p)++; break;
                case 'b':  out[i++] = '\b'; (*p)++; break;
                case 'f':  out[i++] = '\f'; (*p)++; break;
                case 'n':  out[i++] = '\n'; (*p)++; break;
                case 'r':  out[i++] = '\r'; (*p)++; break;
                case 't':  out[i++] = '\t'; (*p)++; break;
                default:   out[i++] = '\\'; (*p)++; break;
            }
        } else {
            out[i++] = *(*p)++;
        }
    }
    out[i] = '\0';
    if (**p != '"')
        return -1;
    (*p)++;
    return 0;
}

static json_value *json_parse_value(const char **p);

static json_value *json_parse_object(const char **p)
{
    if (**p != '{')
        return NULL;
    (*p)++;
    json_skip_whitespace(p);
    json_value *obj = calloc(1, sizeof(json_value));
    obj->type = JSON_OBJECT;
    obj->u.object.count = 0;
    obj->u.object.cap = 16;
    obj->u.object.values = calloc(obj->u.object.cap, sizeof(json_value *));
    while (**p && **p != '}') {
        json_skip_whitespace(p);
        char key[MAX_KEY_LEN];
        if (json_parse_string(p, key, sizeof(key)) != 0)
            break;
        json_skip_whitespace(p);
        if (**p != ':')
            break;
        (*p)++;
        json_skip_whitespace(p);
        json_value *val = json_parse_value(p);
        if (!val)
            break;
        if (obj->u.object.count >= obj->u.object.cap) {
            obj->u.object.cap *= 2;
            obj->u.object.values = realloc(obj->u.object.values,
                obj->u.object.cap * sizeof(json_value *));
        }
        strncpy(obj->u.object.keys + obj->u.object.count * MAX_KEY_LEN,
            key, MAX_KEY_LEN - 1);
        obj->u.object.values[obj->u.object.count] = val;
        obj->u.object.count++;
        json_skip_whitespace(p);
        if (**p == ',')
            (*p)++;
        else if (**p != '}')
            break;
    }
    if (**p == '}')
        (*p)++;
    return obj;
}

static json_value *json_parse_array(const char **p)
{
    if (**p != '[')
        return NULL;
    (*p)++;
    json_skip_whitespace(p);
    json_value *arr = calloc(1, sizeof(json_value));
    arr->type = JSON_ARRAY;
    arr->u.array.count = 0;
    arr->u.array.cap = 16;
    arr->u.array.items = calloc(arr->u.array.cap, sizeof(json_value *));
    while (**p && **p != ']') {
        json_skip_whitespace(p);
        json_value *val = json_parse_value(p);
        if (!val)
            break;
        if (arr->u.array.count >= arr->u.array.cap) {
            arr->u.array.cap *= 2;
            arr->u.array.items = realloc(arr->u.array.items,
                arr->u.array.cap * sizeof(json_value *));
        }
        arr->u.array.items[arr->u.array.count] = val;
        arr->u.array.count++;
        json_skip_whitespace(p);
        if (**p == ',')
            (*p)++;
        else if (**p != ']')
            break;
    }
    if (**p == ']')
        (*p)++;
    return arr;
}

static json_value *json_parse_value(const char **p)
{
    json_skip_whitespace(p);
    if (**p == '"') {
        char str[MAX_STRING_LEN];
        if (json_parse_string(p, str, sizeof(str)) != 0)
            return NULL;
        json_value *val = calloc(1, sizeof(json_value));
        val->type = JSON_STRING;
        val->u.string.str = strdup(str);
        return val;
    } else if (**p == '{') {
        return json_parse_object(p);
    } else if (**p == '[') {
        return json_parse_array(p);
    } else if (strncmp(*p, "null", 4) == 0) {
        *p += 4;
        return NULL;
    } else if (strncmp(*p, "true", 4) == 0) {
        *p += 4;
        json_value *val = calloc(1, sizeof(json_value));
        val->type = JSON_STRING;
        val->u.string.str = strdup("true");
        return val;
    } else if (strncmp(*p, "false", 5) == 0) {
        *p += 5;
        json_value *val = calloc(1, sizeof(json_value));
        val->type = JSON_STRING;
        val->u.string.str = strdup("false");
        return val;
    }
    return NULL;
}

json_value *json_parse_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if ((long)fread(buf, 1, len, f) != len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    const char *p = buf;
    json_value *val = json_parse_object(&p);
    free(buf);
    return val;
}

const char *json_get_string(const json_value *val, const char *key)
{
    if (!val || val->type != JSON_OBJECT || !key)
        return NULL;
    for (size_t i = 0; i < val->u.object.count; i++) {
        if (strcmp(val->u.object.keys + i * MAX_KEY_LEN, key) == 0) {
            if (val->u.object.values[i]->type == JSON_STRING)
                return val->u.object.values[i]->u.string.str;
            return NULL;
        }
    }
    return NULL;
}

const json_value *json_get(const json_value *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT || !key)
        return NULL;
    for (size_t i = 0; i < obj->u.object.count; i++) {
        if (strcmp(obj->u.object.keys + i * MAX_KEY_LEN, key) == 0)
            return obj->u.object.values[i];
    }
    return NULL;
}

size_t json_array_length(const json_value *arr)
{
    if (!arr || arr->type != JSON_ARRAY)
        return 0;
    return arr->u.array.count;
}

const json_value *json_array_get(const json_value *arr, size_t index)
{
    if (!arr || arr->type != JSON_ARRAY || index >= arr->u.array.count)
        return NULL;
    return arr->u.array.items[index];
}

static void json_free_value(json_value *val)
{
    if (!val)
        return;
    switch (val->type) {
        case JSON_STRING:
            free(val->u.string.str);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < val->u.object.count; i++)
                json_free_value(val->u.object.values[i]);
            free(val->u.object.values);
            break;
        case JSON_ARRAY:
            for (size_t i = 0; i < val->u.array.count; i++)
                json_free_value(val->u.array.items[i]);
            free(val->u.array.items);
            break;
    }
    free(val);
}

void json_free(json_value *val)
{
    json_free_value(val);
}

#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef struct json_value json_value;

const char *json_get_string(const json_value *val, const char *key);
const json_value *json_get(const json_value *obj, const char *key);
size_t json_array_length(const json_value *arr);
const json_value *json_array_get(const json_value *arr, size_t index);
json_value *json_parse_file(const char *path);
void json_free(json_value *val);

#endif

/* Minimal subset of cJSON declarations for compilation placeholder. */
#ifndef CJSON_H
#define CJSON_H

#include <stddef.h>

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

cJSON *cJSON_Parse(const char *value);
void cJSON_Delete(cJSON *item);
const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *const object, const char *const string);
const cJSON *cJSON_GetObjectItem(const cJSON * const object, const char * const string);
const cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
int cJSON_GetArraySize(const cJSON *array);
int cJSON_IsString(const cJSON *const item);
int cJSON_IsNumber(const cJSON *const item);
int cJSON_IsObject(const cJSON *const item);
int cJSON_IsArray(const cJSON *const item);

#endif

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>

#ifndef _MSC_VER
#define _strdup strdup
#endif

/* ----------------------------- JSON parsing ----------------------------- */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct {
    char **keys;
    JsonValue **values;
    size_t size;
    size_t capacity;
} JsonObject;

typedef struct {
    JsonValue **items;
    size_t size;
    size_t capacity;
} JsonArray;

struct JsonValue {
    JsonType type;
    union {
        int boolean;
        double number;
        char *string;
        JsonObject object;
        JsonArray array;
    } as;
};

typedef struct {
    const char *start;
    const char *cur;
    char error[128];
} JsonParser;

static void json_skip_ws(JsonParser *parser);
static JsonValue *json_parse_value(JsonParser *parser);

static void *xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

static void json_error(JsonParser *parser, const char *message) {
    if (!parser->error[0]) {
        snprintf(parser->error, sizeof(parser->error), "%s near %.32s", message, parser->cur);
    }
}

static int json_peek(JsonParser *parser) {
    return *parser->cur;
}

static int json_next(JsonParser *parser) {
    if (*parser->cur == '\0') {
        return '\0';
    }
    parser->cur += 1;
    return parser->cur[-1];
}

static void json_expect(JsonParser *parser, char ch) {
    if (json_next(parser) != ch) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Expected '%c'", ch);
        json_error(parser, msg);
    }
}

static char *json_parse_string_literal(JsonParser *parser) {
    json_expect(parser, '"');
    size_t capacity = 32;
    size_t length = 0;
    char *buffer = (char *)xmalloc(capacity);

    while (*parser->cur && *parser->cur != '"') {
        char ch = json_next(parser);
        if (ch == '\\') {
            ch = json_next(parser);
            if (ch == '\0') {
                json_error(parser, "Unterminated escape sequence");
                free(buffer);
                return NULL;
            }
            switch (ch) {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'b':
                    ch = '\b';
                    break;
                case 'f':
                    ch = '\f';
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                case 'u': {
                    /* Preserve unicode escape sequences verbatim */
                    if (length + 6 >= capacity) {
                        capacity *= 2;
                        buffer = (char *)realloc(buffer, capacity);
                        if (!buffer) {
                            fprintf(stderr, "Out of memory\n");
                            exit(EXIT_FAILURE);
                        }
                    }
                    buffer[length++] = '\\';
                    buffer[length++] = 'u';
                    for (int i = 0; i < 4; ++i) {
                        buffer[length++] = json_next(parser);
                    }
                    continue;
                }
                default:
                    json_error(parser, "Invalid escape sequence");
                    free(buffer);
                    return NULL;
            }
        }
        if (length + 2 >= capacity) {
            capacity *= 2;
            buffer = (char *)realloc(buffer, capacity);
            if (!buffer) {
                fprintf(stderr, "Out of memory\n");
                exit(EXIT_FAILURE);
            }
        }
        buffer[length++] = ch;
    }

    if (json_peek(parser) != '"') {
        json_error(parser, "Unterminated string literal");
        free(buffer);
        return NULL;
    }
    json_expect(parser, '"');
    buffer[length] = '\0';
    return buffer;
}

static JsonValue *json_make_value(JsonType type) {
    JsonValue *value = (JsonValue *)xmalloc(sizeof(JsonValue));
    value->type = type;
    if (type == JSON_ARRAY) {
        value->as.array.items = NULL;
        value->as.array.size = 0;
        value->as.array.capacity = 0;
    } else if (type == JSON_OBJECT) {
        value->as.object.keys = NULL;
        value->as.object.values = NULL;
        value->as.object.size = 0;
        value->as.object.capacity = 0;
    }
    return value;
}

static void json_array_push(JsonValue *arrayValue, JsonValue *item) {
    JsonArray *array = &arrayValue->as.array;
    if (array->size == array->capacity) {
        array->capacity = array->capacity ? array->capacity * 2 : 4;
        array->items = (JsonValue **)realloc(array->items, array->capacity * sizeof(JsonValue *));
        if (!array->items) {
            fprintf(stderr, "Out of memory\n");
            exit(EXIT_FAILURE);
        }
    }
    array->items[array->size++] = item;
}

static void json_object_put(JsonValue *objectValue, char *key, JsonValue *value) {
    JsonObject *object = &objectValue->as.object;
    if (object->size == object->capacity) {
        object->capacity = object->capacity ? object->capacity * 2 : 4;
        object->keys = (char **)realloc(object->keys, object->capacity * sizeof(char *));
        object->values = (JsonValue **)realloc(object->values, object->capacity * sizeof(JsonValue *));
        if (!object->keys || !object->values) {
            fprintf(stderr, "Out of memory\n");
            exit(EXIT_FAILURE);
        }
    }
    object->keys[object->size] = key;
    object->values[object->size] = value;
    object->size += 1;
}

static JsonValue *json_parse_number(JsonParser *parser) {
    const char *start = parser->cur;
    if (*parser->cur == '-') {
        parser->cur++;
    }
    while (*parser->cur >= '0' && *parser->cur <= '9') {
        parser->cur++;
    }
    if (*parser->cur == '.') {
        parser->cur++;
        while (*parser->cur >= '0' && *parser->cur <= '9') {
            parser->cur++;
        }
    }
    if (*parser->cur == 'e' || *parser->cur == 'E') {
        parser->cur++;
        if (*parser->cur == '+' || *parser->cur == '-') {
            parser->cur++;
        }
        while (*parser->cur >= '0' && *parser->cur <= '9') {
            parser->cur++;
        }
    }
    size_t length = (size_t)(parser->cur - start);
    char *buffer = (char *)xmalloc(length + 1);
    memcpy(buffer, start, length);
    buffer[length] = '\0';
    double number = strtod(buffer, NULL);
    free(buffer);
    JsonValue *value = json_make_value(JSON_NUMBER);
    value->as.number = number;
    return value;
}

static JsonValue *json_parse_array(JsonParser *parser) {
    json_expect(parser, '[');
    JsonValue *array = json_make_value(JSON_ARRAY);
    json_skip_ws(parser);
    if (json_peek(parser) == ']') {
        json_expect(parser, ']');
        return array;
    }
    while (1) {
        json_skip_ws(parser);
        JsonValue *item = json_parse_value(parser);
        if (!item) {
            json_error(parser, "Invalid array item");
            return array;
        }
        json_array_push(array, item);
        json_skip_ws(parser);
        if (json_peek(parser) == ',') {
            json_expect(parser, ',');
            continue;
        }
        break;
    }
    if (json_peek(parser) != ']') {
        json_error(parser, "Unterminated array");
    }
    json_expect(parser, ']');
    return array;
}

static JsonValue *json_parse_object(JsonParser *parser) {
    json_expect(parser, '{');
    JsonValue *object = json_make_value(JSON_OBJECT);
    json_skip_ws(parser);
    if (json_peek(parser) == '}') {
        json_expect(parser, '}');
        return object;
    }
    while (1) {
        json_skip_ws(parser);
        if (json_peek(parser) != '"') {
            json_error(parser, "Expected string key");
            return object;
        }
        char *key = json_parse_string_literal(parser);
        json_skip_ws(parser);
        json_expect(parser, ':');
        json_skip_ws(parser);
        JsonValue *value = json_parse_value(parser);
        if (!value) {
            json_error(parser, "Invalid object value");
            free(key);
            return object;
        }
        json_object_put(object, key, value);
        json_skip_ws(parser);
        if (json_peek(parser) == ',') {
            json_expect(parser, ',');
            continue;
        }
        break;
    }
    if (json_peek(parser) != '}') {
        json_error(parser, "Unterminated object");
    }
    json_expect(parser, '}');
    return object;
}

static JsonValue *json_parse_literal(JsonParser *parser, const char *literal, JsonType type, int boolValue) {
    size_t len = strlen(literal);
    if (strncmp(parser->cur, literal, len) != 0) {
        json_error(parser, "Unexpected literal");
        return NULL;
    }
    parser->cur += len;
    JsonValue *value = json_make_value(type);
    if (type == JSON_BOOL) {
        value->as.boolean = boolValue;
    }
    return value;
}

static JsonValue *json_parse_value(JsonParser *parser) {
    json_skip_ws(parser);
    int ch = json_peek(parser);
    switch (ch) {
        case '"': {
            char *text = json_parse_string_literal(parser);
            if (!text) {
                return NULL;
            }
            JsonValue *value = json_make_value(JSON_STRING);
            value->as.string = text;
            return value;
        }
        case '{':
            return json_parse_object(parser);
        case '[':
            return json_parse_array(parser);
        case 't':
            return json_parse_literal(parser, "true", JSON_BOOL, 1);
        case 'f':
            return json_parse_literal(parser, "false", JSON_BOOL, 0);
        case 'n':
            return json_parse_literal(parser, "null", JSON_NULL, 0);
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return json_parse_number(parser);
        default:
            json_error(parser, "Unexpected character");
            return NULL;
    }
}

static void json_skip_ws(JsonParser *parser) {
    while (*parser->cur == ' ' || *parser->cur == '\n' || *parser->cur == '\r' || *parser->cur == '\t') {
        parser->cur++;
    }
}

static JsonValue *json_parse(const char *text) {
    JsonParser parser;
    parser.start = text;
    parser.cur = text;
    parser.error[0] = '\0';
    JsonValue *value = json_parse_value(&parser);
    if (!value || parser.error[0]) {
        fprintf(stderr, "JSON parse error: %s\n", parser.error[0] ? parser.error : "unknown");
        return NULL;
    }
    json_skip_ws(&parser);
    if (*parser.cur != '\0') {
        fprintf(stderr, "JSON parse error: trailing characters\n");
        return NULL;
    }
    return value;
}

static void json_free(JsonValue *value) {
    if (!value) return;
    switch (value->type) {
        case JSON_STRING:
            free(value->as.string);
            break;
        case JSON_ARRAY: {
            JsonArray *array = &value->as.array;
            for (size_t i = 0; i < array->size; ++i) {
                json_free(array->items[i]);
            }
            free(array->items);
            break;
        }
        case JSON_OBJECT: {
            JsonObject *object = &value->as.object;
            for (size_t i = 0; i < object->size; ++i) {
                free(object->keys[i]);
                json_free(object->values[i]);
            }
            free(object->keys);
            free(object->values);
            break;
        }
        default:
            break;
    }
    free(value);
}

static JsonValue *json_object_get(const JsonValue *objectValue, const char *key) {
    if (!objectValue || objectValue->type != JSON_OBJECT) return NULL;
    const JsonObject *object = &objectValue->as.object;
    for (size_t i = 0; i < object->size; ++i) {
        if (strcmp(object->keys[i], key) == 0) {
            return object->values[i];
        }
    }
    return NULL;
}

static const char *json_get_string(const JsonValue *value, const char *defaultValue) {
    if (!value) return defaultValue;
    if (value->type == JSON_STRING) {
        return value->as.string ? value->as.string : defaultValue;
    }
    return defaultValue;
}

static double json_get_number(const JsonValue *value, double defaultValue) {
    if (!value) return defaultValue;
    if (value->type == JSON_NUMBER) {
        return value->as.number;
    }
    return defaultValue;
}

static int json_get_bool(const JsonValue *value, int defaultValue) {
    if (!value) return defaultValue;
    if (value->type == JSON_BOOL) {
        return value->as.boolean;
    }
    return defaultValue;
}

static size_t json_array_size(const JsonValue *value) {
    if (!value || value->type != JSON_ARRAY) return 0;
    return value->as.array.size;
}

static JsonValue *json_array_get(const JsonValue *value, size_t index) {
    if (!value || value->type != JSON_ARRAY) return NULL;
    if (index >= value->as.array.size) return NULL;
    return value->as.array.items[index];
}

/* -------------------------- HTTP request helpers ------------------------ */

typedef struct {
    char *data;
    size_t size;
} MemoryBuffer;

static size_t write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;
    char *ptr = (char *)realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Out of memory\n");
        return 0;
    }
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

static char *http_post_json(const char *url, const char *token, const char *payload) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to initialise libcurl\n");
        return NULL;
    }

    MemoryBuffer buffer = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "Content-Type: application/json");

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "User-Agent: auto-website-c-client");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buffer);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "Request failed: %s\n", curl_easy_strerror(res));
        free(buffer.data);
        return NULL;
    }
    if (response_code != 200) {
        fprintf(stderr, "GitHub API returned status %ld: %s\n", response_code, buffer.data ? buffer.data : "<empty>");
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

/* ----------------------------- Data structs ----------------------------- */

typedef struct {
    char *language;
    long long bytes;
    double share;
} LanguageEntry;

typedef struct {
    LanguageEntry *items;
    size_t size;
    size_t capacity;
} LanguageList;

typedef struct {
    char *name;
    char *description;
    char *language;
    char *url;
    char *updated_at;
    int stars;
    int forks;
} RepoEntry;

typedef struct {
    RepoEntry *items;
    size_t size;
    size_t capacity;
} RepoList;

typedef struct {
    char *date;
    int count;
} ContributionPoint;

typedef struct {
    ContributionPoint *items;
    size_t size;
    size_t capacity;
} ContributionList;

typedef struct {
    char *login;
    char *name;
    char *avatar_url;
    char *bio;
    char *location;
    char *blog;
    int followers;
    int following;
    int public_repos;
    int total_stars;
    int total_forks;
    int total_contributions;
    char generated_at[32];
    RepoList top_repos;
    LanguageList languages;
    ContributionList contributions;
} Context;

static void language_list_init(LanguageList *list) {
    list->items = NULL;
    list->size = 0;
    list->capacity = 0;
}

static void language_list_add(LanguageList *list, const char *name, long long bytes) {
    for (size_t i = 0; i < list->size; ++i) {
        if (strcmp(list->items[i].language, name) == 0) {
            list->items[i].bytes += bytes;
            return;
        }
    }
    if (list->size == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->items = (LanguageEntry *)realloc(list->items, list->capacity * sizeof(LanguageEntry));
        if (!list->items) {
            fprintf(stderr, "Out of memory\n");
            exit(EXIT_FAILURE);
        }
    }
    list->items[list->size].language = _strdup(name);
    list->items[list->size].bytes = bytes;
    list->items[list->size].share = 0.0;
    list->size += 1;
}

static void repo_list_init(RepoList *list) {
    list->items = NULL;
    list->size = 0;
    list->capacity = 0;
}

static void repo_list_push(RepoList *list, RepoEntry entry) {
    if (list->size == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->items = (RepoEntry *)realloc(list->items, list->capacity * sizeof(RepoEntry));
        if (!list->items) {
            fprintf(stderr, "Out of memory\n");
            exit(EXIT_FAILURE);
        }
    }
    list->items[list->size++] = entry;
}

static void contribution_list_init(ContributionList *list) {
    list->items = NULL;
    list->size = 0;
    list->capacity = 0;
}

static void contribution_list_push(ContributionList *list, const char *date, int count) {
    if (list->size == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 32;
        list->items = (ContributionPoint *)realloc(list->items, list->capacity * sizeof(ContributionPoint));
        if (!list->items) {
            fprintf(stderr, "Out of memory\n");
            exit(EXIT_FAILURE);
        }
    }
    list->items[list->size].date = _strdup(date);
    list->items[list->size].count = count;
    list->size += 1;
}

static void free_context(Context *ctx) {
    for (size_t i = 0; i < ctx->top_repos.size; ++i) {
        RepoEntry *repo = &ctx->top_repos.items[i];
        free(repo->name);
        free(repo->description);
        free(repo->language);
        free(repo->url);
        free(repo->updated_at);
    }
    free(ctx->top_repos.items);

    for (size_t i = 0; i < ctx->languages.size; ++i) {
        free(ctx->languages.items[i].language);
    }
    free(ctx->languages.items);

    for (size_t i = 0; i < ctx->contributions.size; ++i) {
        free(ctx->contributions.items[i].date);
    }
    free(ctx->contributions.items);

    free(ctx->login);
    free(ctx->name);
    free(ctx->avatar_url);
    free(ctx->bio);
    free(ctx->location);
    free(ctx->blog);
}

static char *dup_or_empty(const char *value) {
    if (!value) return _strdup("");
    return _strdup(value);
}

static int compare_repos(const void *lhs, const void *rhs) {
    const RepoEntry *a = (const RepoEntry *)lhs;
    const RepoEntry *b = (const RepoEntry *)rhs;
    if (b->stars != a->stars) {
        return b->stars - a->stars;
    }
    if (b->forks != a->forks) {
        return b->forks - a->forks;
    }
    return strcmp(a->name, b->name);
}

/* ---------------------------- GraphQL payload --------------------------- */

static char *build_graphql_payload(const char *username) {
    const char *query =
        "query ($login: String!) {\n"
        "  user(login: $login) {\n"
        "    login\n"
        "    name\n"
        "    avatarUrl\n"
        "    bio\n"
        "    location\n"
        "    websiteUrl\n"
        "    followers { totalCount }\n"
        "    following { totalCount }\n"
        "    repositoriesTotal: repositories(ownerAffiliations: OWNER, privacy: PUBLIC) { totalCount }\n"
        "    repositories(first: 100, ownerAffiliations: OWNER, privacy: PUBLIC, orderBy: {field: STARGAZERS, direction: DESC}) {\n"
        "      nodes {\n"
        "        name\n"
        "        description\n"
        "        stargazerCount\n"
        "        forkCount\n"
        "        url\n"
        "        updatedAt\n"
        "        isFork\n"
        "        primaryLanguage { name }\n"
        "        languages(first: 10, orderBy: {field: SIZE, direction: DESC}) {\n"
        "          edges { size node { name } }\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "    contributionsCollection {\n"
        "      contributionCalendar {\n"
        "        totalContributions\n"
        "        weeks {\n"
        "          contributionDays { date contributionCount }\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "}\n";

    size_t payload_size = strlen(query) + strlen(username) + 128;
    char *payload = (char *)xmalloc(payload_size);
    snprintf(payload, payload_size, "{\"query\":\"%s\",\"variables\":{\"login\":\"%s\"}}", query, username);

    /* Replace newline characters with escaped sequence */
    size_t len = strlen(payload);
    size_t extra = 0;
    for (size_t i = 0; i < len; ++i) {
        if (payload[i] == '\n') extra++;
    }
    if (extra) {
        char *expanded = (char *)xmalloc(len + extra + 1);
        size_t j = 0;
        for (size_t i = 0; i < len; ++i) {
            if (payload[i] == '\n') {
                expanded[j++] = '\\';
                expanded[j++] = 'n';
            } else {
                expanded[j++] = payload[i];
            }
        }
        expanded[j] = '\0';
        free(payload);
        payload = expanded;
    }
    return payload;
}

/* ---------------------------- Data extraction --------------------------- */

static void extract_languages(LanguageList *languages, const JsonValue *languagesObj) {
    if (!languagesObj || languagesObj->type != JSON_OBJECT) return;
    JsonValue *edgesVal = json_object_get(languagesObj, "edges");
    if (!edgesVal || edgesVal->type != JSON_ARRAY) return;
    for (size_t i = 0; i < edgesVal->as.array.size; ++i) {
        JsonValue *edge = edgesVal->as.array.items[i];
        if (!edge || edge->type != JSON_OBJECT) continue;
        JsonValue *sizeVal = json_object_get(edge, "size");
        JsonValue *nodeVal = json_object_get(edge, "node");
        if (!sizeVal || !nodeVal || nodeVal->type != JSON_OBJECT) continue;
        JsonValue *nameVal = json_object_get(nodeVal, "name");
        if (!nameVal || nameVal->type != JSON_STRING) continue;
        long long bytes = (long long)json_get_number(sizeVal, 0.0);
        language_list_add(languages, nameVal->as.string, bytes);
    }
}

static void extract_contributions(ContributionList *list, const JsonValue *calendarVal) {
    if (!calendarVal || calendarVal->type != JSON_OBJECT) return;
    JsonValue *weeksVal = json_object_get(calendarVal, "weeks");
    if (!weeksVal || weeksVal->type != JSON_ARRAY) return;
    for (size_t i = 0; i < weeksVal->as.array.size; ++i) {
        JsonValue *week = weeksVal->as.array.items[i];
        JsonValue *daysVal = json_object_get(week, "contributionDays");
        if (!daysVal || daysVal->type != JSON_ARRAY) continue;
        for (size_t j = 0; j < daysVal->as.array.size; ++j) {
            JsonValue *day = daysVal->as.array.items[j];
            if (!day || day->type != JSON_OBJECT) continue;
            const char *date = json_get_string(json_object_get(day, "date"), "");
            int count = (int)json_get_number(json_object_get(day, "contributionCount"), 0.0);
            contribution_list_push(list, date, count);
        }
    }
}

static void trim_contributions(ContributionList *list, size_t maxCount) {
    if (list->size <= maxCount) return;
    size_t offset = list->size - maxCount;
    for (size_t i = 0; i < offset; ++i) {
        free(list->items[i].date);
    }
    memmove(list->items, list->items + offset, (list->size - offset) * sizeof(ContributionPoint));
    list->size -= offset;
}

static void compute_language_shares(LanguageList *list) {
    long long total = 0;
    for (size_t i = 0; i < list->size; ++i) {
        total += list->items[i].bytes;
    }
    for (size_t i = 0; i < list->size; ++i) {
        if (total == 0) {
            list->items[i].share = 0.0;
        } else {
            list->items[i].share = ((double)list->items[i].bytes / (double)total) * 100.0;
        }
    }
}

static int compare_languages(const void *lhs, const void *rhs) {
    const LanguageEntry *a = (const LanguageEntry *)lhs;
    const LanguageEntry *b = (const LanguageEntry *)rhs;
    if (b->bytes > a->bytes) return 1;
    if (b->bytes < a->bytes) return -1;
    return strcmp(a->language, b->language);
}

static char *html_escape(const char *text) {
    size_t length = strlen(text);
    size_t capacity = length + 1;
    char *buffer = (char *)xmalloc(capacity);
    size_t idx = 0;
    for (size_t i = 0; i < length; ++i) {
        char ch = text[i];
        const char *replacement = NULL;
        switch (ch) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            default: break;
        }
        if (replacement) {
            size_t repLen = strlen(replacement);
            if (idx + repLen + 1 > capacity) {
                capacity = (capacity + repLen + 16) * 2;
                buffer = (char *)realloc(buffer, capacity);
                if (!buffer) {
                    fprintf(stderr, "Out of memory\n");
                    exit(EXIT_FAILURE);
                }
            }
            memcpy(buffer + idx, replacement, repLen);
            idx += repLen;
        } else {
            if (idx + 2 > capacity) {
                capacity = (capacity + 16) * 2;
                buffer = (char *)realloc(buffer, capacity);
                if (!buffer) {
                    fprintf(stderr, "Out of memory\n");
                    exit(EXIT_FAILURE);
                }
            }
            buffer[idx++] = ch;
        }
    }
    buffer[idx] = '\0';
    return buffer;
}

static void write_language_json(FILE *fp, const LanguageList *languages) {
    fprintf(fp, "[");
    for (size_t i = 0; i < languages->size; ++i) {
        const LanguageEntry *entry = &languages->items[i];
        if (i > 0) fprintf(fp, ",");
        fprintf(fp, "{\"language\":\"%s\",\"share\":%.2f,\"bytes\":%lld}", entry->language, entry->share, entry->bytes);
    }
    fprintf(fp, "]");
}

static void write_contribution_json(FILE *fp, const ContributionList *contribs) {
    fprintf(fp, "[");
    for (size_t i = 0; i < contribs->size; ++i) {
        if (i > 0) fprintf(fp, ",");
        fprintf(fp, "{\"date\":\"%s\",\"count\":%d}", contribs->items[i].date, contribs->items[i].count);
    }
    fprintf(fp, "]");
}

static void write_html(const Context *ctx, const char *output_path) {
    FILE *fp = fopen(output_path, "w");
    if (!fp) {
        perror("fopen");
        return;
    }

    char *nameEsc = html_escape(ctx->name);
    char *loginEsc = html_escape(ctx->login);
    char *bioEsc = html_escape(ctx->bio);
    char *locationEsc = html_escape(ctx->location);
    char *blogEsc = html_escape(ctx->blog);
    char *avatarEsc = html_escape(ctx->avatar_url);

    fprintf(fp, "<!DOCTYPE html>\n");
    fprintf(fp, "<html lang=\"en\">\n<head>\n");
    fprintf(fp, "    <meta charset=\"utf-8\">\n");
    fprintf(fp, "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    fprintf(fp, "    <meta name=\"description\" content=\"Live GitHub statistics for %s (@%s). Updated daily via GitHub Actions.\">\n", nameEsc, loginEsc);
    fprintf(fp, "    <title>%s · GitHub Insights</title>\n", nameEsc);
    fprintf(fp, "    <link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">\n");
    fprintf(fp, "    <link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>\n");
    fprintf(fp, "    <link href=\"https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap\" rel=\"stylesheet\">\n");
    fprintf(fp, "    <link rel=\"stylesheet\" href=\"assets/styles.css\">\n");
    fprintf(fp, "    <script defer src=\"https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js\"></script>\n");
    fprintf(fp, "</head>\n<body>\n");

    fprintf(fp, "    <header class=\"hero\">\n        <div class=\"hero__avatar\">\n            <img src=\"%s\" alt=\"%s avatar\" loading=\"lazy\">\n        </div>\n        <div>\n            <h1>%s</h1>\n            <p class=\"hero__handle\">@%s</p>\n", avatarEsc, nameEsc, nameEsc, loginEsc);
    if (strlen(ctx->bio) > 0) {
        fprintf(fp, "            <p class=\"hero__tagline\">%s</p>\n", bioEsc);
    }
    fprintf(fp, "            <div class=\"hero__meta\">\n");
    if (strlen(ctx->location) > 0) {
        fprintf(fp, "                <span>📍 %s</span>\n", locationEsc);
    }
    if (strlen(ctx->blog) > 0) {
        fprintf(fp, "                <span>🔗 <a href=\"%s\" target=\"_blank\" rel=\"noopener\">%s</a></span>\n", blogEsc, blogEsc);
    }
    fprintf(fp, "            </div>\n        </div>\n    </header>\n");

    fprintf(fp, "    <main>\n");
    fprintf(fp, "        <section class=\"stats-grid\" aria-label=\"Key metrics\">\n");
    fprintf(fp, "            <article class=\"stat-card\"><h2>Total Stars</h2><p class=\"stat-card__value\">%d</p><p class=\"stat-card__hint\">Across public repositories</p></article>\n", ctx->total_stars);
    fprintf(fp, "            <article class=\"stat-card\"><h2>Followers</h2><p class=\"stat-card__value\">%d</p><p class=\"stat-card__hint\">On GitHub</p></article>\n", ctx->followers);
    fprintf(fp, "            <article class=\"stat-card\"><h2>Repositories</h2><p class=\"stat-card__value\">%d</p><p class=\"stat-card__hint\">Public projects</p></article>\n", ctx->public_repos);
    fprintf(fp, "            <article class=\"stat-card\"><h2>Contributions</h2><p class=\"stat-card__value\">%d</p><p class=\"stat-card__hint\">Past 365 days</p></article>\n", ctx->total_contributions);
    fprintf(fp, "            <article class=\"stat-card\"><h2>Total Forks</h2><p class=\"stat-card__value\">%d</p><p class=\"stat-card__hint\">Across top repos</p></article>\n", ctx->total_forks);
    fprintf(fp, "            <article class=\"stat-card\"><h2>Following</h2><p class=\"stat-card__value\">%d</p><p class=\"stat-card__hint\">Developers tracked</p></article>\n", ctx->following);
    fprintf(fp, "        </section>\n");

    fprintf(fp, "        <section class=\"panel\" aria-label=\"Language breakdown\">\n            <div class=\"panel__header\">\n                <h2>Language Footprint</h2>\n                <p>Distribution across public repositories (top %zu languages).</p>\n            </div>\n            <div class=\"panel__body panel__body--chart\">\n", ctx->languages.size);
    if (ctx->languages.size == 0) {
        fprintf(fp, "                <p>No language information available yet.</p>\n");
    } else {
        fprintf(fp, "                <canvas id=\"languageChart\" width=\"600\" height=\"320\" role=\"img\" aria-label=\"Language usage chart\"></canvas>\n");
        fprintf(fp, "                <table class=\"language-table\">\n                    <thead>\n                        <tr><th scope=\"col\">Language</th><th scope=\"col\">Share</th><th scope=\"col\">Source bytes</th></tr>\n                    </thead>\n                    <tbody>\n");
        for (size_t i = 0; i < ctx->languages.size; ++i) {
            const LanguageEntry *entry = &ctx->languages.items[i];
            char *langEsc = html_escape(entry->language);
            fprintf(fp, "                        <tr><th scope=\"row\">%s</th><td>%.2f%%</td><td>%lld</td></tr>\n", langEsc, entry->share, entry->bytes);
            free(langEsc);
        }
        fprintf(fp, "                    </tbody>\n                </table>\n");
    }
    fprintf(fp, "            </div>\n        </section>\n");

    fprintf(fp, "        <section class=\"panel\" aria-label=\"Contribution activity\">\n            <div class=\"panel__header\">\n                <h2>Contribution Trend</h2>\n                <p>Commits, pull requests, issues, and reviews across the last %zu days.</p>\n            </div>\n            <div class=\"panel__body panel__body--chart\">\n", ctx->contributions.size);
    if (ctx->contributions.size == 0) {
        fprintf(fp, "                <p>No contribution data available.</p>\n");
    } else {
        fprintf(fp, "                <canvas id=\"contributionChart\" width=\"600\" height=\"320\" role=\"img\" aria-label=\"Contribution activity chart\"></canvas>\n");
    }
    fprintf(fp, "            </div>\n        </section>\n");

    fprintf(fp, "        <section class=\"panel\" aria-label=\"Highlighted repositories\">\n            <div class=\"panel__header\">\n                <h2>Spotlight Projects</h2>\n                <p>Top repositories ranked by stars and forks.</p>\n            </div>\n            <div class=\"repo-grid\">\n");
    if (ctx->top_repos.size == 0) {
        fprintf(fp, "                <p>No repositories to show yet. Keep building!</p>\n");
    } else {
        for (size_t i = 0; i < ctx->top_repos.size; ++i) {
            RepoEntry *repo = &ctx->top_repos.items[i];
            char *nameEsc = html_escape(repo->name);
            char *descEsc = html_escape(repo->description);
            char *langEsc = html_escape(repo->language);
            char *urlEsc = html_escape(repo->url);
            char *updatedEsc = html_escape(repo->updated_at);
            fprintf(fp, "                <article class=\"repo-card\">\n                    <header>\n                        <h3><a href=\"%s\" target=\"_blank\" rel=\"noopener\">%s</a></h3>\n                        <span class=\"repo-card__language\">%s</span>\n                    </header>\n", urlEsc, nameEsc, langEsc);
            if (strlen(repo->description) > 0) {
                fprintf(fp, "                    <p>%s</p>\n", descEsc);
            }
            fprintf(fp, "                    <footer>\n                        <span>⭐ %d</span>\n                        <span>🍴 %d</span>\n", repo->stars, repo->forks);
            if (strlen(repo->updated_at) >= 10) {
                fprintf(fp, "                        <span>🡅 %.10s</span>\n", updatedEsc);
            }
            fprintf(fp, "                    </footer>\n                </article>\n");
            free(nameEsc);
            free(descEsc);
            free(langEsc);
            free(urlEsc);
            free(updatedEsc);
        }
    }
    fprintf(fp, "            </div>\n        </section>\n");

    fprintf(fp, "    </main>\n");
    fprintf(fp, "    <footer class=\"footer\">\n        <p>Generated on %s by an automated workflow.</p>\n        <p>Source available on <a href=\"https://github.com/%s/Auto-Website\" target=\"_blank\" rel=\"noopener\">GitHub</a>.</p>\n    </footer>\n", ctx->generated_at, loginEsc);

    fprintf(fp, "    <script>\n    const languageData = ");
    write_language_json(fp, &ctx->languages);
    fprintf(fp, ";\n    const contributionData = ");
    write_contribution_json(fp, &ctx->contributions);
    fprintf(fp, ";\n    const palette = ['#5B8FF9','#5AD8A6','#5D7092','#F6BD16','#E8684A','#6DC8EC','#9270CA','#FF9D4D'];\n    function buildLanguageChart(){if(!languageData.length||!window.Chart)return;const ctx=document.getElementById('languageChart');const labels=languageData.map(i=>i.language);const shares=languageData.map(i=>i.share);new Chart(ctx,{type:'doughnut',data:{labels,datasets:[{data:shares,backgroundColor:palette,borderWidth:0}]},options:{plugins:{legend:{display:true,position:'bottom'}}}});}\n    function buildContributionChart(){if(!contributionData.length||!window.Chart)return;const ctx=document.getElementById('contributionChart');const labels=contributionData.map(p=>p.date);const counts=contributionData.map(p=>p.count);new Chart(ctx,{type:'line',data:{labels,datasets:[{label:'Daily contributions',data:counts,borderColor:'#5B8FF9',backgroundColor:'rgba(91,143,249,0.2)',tension:0.3,pointRadius:0,fill:true}]},options:{scales:{x:{ticks:{maxTicksLimit:8}},y:{beginAtZero:true}},plugins:{legend:{display:false}}}});}\n    document.addEventListener('DOMContentLoaded', ()=>{buildLanguageChart();buildContributionChart();});\n    </script>\n");
    fprintf(fp, "</body>\n</html>\n");

    free(nameEsc);
    free(loginEsc);
    free(bioEsc);
    free(locationEsc);
    free(blogEsc);
    free(avatarEsc);

    fclose(fp);
}

/* ------------------------------ Entry point ----------------------------- */

int main(void) {
    const char *token = getenv("GITHUB_TOKEN");
    if (!token || strlen(token) == 0) {
        token = getenv("GH_STATS_TOKEN");
    }
    if (!token || strlen(token) == 0) {
        fprintf(stderr, "Missing GITHUB_TOKEN or GH_STATS_TOKEN environment variable.\n");
        return EXIT_FAILURE;
    }
    const char *username = getenv("GITHUB_USERNAME");
    if (!username || strlen(username) == 0) {
        fprintf(stderr, "Missing GITHUB_USERNAME environment variable.\n");
        return EXIT_FAILURE;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    char *payload = build_graphql_payload(username);
    char *response = http_post_json("https://api.github.com/graphql", token, payload);
    free(payload);

    if (!response) {
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    JsonValue *root = json_parse(response);
    free(response);
    if (!root) {
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    JsonValue *dataVal = json_object_get(root, "data");
    JsonValue *userVal = json_object_get(dataVal, "user");
    if (!userVal) {
        fprintf(stderr, "GitHub API response missing user data.\n");
        json_free(root);
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    Context ctx;
    ctx.login = dup_or_empty(json_get_string(json_object_get(userVal, "login"), username));
    ctx.name = dup_or_empty(json_get_string(json_object_get(userVal, "name"), ctx.login));
    ctx.avatar_url = dup_or_empty(json_get_string(json_object_get(userVal, "avatarUrl"), ""));
    ctx.bio = dup_or_empty(json_get_string(json_object_get(userVal, "bio"), ""));
    ctx.location = dup_or_empty(json_get_string(json_object_get(userVal, "location"), ""));
    ctx.blog = dup_or_empty(json_get_string(json_object_get(userVal, "websiteUrl"), ""));
    ctx.followers = (int)json_get_number(json_object_get(json_object_get(userVal, "followers"), "totalCount"), 0);
    ctx.following = (int)json_get_number(json_object_get(json_object_get(userVal, "following"), "totalCount"), 0);
    ctx.public_repos = (int)json_get_number(json_object_get(json_object_get(userVal, "repositoriesTotal"), "totalCount"), 0);

    repo_list_init(&ctx.top_repos);
    language_list_init(&ctx.languages);
    contribution_list_init(&ctx.contributions);

    JsonValue *reposVal = json_object_get(json_object_get(userVal, "repositories"), "nodes");
    ctx.total_stars = 0;
    ctx.total_forks = 0;

    if (reposVal && reposVal->type == JSON_ARRAY) {
        for (size_t i = 0; i < reposVal->as.array.size; ++i) {
            JsonValue *repo = reposVal->as.array.items[i];
            if (!repo || repo->type != JSON_OBJECT) continue;
            if (json_get_bool(json_object_get(repo, "isFork"), 0)) {
                continue;
            }
            RepoEntry entry;
            entry.name = dup_or_empty(json_get_string(json_object_get(repo, "name"), ""));
            entry.description = dup_or_empty(json_get_string(json_object_get(repo, "description"), ""));
            entry.language = dup_or_empty(json_get_string(json_object_get(json_object_get(repo, "primaryLanguage"), "name"), "Unknown"));
            entry.url = dup_or_empty(json_get_string(json_object_get(repo, "url"), ""));
            entry.updated_at = dup_or_empty(json_get_string(json_object_get(repo, "updatedAt"), ""));
            entry.stars = (int)json_get_number(json_object_get(repo, "stargazerCount"), 0);
            entry.forks = (int)json_get_number(json_object_get(repo, "forkCount"), 0);
            ctx.total_stars += entry.stars;
            ctx.total_forks += entry.forks;
            repo_list_push(&ctx.top_repos, entry);

            JsonValue *languageVal = json_object_get(repo, "languages");
            extract_languages(&ctx.languages, languageVal);
        }
    }

    qsort(ctx.top_repos.items, ctx.top_repos.size, sizeof(RepoEntry), compare_repos);
    if (ctx.top_repos.size > 6) {
        for (size_t i = 6; i < ctx.top_repos.size; ++i) {
            RepoEntry *repo = &ctx.top_repos.items[i];
            free(repo->name);
            free(repo->description);
            free(repo->language);
            free(repo->url);
            free(repo->updated_at);
        }
        ctx.top_repos.size = 6;
    }

    compute_language_shares(&ctx.languages);
    qsort(ctx.languages.items, ctx.languages.size, sizeof(LanguageEntry), compare_languages);

    JsonValue *calendar = json_object_get(json_object_get(userVal, "contributionsCollection"), "contributionCalendar");
    ctx.total_contributions = (int)json_get_number(json_object_get(calendar, "totalContributions"), 0);
    extract_contributions(&ctx.contributions, calendar);
    trim_contributions(&ctx.contributions, 120);

    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    strftime(ctx.generated_at, sizeof(ctx.generated_at), "%Y-%m-%d %H:%M UTC", utc);

    write_html(&ctx, "docs/index.html");

    printf("Site updated for %s -> docs/index.html\n", ctx.login);

    free_context(&ctx);
    json_free(root);
    curl_global_cleanup();
    return EXIT_SUCCESS;
}

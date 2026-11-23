#ifdef _WIN32
#define __USE_MINGW_ANSI_STDIO 1
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "boolean_query.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum
{
    BUCKET_COUNT = 4093
};

typedef struct Entry
{
    char token[5];
    int *postings;
    size_t count;
    size_t capacity;
    struct Entry *next;
} Entry;

typedef struct
{
    Entry *buckets[BUCKET_COUNT];
} Index;

static double now_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return 1000.0 * (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return 1000.0 * (double)now.tv_sec + (double)now.tv_nsec / 1000000.0;
#endif
}

static size_t bucket_for(const char *token)
{
    size_t hash = 0;
    for (size_t i = 0; token[i] != '\0'; i++)
    {
        hash = (hash * 31 + (unsigned char)token[i]) % BUCKET_COUNT;
    }
    return hash;
}

static Entry *find_entry(const Index *index, size_t bucket, const char *token)
{
    for (Entry *entry = index->buckets[bucket]; entry != NULL; entry = entry->next)
    {
        if (strcmp(entry->token, token) == 0)
        {
            return entry;
        }
    }
    return NULL;
}

static int add_token(Index *index, const char *token, int doc_id)
{
    size_t bucket = bucket_for(token);
    Entry *entry = find_entry(index, bucket, token);

    if (entry == NULL)
    {
        entry = malloc(sizeof *entry);
        if (entry == NULL)
        {
            return 0;
        }
        *entry = (Entry){0};
        strcpy(entry->token, token);
        entry->next = index->buckets[bucket];
        index->buckets[bucket] = entry;
    }

    /* Chapters are indexed in increasing document ID order. */
    if (entry->count > 0 && entry->postings[entry->count - 1] == doc_id)
    {
        return 1;
    }
    if (entry->count == entry->capacity)
    {
        if (entry->capacity > SIZE_MAX / sizeof *entry->postings / 2)
        {
            return 0;
        }
        size_t new_capacity = entry->capacity == 0 ? 2 : entry->capacity * 2;
        int *larger = realloc(entry->postings, new_capacity * sizeof *larger);
        if (larger == NULL)
        {
            return 0;
        }
        entry->postings = larger;
        entry->capacity = new_capacity;
    }
    entry->postings[entry->count] = doc_id;
    entry->count++;
    return 1;
}

static void free_index(Index *index)
{
    for (size_t bucket = 0; bucket < BUCKET_COUNT; bucket++)
    {
        Entry *entry = index->buckets[bucket];
        while (entry != NULL)
        {
            Entry *next = entry->next;
            free(entry->postings);
            free(entry);
            entry = next;
        }
    }
}

static int utf8_width(int first)
{
    if (first < 0x80)
    {
        return 1;
    }
    if (first >= 0xC2 && first <= 0xDF)
    {
        return 2;
    }
    if (first >= 0xE0 && first <= 0xEF)
    {
        return 3;
    }
    if (first >= 0xF0 && first <= 0xF4)
    {
        return 4;
    }
    return 0;
}

static int read_character(FILE *file, char token[5], unsigned long *codepoint)
{
    int first = fgetc(file);
    if (first == EOF)
    {
        return ferror(file) ? -1 : 0;
    }
    int width = utf8_width(first);
    if (width == 0)
    {
        return -1;
    }

    const unsigned long mask[] = {0, 0x7F, 0x1F, 0x0F, 0x07};
    const unsigned long minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    unsigned long value = (unsigned long)first & mask[width];
    token[0] = (char)first;

    for (int i = 1; i < width; i++)
    {
        int next = fgetc(file);
        if (next < 0x80 || next > 0xBF)
        {
            return -1;
        }
        token[i] = (char)next;
        value = (value << 6) | (unsigned long)(next & 0x3F);
    }
    if (value < minimum[width] || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF))
    {
        return -1;
    }
    token[width] = '\0';
    *codepoint = value;
    return 1;
}

static int is_indexable(unsigned long value)
{
    return (value >= 0x3400 && value <= 0x4DBF) || (value >= 0x4E00 && value <= 0x9FFF) || (value >= 0xF900 && value <= 0xFAFF);
}

static int index_chapter(Index *index, const char *path, int doc_id)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        perror(path);
        return 0;
    }

    int status;
    char token[5];
    unsigned long codepoint;
    while ((status = read_character(file, token, &codepoint)) > 0)
    {
        if (is_indexable(codepoint) && !add_token(index, token, doc_id))
        {
            fprintf(stderr, "Cannot allocate index memory for %s.\n", path);
            fclose(file);
            return 0;
        }
    }

    if (status < 0)
    {
        if (ferror(file))
        {
            perror(path);
        }
        else
        {
            fprintf(stderr, "%s contains invalid UTF-8.\n", path);
        }
    }
    fclose(file);
    return status == 0;
}

static int build_index(Index *index, const char *directory, int chapter_count)
{
    for (int i = 0; i < chapter_count; i++)
    {
        int doc_id = i + 1;
        char path[1024];
        int length = snprintf(path, sizeof path, "%s/%03d.txt", directory, doc_id);
        if (length < 0 || (size_t)length >= sizeof path)
        {
            fprintf(stderr, "Chapter path is too long.\n");
            return 0;
        }
        if (!index_chapter(index, path, doc_id))
        {
            return 0;
        }
    }
    return 1;
}

static QueryResult lookup_term(const void *context, const char *term)
{
    const Index *index = context;
    const Entry *entry = find_entry(index, bucket_for(term), term);
    if (entry == NULL)
    {
        return (QueryResult){0};
    }
    return (QueryResult){entry->postings, entry->count, NULL};
}

static void print_postings(const char *expression, QueryResult result)
{
    printf("%s -> [", expression);
    for (size_t i = 0; i < result.count; i++)
    {
        if (i > 0)
        {
            printf(", ");
        }
        printf("%d", result.ids[i]);
    }
    puts("]");
}

static int run_queries(const Index *index, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        perror(path);
        return 0;
    }

    char line[4096];
    size_t line_number = 0;
    while (fgets(line, sizeof line, file) != NULL)
    {
        line_number++;
        char *token = line + strspn(line, " \t");
        if (line_number == 1 && strncmp(token, "\xef\xbb\xbf", 3) == 0)
        {
            token += 3;
            token += strspn(token, " \t");
        }
        size_t length = strcspn(token, "\r\n");
        if (strlen(line) == sizeof line - 1 && strchr(line, '\n') == NULL)
        {
            fprintf(stderr, "%s line %zu is too long.\n", path, line_number);
            fclose(file);
            return 0;
        }
        while (length > 0 && (token[length - 1] == ' ' || token[length - 1] == '\t'))
        {
            length--;
        }
        token[length] = '\0';
        if (length == 0)
        {
            continue;
        }
        QueryResult result;
        double started = now_ms();
        const char *error = evaluate_query(token, index, lookup_term, &result);
        double elapsed = now_ms() - started;
        if (error != NULL)
        {
            fprintf(stderr, "%s line %zu: %s.\n", path, line_number, error);
            fclose(file);
            return 0;
        }
        print_postings(token, result);
        printf("  Query: %.4f ms\n", elapsed);
        free(result.owned);
    }

    int ok = !ferror(file);
    if (!ok)
    {
        perror(path);
    }
    fclose(file);
    return ok;
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: book_search <chapter-dir> <chapter-count> <query-file>\n");
        return EXIT_FAILURE;
    }

    errno = 0;
    char *end;
    long count = strtol(argv[2], &end, 10);
    if (errno == ERANGE || *end != '\0' || count < 1 || count > INT_MAX)
    {
        fprintf(stderr, "Chapter count must be a positive integer.\n");
        return EXIT_FAILURE;
    }

    Index index = {0};
    double started = now_ms();
    if (!build_index(&index, argv[1], (int)count))
    {
        free_index(&index);
        return EXIT_FAILURE;
    }
    printf("Indexed %ld chapters in %.3f ms\n", count, now_ms() - started);

    int ok = run_queries(&index, argv[3]);
    free_index(&index);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

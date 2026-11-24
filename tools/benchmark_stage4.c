#include "boolean_query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    REPEATS = 2000, TRIALS = 9,
    LONG_COUNT = 16384, SHORT_COUNT = 2048, VERY_LONG_COUNT = 131072
};

typedef struct {
    QueryResult common;
    QueryResult other;
} Corpus;

static QueryResult lookup(const void *context, const char *term)
{
    const Corpus *corpus = context;
    return strcmp(term, "common") == 0 ? corpus->common : corpus->other;
}

static QueryResult query(const Corpus *corpus)
{
    QueryResult result;
    const char *error = evaluate_query("common AND other", corpus, lookup, 1, &result);
    if (error != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(EXIT_FAILURE);
    }
    return result;
}

static double measure(const Corpus *corpus)
{
    clock_t start = clock();
    for (int i = 0; i < REPEATS; i++) {
        QueryResult result = query(corpus);
        free(result.owned);
    }
    return 1000000.0 * (double)(clock() - start) / CLOCKS_PER_SEC / REPEATS;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

static void benchmark(const char *name, const int *left, size_t left_count,
                      const int *right, size_t right_count, size_t matches)
{
    Corpus array = {
        .common = {.ids = left, .count = left_count},
        .other = {.ids = right, .count = right_count}
    };
    PostingSkip *left_skip = posting_skip_build(left, left_count);
    PostingSkip *right_skip = posting_skip_build(right, right_count);
    if (left_skip == NULL || right_skip == NULL) {
        fputs("Cannot allocate benchmark skip levels.\n", stderr);
        exit(EXIT_FAILURE);
    }
    Corpus skip = array;
    skip.common.skip = left_skip;
    skip.other.skip = right_skip;

    QueryResult expected = query(&array);
    QueryResult actual = query(&skip);
    if (expected.count != matches || actual.count != matches ||
        (matches > 0 && memcmp(expected.ids, actual.ids, matches * sizeof(int)) != 0)) {
        fprintf(stderr, "%s: intersection mismatch\n", name);
        exit(EXIT_FAILURE);
    }
    free(expected.owned);
    free(actual.owned);

    double array_times[TRIALS];
    double skip_times[TRIALS];
    for (int trial = 0; trial < TRIALS; trial++) {
        if (trial % 2 == 0) {
            array_times[trial] = measure(&array);
            skip_times[trial] = measure(&skip);
        } else {
            skip_times[trial] = measure(&skip);
            array_times[trial] = measure(&array);
        }
    }
    qsort(array_times, TRIALS, sizeof array_times[0], compare_double);
    qsort(skip_times, TRIALS, sizeof skip_times[0], compare_double);
    double array_us = array_times[TRIALS / 2];
    double skip_us = skip_times[TRIALS / 2];

    printf("%s,%zu,%zu,%zu,%.3f,%.3f,%.1f%%,%zu,%zu\n",
           name, left_count, right_count, matches, array_us, skip_us,
           100.0 * (1.0 - skip_us / array_us), (left_count + right_count) * sizeof(int),
           posting_skip_bytes(left_skip) + posting_skip_bytes(right_skip));
    free(left_skip);
    free(right_skip);
}

int main(void)
{
    static int left[VERY_LONG_COUNT];
    static int right[LONG_COUNT];
    puts("case,left_count,right_count,matches,array_us,skip_us,time_saved,array_bytes,extra_skip_bytes");

    /* Sparse in the longer list, with shared IDs and both lists above 1000. */
    for (int i = 0; i < LONG_COUNT; i++) {
        left[i] = i * 2 + 2;
    }
    for (int i = 0; i < SHORT_COUNT; i++) {
        right[i] = i * 16 + 2;
    }
    benchmark("skewed", left, LONG_COUNT, right, SHORT_COUNT, SHORT_COUNT);

    for (int i = 0; i < VERY_LONG_COUNT; i++) {
        left[i] = i * 2 + 2;
    }
    for (int i = 0; i < SHORT_COUNT; i++) {
        right[i] = i * 128 + 2;
    }
    benchmark("very_skewed", left, VERY_LONG_COUNT, right, SHORT_COUNT, SHORT_COUNT);

    /* Two long lists with a small overlap between large contiguous runs. */
    for (int i = 0; i < LONG_COUNT; i++) {
        left[i] = i + 1;
        right[i] = i + LONG_COUNT - 128 + 1;
    }
    benchmark("clustered", left, LONG_COUNT, right, LONG_COUNT, 128);

    /* Alternating IDs leave no jumps to take. */
    for (int i = 0; i < LONG_COUNT; i++) {
        left[i] = i * 2 + 1;
        right[i] = i * 2 + 2;
    }
    benchmark("interleaved", left, LONG_COUNT, right, LONG_COUNT, 0);

    benchmark("identical", left, LONG_COUNT, left, LONG_COUNT, LONG_COUNT);
    return EXIT_SUCCESS;
}

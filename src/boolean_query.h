#ifndef BOOLEAN_QUERY_H
#define BOOLEAN_QUERY_H

#include "posting_skip.h"

#include <stddef.h>

enum { QUERY_BUFFER_SIZE = 4096 };

typedef struct {
    const int *ids;
    size_t count;
    int *owned;
    const PostingSkip *skip;
} QueryResult;

/* Single terms borrow the index's array. Free only result.owned. */
typedef QueryResult (*LookupTerm)(const void *index, const char *term);

const char *evaluate_query(const char *expression, const void *index,
                           LookupTerm lookup, int word_mode, QueryResult *result);

#endif

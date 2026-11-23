#ifndef BOOLEAN_QUERY_H
#define BOOLEAN_QUERY_H

#include <stddef.h>

typedef struct {
    const int *ids;
    size_t count;
    int *owned;
} QueryResult;

/* Single terms borrow the index's array. Free only result.owned. */
typedef QueryResult (*LookupTerm)(const void *index, const char *term);

const char *evaluate_query(const char *expression, const void *index,
                           LookupTerm lookup, QueryResult *result);

#endif

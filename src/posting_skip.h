#ifndef POSTING_SKIP_H
#define POSTING_SKIP_H

#include <stddef.h>

enum { POSTING_SKIP_MIN_COUNT = 1024 };

typedef struct PostingSkip PostingSkip;

/* The source IDs must be sorted, unique, and remain in place until free(). */
PostingSkip *posting_skip_build(const int *ids, size_t count);
size_t posting_skip_bytes(const PostingSkip *skip);

/* Return the first position at or after start whose ID is >= target, or count. */
size_t posting_skip_advance(const PostingSkip *skip, const int *ids,
                            size_t count, size_t start, int target);

#endif

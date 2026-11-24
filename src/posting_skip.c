#include "posting_skip.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

enum {
    STRIDE_BITS = 4,
    STRIDE = 1 << STRIDE_BITS,
    MAX_LEVELS = sizeof(size_t) * CHAR_BIT / STRIDE_BITS
};

typedef struct {
    unsigned shift;
    size_t offset;
} SkipLevel;

struct PostingSkip {
    size_t level_count;
    size_t anchor_count;
    SkipLevel levels[MAX_LEVELS];
    const int *anchors[];
};

PostingSkip *posting_skip_build(const int *ids, size_t count)
{
    size_t anchor_count = 0;
    size_t level_count = 0;
    for (size_t step = STRIDE; step < count; step *= STRIDE) {
        anchor_count += (count - 1) / step;
        level_count++;
        if (step > (count - 1) / STRIDE) {
            break;
        }
    }
    if (anchor_count > (SIZE_MAX - sizeof(PostingSkip)) / sizeof(const int *)) {
        return NULL;
    }

    PostingSkip *skip = malloc(sizeof *skip + anchor_count * sizeof *skip->anchors);
    if (skip == NULL) {
        return NULL;
    }
    skip->level_count = level_count;
    skip->anchor_count = anchor_count;

    size_t offset = 0;
    size_t step = STRIDE;
    for (size_t level = 0; level < level_count; level++) {
        skip->levels[level] = (SkipLevel){(unsigned)(level + 1) * STRIDE_BITS, offset};
        /* Documents arrive in ascending order, so each lane is appended in order. */
        for (size_t position = step; position < count; position += step) {
            skip->anchors[offset++] = ids + position;
            if (step >= count - position) {
                break;
            }
        }
        if (level + 1 < level_count) {
            step *= STRIDE;
        }
    }
    return skip;
}

size_t posting_skip_bytes(const PostingSkip *skip)
{
    return skip == NULL ? 0 : sizeof *skip + skip->anchor_count * sizeof *skip->anchors;
}

size_t posting_skip_advance(const PostingSkip *skip, const int *ids,
                            size_t count, size_t start, int target)
{
    if (start >= count) {
        return count;
    }
    if (ids[start] >= target) {
        return start;
    }

    size_t position = start;
    for (size_t level = skip->level_count; level > 0; level--) {
        SkipLevel lane = skip->levels[level - 1];
        size_t next = (position >> lane.shift) + 1;
        size_t last = (count - 1) >> lane.shift;
        while (next <= last) {
            const int *anchor = skip->anchors[lane.offset + next - 1];
            if (*anchor > target) {
                break;
            }
            position = next << lane.shift;
            if (*anchor == target) {
                return position;
            }
            next++;
        }
    }
    while (position < count && ids[position] < target) {
        position++;
    }
    return position;
}

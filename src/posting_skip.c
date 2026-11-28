#include "posting_skip.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

enum
{
    STRIDE_BITS = 4,
    STRIDE = 1 << STRIDE_BITS,
    MAX_LEVELS = sizeof(size_t) * CHAR_BIT / STRIDE_BITS
};

typedef struct
{
    unsigned shift;
    size_t offset;
} SkipLevel;

struct PostingSkip
{
    size_t level_count;
    size_t anchor_count;
    SkipLevel levels[MAX_LEVELS];
    const int *anchors[];
};

PostingSkip *posting_skip_build(const int *ids, size_t count) // 传入的是词所在章节的数组，和总数
{
    size_t anchor_count = 0;
    size_t level_count = 0;
    for (size_t step = STRIDE; step < count; step *= STRIDE) // 每一层的step都不同
    {
        anchor_count += (count - 1) / step; // 当前层（轮）的跳点个数，再累加
        level_count++;
        if (step > (count - 1) / STRIDE)
        {
            break;
        }
    }
    if (anchor_count > (SIZE_MAX - sizeof(PostingSkip)) / sizeof(const int *))
    {
        return NULL;
    }

    // sizeof *skip给 PostingSkip 的固定字段留空间，后面给anchors[] 留出anchor_count个指针位置
    PostingSkip *skip = malloc(sizeof *skip + anchor_count * sizeof *skip->anchors);
    if (skip == NULL)
    {
        return NULL;
    }
    skip->level_count = level_count;   // 有几层
    skip->anchor_count = anchor_count; // 所有层跳点总数

    size_t offset = 0;
    size_t step = STRIDE;
    for (size_t level = 0; level < level_count; level++)
    {
        // shift 用来表示步长。第一层 shift = 4，对应步长 2⁴ = 16。第二层 shift = 8，对应步长 2⁸ = 256
        skip->levels[level] = (SkipLevel){(unsigned)(level + 1) * STRIDE_BITS, offset};
        //  等价于
        /*SkipLevel current = {
             .shift = (unsigned)(level + 1) * STRIDE_BITS,
             .offset = offset
            };
         skip->levels[level] = current;*/

        /* Documents arrive in ascending order, so each lane is appended in order. */
        for (size_t position = step; position < count; position += step)
        { // 本来跳表数组anchors还是空的
            // offset决定写到哪一格，position决定这一格存谁的地址
            skip->anchors[offset++] = ids + position; // anchors[0] 存入了 ids[16] 的地址
            if (step >= count - position)
            {
                break;
            }
        }
        if (level + 1 < level_count)
        {
            step *= STRIDE; // 进入下一层
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
    if (start >= count)
    {
        return count;
    }
    if (ids[start] >= target)
    {
        return start;
    }

    size_t position = start;
    for (size_t level = skip->level_count; level > 0; level--)
    {
        SkipLevel lane = skip->levels[level - 1];
        size_t next = (position >> lane.shift) + 1;
        size_t last = (count - 1) >> lane.shift;
        while (next <= last)
        {
            const int *anchor = skip->anchors[lane.offset + next - 1];
            if (*anchor > target)
            {
                break;
            }
            position = next << lane.shift;
            if (*anchor == target)
            {
                return position;
            }
            next++;
        }
    }
    while (position < count && ids[position] < target)
    {
        position++;
    }
    return position;
}

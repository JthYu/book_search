#include "boolean_query.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
    END,
    TERM,
    LEFT,
    RIGHT,
    AND,
    OR,
    NOT,
    INVALID
} Token;

typedef struct
{
    const char *cursor;
    Token token;
    char term[QUERY_BUFFER_SIZE];
    int word_mode;
    const void *index;
    LookupTerm lookup;
    const char *error;
} Parser;

static int is_single_character(const char *start, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)start;
    unsigned char first = bytes[0];
    if ((length == 1 && first < 0x80) || (length == 2 && first >= 0xC2 && first <= 0xDF) || (length == 3 && first >= 0xE0 && first <= 0xEF) || (length == 4 && first >= 0xF0 && first <= 0xF4))
    {
        for (size_t i = 1; i < length; i++)
        {
            if ((bytes[i] & 0xC0) != 0x80)
            {
                return 0;
            }
        }
        if ((first == 0xE0 && bytes[1] < 0xA0) || (first == 0xED && bytes[1] >= 0xA0) || (first == 0xF0 && bytes[1] < 0x90) || (first == 0xF4 && bytes[1] >= 0x90))
        {
            return 0;
        }
        return 1;
    }
    return 0;
}

static void next_token(Parser *parser)
{
    const char *start = parser->cursor;
    // while每发现一个空白字符（比如空格或制表符），就用 start++ 往后挪一格
    while (isspace((unsigned char)*start))
    {
        start++;
    }

    if (*start == '\0') // 跳过空白后直接到了结尾
    {
        parser->cursor = start;
        parser->token = END;
        return;
    }

    if (*start == '(' || *start == ')')
    {
        parser->cursor = start + 1;
        parser->token = *start == '(' ? LEFT : RIGHT;
        return;
    }

    const char *end = start;
    while (*end != '\0' && !isspace((unsigned char)*end) && *end != '(' && *end != ')')
    {
        end++;
    }
    size_t length = (size_t)(end - start);
    parser->cursor = end;
    if (length == 3 && memcmp(start, "AND", 3) == 0) // 从start开始比较
    {
        parser->token = AND;
        return;
    }
    if (length == 2 && memcmp(start, "OR", 2) == 0)
    {
        parser->token = OR;
        return;
    }
    if (length == 3 && memcmp(start, "NOT", 3) == 0)
    {
        parser->token = NOT;
        return;
    }

    if (length >= sizeof parser->term) // 看是否越界
    {
        parser->token = INVALID;
        return;
    }
    if (!parser->word_mode && !is_single_character(start, length))
    {
        parser->token = INVALID;
        return;
    }
    memcpy(parser->term, start, length); // 从start开始复制长度为length的字符串到parser->term里
    parser->term[length] = '\0';
    parser->token = TERM;
}

static int precedence(Token token)
{
    if (token == AND || token == NOT)
    {
        return 2;
    }
    return token == OR ? 1 : 0;
}

static size_t intersect_arrays(QueryResult left, QueryResult right, int *ids)
{
    size_t i = 0;
    size_t j = 0;
    size_t n = 0;
    while (i < left.count && j < right.count)
    {
        if (left.ids[i] == right.ids[j])
        {
            ids[n++] = left.ids[i];
            i++;
            j++;
        }
        else if (left.ids[i] < right.ids[j])
        {
            i++;
        }
        else
        {
            j++;
        }
    }
    return n;
}

static size_t intersect_with_skips(QueryResult left, QueryResult right, int *ids)
{
    enum
    {
        LOOKAHEAD = 8
    };
    size_t i = 0;
    size_t j = 0;
    size_t n = 0;
    while (i < left.count && j < right.count)
    {
        if (left.ids[i] == right.ids[j])
        {
            ids[n++] = left.ids[i];
            i++;
            j++;
        }
        else if (left.ids[i] < right.ids[j])
        {
            i++;
            if (left.skip != NULL && left.count - i > LOOKAHEAD && left.ids[i + LOOKAHEAD] <= right.ids[j])
            {
                i = posting_skip_advance(left.skip, left.ids, left.count, i, right.ids[j]);
            }
        }
        else
        {
            j++;
            if (right.skip != NULL && right.count - j > LOOKAHEAD && right.ids[j + LOOKAHEAD] <= left.ids[i])
            {
                j = posting_skip_advance(right.skip, right.ids, right.count, j, left.ids[i]);
            }
        }
    }
    return n;
}

static int merge_lists(Token op, QueryResult left, QueryResult right, QueryResult *out)
{
    size_t capacity = left.count;
    if (op == OR) // 求并集，容量之和超过则必超过maxsize
    {
        if (right.count > SIZE_MAX - capacity)
        {
            return 0;
        }
        capacity += right.count;
    }
    else if (op == AND && right.count < capacity)
    {
        capacity = right.count; // 求交集，以小的那个为新容量
    }

    if (capacity == 0)
    {
        *out = (QueryResult){0}; // 没有匹配的章节
        return 1;                // 合并成功
    }
    if (capacity > SIZE_MAX / sizeof(int)) // 申请内存的总字节数不能超
    {
        return 0;
    }
    int *ids = malloc(capacity * sizeof *ids);

    if (ids == NULL)
    {
        return 0;
    }

    if (op == AND)
    {
        size_t count = left.skip != NULL || right.skip != NULL
                           ? intersect_with_skips(left, right, ids) // 只要有一份带跳表就选skip
                           : intersect_arrays(left, right, ids);
        *out = (QueryResult){.ids = ids, .count = count, .owned = ids}; // 分别是交集结果，结果（章节个数）
        return 1;
    }

    // 剩下处理OR和NOT
    size_t i = 0;
    size_t j = 0;
    size_t n = 0;
    while (i < left.count && j < right.count)
    {
        if (left.ids[i] == right.ids[j])
        {
            if (op != NOT)
            {
                ids[n++] = left.ids[i];
            }
            i++;
            j++;
        }
        else if (left.ids[i] < right.ids[j])
        {
            ids[n++] = left.ids[i++];
        }
        else
        {
            if (op == OR)
            {
                ids[n++] = right.ids[j];
            }
            j++;
        }
    }
    while (i < left.count)
    {
        ids[n++] = left.ids[i++];
    }
    if (op == OR)
    {
        while (j < right.count)
        {
            ids[n++] = right.ids[j++];
        }
    }
    *out = (QueryResult){.ids = ids, .count = n, .owned = ids};
    return 1;
}

static QueryResult parse_expression(Parser *parser, int minimum, int depth);

static QueryResult parse_operand(Parser *parser, int depth)
{
    if (parser->token == TERM)
    {
        QueryResult result = parser->lookup(parser->index, parser->term);
        next_token(parser); // 这样使parser->token=AND(假设是 ab AND bc)
        return result;      // 赋给parser_expression里的left
    }
    if (parser->token == LEFT && depth < 64)
    {
        next_token(parser);
        QueryResult result = parse_expression(parser, 1, depth + 1);
        if (parser->error == NULL)
        {
            if (parser->token == RIGHT)
            {
                next_token(parser);
            }
            else
            {
                parser->error = "expected a closing parenthesis";
            }
        }
        return result;
    }

    if (parser->token == LEFT)
    {
        parser->error = "parentheses nested too deeply";
    }
    else
    {
        parser->error = parser->word_mode
                            ? "expected a term or an opening parenthesis"
                            : "expected one UTF-8 character or an opening parenthesis";
    }
    return (QueryResult){0};
}

static QueryResult parse_expression(Parser *parser, int minimum, int depth)
{
    QueryResult left = parse_operand(parser, depth);
    while (parser->error == NULL && precedence(parser->token) >= minimum) // precedence运算优先级，AND和NOT高于OR
    {
        Token op = parser->token; // 把 AND 存到局部变量 op
        next_token(parser);       // 把 parser->token 更新为 TERM，把 "bc"存进 parser->term
        // 递归读取右边
        QueryResult right = parse_expression(parser, precedence(op) + 1, depth);
        QueryResult merged = {0};
        if (parser->error == NULL && !merge_lists(op, left, right, &merged))
        {
            parser->error = "could not allocate query results";
        }
        free(left.owned);
        free(right.owned);
        left = merged;
    }
    return left;
}

const char *evaluate_query(const char *expression, const void *index,
                           LookupTerm lookup, int word_mode, QueryResult *result)
{
    Parser parser = {
        .cursor = expression, .index = index, .lookup = lookup, .word_mode = word_mode};
    next_token(&parser);
    *result = parse_expression(&parser, 1, 0);
    if (parser.error == NULL && parser.token != END)
    {
        parser.error = "unexpected token after expression";
    }
    if (parser.error != NULL)
    {
        free(result->owned);
        *result = (QueryResult){0};
    }
    return parser.error;
}

#include "boolean_query.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum { END, TERM, LEFT, RIGHT, AND, OR, NOT, INVALID } Token;

typedef struct {
    const char *cursor;
    Token token;
    char term[5];
    const void *index;
    LookupTerm lookup;
    const char *error;
} Parser;

static void next_token(Parser *parser)
{
    const char *start = parser->cursor;
    while (isspace((unsigned char)*start)) {
        start++;
    }

    if (*start == '\0') {
        parser->cursor = start;
        parser->token = END;
        return;
    }

    if (*start == '(' || *start == ')') {
        parser->cursor = start + 1;
        parser->token = *start == '(' ? LEFT : RIGHT;
        return;
    }

    const char *end = start;
    while (*end != '\0' && !isspace((unsigned char)*end)
           && *end != '(' && *end != ')') {
        end++;
    }
    size_t length = (size_t)(end - start);
    parser->cursor = end;
    if (length == 3 && memcmp(start, "AND", 3) == 0) {
        parser->token = AND;
        return;
    }
    if (length == 2 && memcmp(start, "OR", 2) == 0) {
        parser->token = OR;
        return;
    }
    if (length == 3 && memcmp(start, "NOT", 3) == 0) {
        parser->token = NOT;
        return;
    }

    /* Accept exactly one UTF-8 code point as a stage 2 operand. */
    const unsigned char *bytes = (const unsigned char *)start;
    unsigned char first = bytes[0];
    if ((length == 1 && first < 0x80)
        || (length == 2 && first >= 0xC2 && first <= 0xDF)
        || (length == 3 && first >= 0xE0 && first <= 0xEF)
        || (length == 4 && first >= 0xF0 && first <= 0xF4)) {
        for (size_t i = 1; i < length; i++) {
            if ((bytes[i] & 0xC0) != 0x80) {
                parser->token = INVALID;
                return;
            }
        }
        if ((first == 0xE0 && bytes[1] < 0xA0)
            || (first == 0xED && bytes[1] >= 0xA0)
            || (first == 0xF0 && bytes[1] < 0x90)
            || (first == 0xF4 && bytes[1] >= 0x90)) {
            parser->token = INVALID;
            return;
        }
        memcpy(parser->term, start, length);
        parser->term[length] = '\0';
        parser->token = TERM;
        return;
    }
    parser->token = INVALID;
}

static int precedence(Token token)
{
    if (token == AND || token == NOT) {
        return 2;
    }
    return token == OR ? 1 : 0;
}

static int merge_lists(Token op, QueryResult left, QueryResult right,
                       QueryResult *out)
{
    size_t capacity = left.count;
    if (op == OR) {
        if (right.count > SIZE_MAX - capacity) {
            return 0;
        }
        capacity += right.count;
    } else if (op == AND && right.count < capacity) {
        capacity = right.count;
    }

    if (capacity == 0) {
        *out = (QueryResult){0};
        return 1;
    }
    if (capacity > SIZE_MAX / sizeof(int)) {
        return 0;
    }
    int *ids = malloc(capacity * sizeof *ids);
    if (ids == NULL) {
        return 0;
    }

    size_t i = 0;
    size_t j = 0;
    size_t n = 0;
    while (i < left.count && j < right.count) {
        if (left.ids[i] == right.ids[j]) {
            if (op != NOT) {
                ids[n++] = left.ids[i];
            }
            i++;
            j++;
        } else if (left.ids[i] < right.ids[j]) {
            if (op != AND) {
                ids[n++] = left.ids[i];
            }
            i++;
        } else {
            if (op == OR) {
                ids[n++] = right.ids[j];
            }
            j++;
        }
    }
    if (op != AND) {
        while (i < left.count) {
            ids[n++] = left.ids[i++];
        }
    }
    if (op == OR) {
        while (j < right.count) {
            ids[n++] = right.ids[j++];
        }
    }
    *out = (QueryResult){ids, n, ids};
    return 1;
}

static QueryResult parse_expression(Parser *parser, int minimum, int depth);

static QueryResult parse_operand(Parser *parser, int depth)
{
    if (parser->token == TERM) {
        QueryResult result = parser->lookup(parser->index, parser->term);
        next_token(parser);
        return result;
    }
    if (parser->token == LEFT && depth < 64) {
        next_token(parser);
        QueryResult result = parse_expression(parser, 1, depth + 1);
        if (parser->error == NULL) {
            if (parser->token == RIGHT) {
                next_token(parser);
            } else {
                parser->error = "expected a closing parenthesis";
            }
        }
        return result;
    }

    if (parser->token == LEFT) {
        parser->error = "parentheses nested too deeply";
    } else {
        parser->error = "expected one UTF-8 character or an opening parenthesis";
    }
    return (QueryResult){0};
}

static QueryResult parse_expression(Parser *parser, int minimum, int depth)
{
    QueryResult left = parse_operand(parser, depth);
    while (parser->error == NULL && precedence(parser->token) >= minimum) {
        Token op = parser->token;
        next_token(parser);
        QueryResult right = parse_expression(parser, precedence(op) + 1, depth);
        QueryResult merged = {0};
        if (parser->error == NULL && !merge_lists(op, left, right, &merged)) {
            parser->error = "could not allocate query results";
        }
        free(left.owned);
        free(right.owned);
        left = merged;
    }
    return left;
}

const char *evaluate_query(const char *expression, const void *index,
                           LookupTerm lookup, QueryResult *result)
{
    Parser parser = {.cursor = expression, .index = index, .lookup = lookup};
    next_token(&parser);
    *result = parse_expression(&parser, 1, 0);
    if (parser.error == NULL && parser.token != END) {
        parser.error = "unexpected token after expression";
    }
    if (parser.error != NULL) {
        free(result->owned);
        *result = (QueryResult){0};
    }
    return parser.error;
}

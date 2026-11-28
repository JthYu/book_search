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
    int *postings;
    size_t count;
    size_t capacity;
    struct Entry *next;
    PostingSkip *skip;
    char token[];
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
    {                                                                // 计算哈希值
        hash = (hash * 31 + (unsigned char)token[i]) % BUCKET_COUNT; // BUCKET_COUNT = 4093
    }
    return hash;
}

static Entry *find_entry(const Index *index, size_t bucket, const char *token)
{
    // 不同的词可能算出同一个下标，同一个桶里可以有多个节点，用 next 串成链表
    // bucket是桶的编号而不是桶内节点的编号
    for (Entry *entry = index->buckets[bucket]; entry != NULL; entry = entry->next)
    {
        if (strcmp(entry->token, token) == 0)
        {
            return entry;
        }
    }
    return NULL; // 这个词从没出现过，函数就返回NULL
}

static int add_token(Index *index, const char *token, int doc_id)
{
    size_t bucket = bucket_for(token);               // 根据词的内容计算出下标，token就是那个line，比如“宝玉”
    Entry *entry = find_entry(index, bucket, token); // 不同的词可能算出同一个下标

    // 如果这个词（token从没出现过，不在buckets[bucket]里
    // 给新词的节点申请内存
    if (entry == NULL)
    {
        size_t token_size = strlen(token) + 1; // 算上结尾的‘\0’
        // sizeof *entry是 Entry 节点固定字段所需的字节数，额外的字节留给节点末尾的char token[]
        entry = malloc(sizeof *entry + token_size);
        if (entry == NULL) // 申请内存失败
        {
            return 0;
        }

        // 把新节点接到桶的最前面
        *entry = (Entry){0}; // ？初始化。构造一个 Entry 初始值。整数成员初始化为 0，指针成员初始化为 NULL
        strcpy(entry->token, token);
        entry->next = index->buckets[bucket];
        index->buckets[bucket] = entry;
    }

    /* Chapters are indexed in increasing document ID order. */
    // postings 存这个词出现过的章节编号，count是已经存了几个
    if (entry->count > 0 && entry->postings[entry->count - 1] == doc_id)
    {
        return 1;
    }
    // capacity一开始都是0，然后不断扩容
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
    entry->postings[entry->count] = doc_id; // 写入章节编号
    entry->count++;
    return 1;
}

static void free_index(Index *index)
{
    for (size_t bucket = 0; bucket < BUCKET_COUNT; bucket++) // 逐个桶
    {
        Entry *entry = index->buckets[bucket]; // 逐个节点
        while (entry != NULL)
        {
            Entry *next = entry->next;
            free(entry->skip);
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

/* Word files contain one token per line, generated by tools/preprocess.py. */
static int index_words(Index *index, const char *path, int doc_id)
{
    // 以二进制读取模式打开文件（read binary）
    FILE *file = fopen(path, "rb"); // 可以把 file 理解成接下来读这个文件要用的指针
    if (file == NULL)
    {
        perror(path);
        return 0;
    }

    char line[QUERY_BUFFER_SIZE];
    int ok = 1;

    // fgets() 读文件，把“宝玉\n”存进line，再补上字符串结束符\0。
    // 读取成功时，fgets()返回line的首地址
    while (fgets(line, sizeof line, file) != NULL)
    {
        size_t length = strcspn(line, "\r\n"); // 从头往后找，遇到第一个 \r 或 \n 就停。计算它前面有几个字节（一个汉字占3字节）
        if (length == sizeof line - 1)         // 读满了
        {
            fprintf(stderr, "%s contains an overlong token.\n", path);
            ok = 0;
            break;
        }
        line[length] = '\0'; // 把原来的\n改成\0

        if (length > 0 && !add_token(index, line, doc_id))
        {
            fprintf(stderr, "Cannot allocate index memory for %s.\n", path);
            ok = 0;
            break;
        }
    }
    if (ferror(file))
    {
        perror(path);
        ok = 0;
    }
    fclose(file);
    return ok;
}

// 建立索引表模块
static int build_index(Index *index, const char *directory, int chapter_count,
                       int word_mode)
{
    for (int i = 0; i < chapter_count; i++)
    {
        int doc_id = i + 1; // 章节编号
        char path[1024];    // 路径字符串
        // snprintf返回路径需要的字符数，这里是 20
        int length = snprintf(path, sizeof path, "%s/%03d.txt", directory, doc_id); // path里是 "build/tokens/001.txt"
        if (length < 0 || (size_t)length >= sizeof path)
        {
            fprintf(stderr, "Chapter path is too long.\n");
            return 0;
        }

        // word_mode非零就调用 index_words()，等于 0 就调用 index_chapter()
        int ok = word_mode ? index_words(index, path, doc_id)
                           : index_chapter(index, path, doc_id);
        if (!ok)
        {
            return 0;
        }
    }
    return 1;
}

static int build_skip_levels(Index *index)
{
    // 外层 for 用 bucket++ 逐个访问桶，内层 for 用 entry->next 走完当前桶里的链表
    for (size_t bucket = 0; bucket < BUCKET_COUNT; bucket++)
    {
        for (Entry *entry = index->buckets[bucket]; entry != NULL; entry = entry->next)
        {
            if (entry->count >= POSTING_SKIP_MIN_COUNT) // 只有某个词的章节列表足够长（1024）才建立跳表
            {
                entry->skip = posting_skip_build(entry->postings, entry->count); // 返回跳表的地址
                if (entry->skip == NULL)
                {
                    fprintf(stderr, "Cannot allocate skip levels.\n");
                    return 0;
                }
            }
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
    return (QueryResult){
        .ids = entry->postings, .count = entry->count, .skip = entry->skip};
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

static void print_index_stats(const Index *index, int skip_mode)
{
    size_t terms = 0;
    size_t postings = 0;
    size_t logical_bytes = 0;
    size_t allocated_bytes = sizeof *index;
    size_t skip_bytes = 0;
    size_t skip_lists = 0;
    for (size_t bucket = 0; bucket < BUCKET_COUNT; bucket++)
    {
        for (const Entry *entry = index->buckets[bucket]; entry; entry = entry->next)
        {
            size_t token_bytes = strlen(entry->token) + 1;
            // 统计跳表占用的空间
            if (entry->skip != NULL)
            {
                skip_lists++;
                skip_bytes += posting_skip_bytes(entry->skip);
            }
            // 数词条和章节记录
            terms++;
            postings += entry->count;
            // 统计索引占用字节数
            logical_bytes += token_bytes + entry->count * sizeof(int);
            allocated_bytes += sizeof *entry + token_bytes + entry->capacity * sizeof(int);
        }
    }
    printf("Index: %zu terms, %zu postings, %zu logical bytes, %zu allocated bytes\n",
           terms, postings, logical_bytes, allocated_bytes + skip_bytes);
    printf("Mode: %s, %zu skip lists, %zu extra skip bytes\n",
           skip_mode ? "skip" : "array", skip_lists, skip_bytes);
}

// 查询模块
static int run_queries(const Index *index, const char *path, int word_mode)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        perror(path);
        return 0;
    }

    char line[QUERY_BUFFER_SIZE];
    size_t line_number = 0;
    while (fgets(line, sizeof line, file) != NULL)
    {
        line_number++;
        char *token = line + strspn(line, " \t");
        // 检查第一行有没有 UTF-8 BOM
        if (line_number == 1 && strncmp(token, "\xef\xbb\xbf", 3) == 0)
        {
            token += 3;
            token += strspn(token, " \t");
        }
        // 从 token 指向的位置开始数，遇到 \r 或 \n 就停
        size_t length = strcspn(token, "\r\n");
        // 看是否过长
        if (strlen(line) == sizeof line - 1 && strchr(line, '\n') == NULL)
        {
            fprintf(stderr, "%s line %zu is too long.\n", path, line_number);
            fclose(file);
            return 0;
        }
        // 去掉行尾空格和换行符
        while (length > 0 && (token[length - 1] == ' ' || token[length - 1] == '\t'))
        {
            length--;
        }
        token[length] = '\0';
        if (length == 0)
        {
            continue;
        }
        QueryResult result; // 查询结果
        double started = now_ms();
        const char *error = evaluate_query(token, index, lookup_term, word_mode, &result);
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

// eg:指令为.\book_search.exe build/tokens 120 tests/stage3_queries.txt --words --skip
int main(int argc, char *argv[]) // 把终端这条命令按空格分成几块：运行的程序，按章编号的文件（目录），总章数，查询文件，mode开关，skip开关
{
    if (argc < 4 || argc > 6) // 保证终端的命令有[4,6]块
    {
        fprintf(stderr, "Usage: book_search <chapter-dir> <chapter-count> <query-file> [--words] [--skip]\n");
        return EXIT_FAILURE;
    }

    // word和skip两个开关默认为关着
    int word_mode = 0;
    int skip_mode = 0;
    for (int arg = 4; arg < argc; arg++)
    { // 看是否打开两个开关
        if (strcmp(argv[arg], "--words") == 0 && !word_mode)
        {
            word_mode = 1;
        }
        else if (strcmp(argv[arg], "--skip") == 0 && !skip_mode)
        {
            skip_mode = 1;
        }
        else
        {
            fprintf(stderr, "Unknown or repeated option: %s\n", argv[arg]);
            return EXIT_FAILURE;
        }
    }

    errno = 0;
    char *end;
    // strtol 还会告诉读到哪里停了。`end` 是个字符指针，传 &end 是为了让 strtol 把那个位置写进 end。
    // 所以 *end 就是「停下来时看到的那个字符」
    long count = strtol(argv[2], &end, 10); // 把字符串“120”转换为数字count=120
    if (errno == ERANGE || *end != '\0' || count < 1 || count > INT_MAX)
    {
        fprintf(stderr, "Chapter count must be a positive integer.\n");
        return EXIT_FAILURE;
    }

    Index index = {0};
    double started = now_ms();
    // 前半部分：去 build/tokens（2） 读 120 回（3），按词建立索引（4），把结果存到 index 里（1）
    // 后半：skip_mode是1，才调用 build_skip_levels()
    // 建索引失败，或skip_mode=1但建跳表失败，才进入if
    if (!build_index(&index, argv[1], (int)count, word_mode) || (skip_mode && !build_skip_levels(&index)))
    {
        free_index(&index);
        return EXIT_FAILURE;
    }
    printf("Indexed %ld chapters in %.3f ms\n", count, now_ms() - started);

    print_index_stats(&index, skip_mode);
    int ok = run_queries(&index, argv[3], word_mode);
    free_index(&index);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

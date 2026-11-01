// Received-line queue (F-R24): order, capacity, over-length markers
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "linequeue.h"

static linequeue_t q;

static void expect_front(const char *line)
{
    size_t len = 0;
    const char *got = linequeue_front(&q, &len);
    if (line == NULL) {
        assert(got == NULL);
    } else {
        assert(got != NULL && len == strlen(line) && strcmp(got, line) == 0);
    }
    linequeue_drop(&q);
}

static void test_order_and_capacity(void)
{
    char line[16];

    linequeue_reset(&q);
    assert(linequeue_empty(&q));

    // Wraps around the slots several times
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < LINEQUEUE_SLOTS; i++) {
            assert(!linequeue_full(&q));
            snprintf(line, sizeof line, "l%d.%d", round, i);
            if (i == 3)
                linequeue_push_too_long(&q);
            else
                linequeue_push(&q, line, strlen(line));
        }
        assert(linequeue_full(&q));
        for (int i = 0; i < LINEQUEUE_SLOTS; i++) {
            snprintf(line, sizeof line, "l%d.%d", round, i);
            expect_front(i == 3 ? NULL : line);
        }
        assert(linequeue_empty(&q));
    }
}

static void test_long_and_nul(void)
{
    char big[LINEBUF_MAX];

    linequeue_reset(&q);
    memset(big, 'x', sizeof big - 1);
    linequeue_push(&q, big, sizeof big - 1);
    // An embedded NUL is kept, with the length
    linequeue_push(&q, "a\0b", 3);

    size_t len;
    const char *got = linequeue_front(&q, &len);
    assert(len == sizeof big - 1 && memcmp(got, big, len) == 0 && got[len] == '\0');
    linequeue_drop(&q);
    got = linequeue_front(&q, &len);
    assert(len == 3 && memcmp(got, "a\0b", 3) == 0);
    linequeue_drop(&q);

    // Reset empties it
    linequeue_push(&q, "x", 1);
    linequeue_reset(&q);
    assert(linequeue_empty(&q));
}

int main(void)
{
    test_order_and_capacity();
    test_long_and_nul();
    printf("test_linequeue: ok\n");
    return 0;
}

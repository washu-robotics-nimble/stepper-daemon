// Host link line assembler (F-R31)
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "linebuf.h"

// Feed a string; return the result of its last byte
static linebuf_result_t feed(linebuf_t *lb, const char *s, size_t n)
{
    linebuf_result_t r = LINEBUF_NONE;
    for (size_t i = 0; i < n; i++)
        r = linebuf_put(lb, s[i]);
    return r;
}

static void feed_repeat(linebuf_t *lb, char c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        assert(linebuf_put(lb, c) == LINEBUF_NONE);
}

static void test_simple_lines(void)
{
    linebuf_t lb;
    linebuf_reset(&lb);

    assert(feed(&lb, "abc\n", 4) == LINEBUF_LINE);
    assert(lb.len == 3 && strcmp(lb.buf, "abc") == 0);

    // Next line starts fresh
    assert(feed(&lb, "de\n", 3) == LINEBUF_LINE);
    assert(lb.len == 2 && strcmp(lb.buf, "de") == 0);

    // Empty line
    assert(linebuf_put(&lb, '\n') == LINEBUF_LINE);
    assert(lb.len == 0 && lb.buf[0] == '\0');

    // '\r' is passed through; the JSON parser treats it as whitespace
    assert(feed(&lb, "x\r\n", 3) == LINEBUF_LINE);
    assert(lb.len == 2 && strcmp(lb.buf, "x\r") == 0);
}

static void test_length_limit(void)
{
    linebuf_t lb;
    linebuf_reset(&lb);

    // Longest allowed line: LINEBUF_MAX bytes including '\n'
    feed_repeat(&lb, 'a', LINEBUF_MAX - 1);
    assert(linebuf_put(&lb, '\n') == LINEBUF_LINE);
    assert(lb.len == LINEBUF_MAX - 1);

    // One byte more is over-length, reported once at its '\n'
    feed_repeat(&lb, 'b', LINEBUF_MAX);
    feed_repeat(&lb, 'b', 5000);
    assert(linebuf_put(&lb, '\n') == LINEBUF_TOO_LONG);

    // The next line is unaffected
    assert(feed(&lb, "ok\n", 3) == LINEBUF_LINE);
    assert(strcmp(lb.buf, "ok") == 0);
}

static void test_reset_discards_partial(void)
{
    linebuf_t lb;
    linebuf_reset(&lb);

    feed(&lb, "{\"seq\":", 7);
    linebuf_reset(&lb);
    assert(feed(&lb, "new\n", 4) == LINEBUF_LINE);
    assert(strcmp(lb.buf, "new") == 0);

    // Reset also clears an over-length state
    feed_repeat(&lb, 'c', LINEBUF_MAX + 10);
    linebuf_reset(&lb);
    assert(feed(&lb, "z\n", 2) == LINEBUF_LINE);
}

int main(void)
{
    test_simple_lines();
    test_length_limit();
    test_reset_discards_partial();
    printf("test_linebuf: ok\n");
    return 0;
}

// Host unit tests for the lease arbiter (src/daemon/lease.c)

#include <assert.h>
#include <stdio.h>
#include "lease.h"

#define A 1
#define B 2

static void test_take_release(void)
{
    lease_t l;
    lease_init(&l, 5000);

    assert(lease_holder(&l, 0) == 0);
    assert(lease_take(&l, A, 0));
    assert(lease_holder(&l, 1) == A);
    // Only one holder (D-R18)
    assert(!lease_take(&l, B, 1));
    assert(!lease_renew(&l, B, 1));
    // Taking it again renews it
    assert(lease_take(&l, A, 4000));
    assert(lease_holder(&l, 8999) == A);
    assert(lease_holder(&l, 9000) == 0);

    // Release frees it at once; a release by another client does nothing
    assert(lease_take(&l, A, 10000));
    lease_release(&l, B);
    assert(lease_holder(&l, 10001) == A);
    lease_release(&l, A);
    assert(lease_holder(&l, 10001) == 0);
    assert(lease_take(&l, B, 10002));
    // Disconnect is a release (D-R18)
    lease_release(&l, B);
    assert(lease_take(&l, A, 10003));
}

static void test_expiry_and_renew(void)
{
    lease_t l;
    lease_init(&l, 5000);

    assert(lease_take(&l, A, 1000));
    assert(lease_renew(&l, A, 5999));       // expires at 6000; renewed to 10999
    assert(lease_holder(&l, 10998) == A);
    assert(!lease_renew(&l, A, 10999));     // expired: gone
    assert(lease_holder(&l, 10999) == 0);
    assert(lease_take(&l, B, 11000));
    assert(!lease_take(&l, A, 11000));
}

static void test_classes(void)
{
    lease_t l;
    lease_init(&l, 5000);

    // Without a holder: everything but writes (D-R16, D-R17, D-R18)
    assert(lease_check(&l, CLASS_READ, A, 0));
    assert(lease_check(&l, CLASS_SESSION, A, 0));
    assert(lease_check(&l, CLASS_EMERGENCY, A, 0));
    assert(!lease_check(&l, CLASS_WRITE, A, 0));

    assert(lease_take(&l, A, 0));
    assert(lease_check(&l, CLASS_WRITE, A, 1));
    assert(!lease_check(&l, CLASS_WRITE, B, 1));
    assert(lease_check(&l, CLASS_READ, B, 1));
    assert(lease_check(&l, CLASS_EMERGENCY, B, 1));
    assert(lease_check(&l, CLASS_SESSION, B, 1));

    // No command renews the lease, writes included: only lease and renew
    // do (D-R18)
    assert(lease_check(&l, CLASS_WRITE, A, 4000));
    assert(lease_check(&l, CLASS_READ, A, 4500));
    assert(lease_holder(&l, 4999) == A);
    assert(lease_holder(&l, 5000) == 0);
    assert(!lease_check(&l, CLASS_WRITE, A, 5000));
}

int main(void)
{
    test_take_release();
    test_expiry_and_renew();
    test_classes();
    printf("test_daemon_lease: ok\n");
    return 0;
}

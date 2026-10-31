// Smoke test for the vendored cJSON: parses a host-link frame and checks
// the integer handling rule from firmware impl (read via valuedouble).
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

int main(void)
{
    const char *frame = "{\"seq\":4294967295,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":-400}";
    cJSON *root = cJSON_Parse(frame);
    assert(root != NULL);

    // seq is uint32: valueint clamps at INT_MAX, valuedouble is exact
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    assert(cJSON_IsNumber(seq));
    assert((unsigned long)seq->valuedouble == 4294967295UL);

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    assert(cJSON_IsString(cmd) && strcmp(cmd->valuestring, "move") == 0);

    cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
    assert(cJSON_IsNumber(steps) && (int)steps->valuedouble == -400);

    // Unknown fields are simply absent from lookups (F-R31)
    assert(cJSON_GetObjectItemCaseSensitive(root, "nope") == NULL);

    // Malformed frame is rejected
    assert(cJSON_Parse("{\"seq\":1,") == NULL);

    cJSON_Delete(root);
    printf("test_cjson: ok\n");
    return 0;
}

#include "allocator.h"
#include "array.h"
#include "asset_catalog.h"
#include "atomics.inl"
#include "basic.h"
#include "frame_allocator.h"
#include "hash.h"
#include "linear_allocator.h"
#include "log.h"
#include "math.h"
#include "murmur_hash64.h"
#include "os.h"
#include "random.h"
#include "string_util.h"
#include "rect.h"
#include "unicode.h"

#include <stdio.h>

typedef struct Test_Runner {
    uint32_t passed;
    uint32_t failed;
} Test_Runner;

void test(Test_Runner *tr, bool condition, const char *msg, const char *file, uint32_t line)
{
    if (condition)
    {
        tr->passed++;
    }
    else
    {
        tr->failed++;
    }
    printf("Test %s: %s [%s:%i]\n", condition ? "OK" : "FAILED", msg, file, line);
}

#define TEST(cond, tr) test(tr, cond, #cond, __FILE__, __LINE__)

typedef struct Test_Asset {
    int value;
} Test_Asset;

static Test_Asset *placeholder_asset = &(Test_Asset) {
    .value = 999,
};

static Test_Asset *fallback_asset = &(Test_Asset) {
    .value = 404,
};

typedef struct Test_Asset_Descriptor {
    int value;
} Test_Asset_Descriptor;

bool test_asset_load(const void *data, uint64_t size, Test_Asset_Descriptor *descriptor)
{
    log_info("Load called on thread id = %i\n", os_thread_id());
    log_info("Got file data: %s", (const char *)data);
    descriptor->value = 200;
    return true;
}

void test_asset_load_complete(const Test_Asset_Descriptor *descriptor, Test_Asset *asset)
{
    asset->value = descriptor->value;
}

void *test_placeholder_asset(Asset_Catalog *catalog)
{
    return placeholder_asset;
}

void *test_fallback_asset(Asset_Catalog *catalog)
{
    return fallback_asset;
}

void test_asset_catalog(Test_Runner *tr)
{
    log_info("Main thread id: %i\n", os_thread_id());

    Asset_Catalog *catalog = make_asset_catalog(4096, &(Asset_Catalog_Interface) {
        .callbacks = {
            .asset_load = test_asset_load,
            .asset_load_complete = test_asset_load_complete,
        },
        .asset_size = sizeof(Test_Asset),
        .descriptor_size = sizeof(Test_Asset_Descriptor),
        .placeholder_asset = placeholder_asset,
        .fallback_asset = fallback_asset,
    });

    Test_Asset *my_asset = find_or_load_asset(catalog, "test.txt", false);
    TEST(my_asset->value == 200, tr);

    Test_Asset *my_invalid_asset = find_or_load_asset(catalog, "invalid.txt", false);
    TEST(my_invalid_asset->value == fallback_asset->value, tr);

    Test_Asset *my_async_asset = find_or_load_asset(catalog, "test2.txt", true);
    TEST(my_async_asset->value == placeholder_asset->value, tr);

    Test_Asset *my_async_invalid_asset = find_or_load_asset(catalog, "invalid2.txt", true);
    TEST(my_async_invalid_asset->value == placeholder_asset->value, tr);

    os_sleep(1);
    poll_async_assets();

    TEST(my_async_asset->value == 200, tr);
    TEST(my_async_invalid_asset->value == fallback_asset->value, tr);

    free_asset_catalog(catalog);
    TEST(total_bytes_allocated() == 0, tr);
}

int main(int argc, char **argv)
{
    register_log_callback(log_stdout);

    Test_Runner *tr = &(Test_Runner) { 0 };

    test_asset_catalog(tr);

    printf("---\n%i tests passed and %i failed\n", tr->passed, tr->failed);
    return tr->failed > 0 ? 1 : 0;
}
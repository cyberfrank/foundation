#include "asset_catalog.h"
#include "hash.h"
#include "allocator.h"
#include "murmur_hash64.h"
#include "array.h"
#include "os.h"
#include "log.h"

struct Asset_Catalog {
    void *data;
    uint64_t asset_size;
    uint64_t descriptor_size;
    uint64_t size;
    uint64_t capacity;
    Hash name_to_slot;
    Asset_Catalog_Callbacks callbacks;
    Allocator data_allocator;
    Allocator hash_allocator;
    bool no_descriptor;
    const void *placeholder_asset;
    const void *fallback_asset;
};

enum {
    ASSET_STATE_PENDING,
    ASSET_STATE_VALID,
    ASSET_STATE_FAILED,
    ASSET_STATE_HANDLED,
};

typedef struct Pending_Asset {
    Asset_Catalog *catalog;
    uint64_t hash_name;
    Allocator *allocator;
    void *descriptor;
    void *data;
    uint64_t size;
    uint32_t asset_state;
} Pending_Asset;

struct Asset_Loader {
    bool initialized;
    Allocator *allocator;
    Thread_Handle thread_handle;
    Semaphore_Handle pending_assets_counter;
    Semaphore_Handle loaded_assets_counter;
    Critical_Section pending_assets_cs;
    Pending_Asset *pending_assets;
} *asset_loader = &(struct Asset_Loader) {
    .initialized = false,
};

static void asset_loader_thread_entry_point(struct Asset_Loader *ctx)
{
    while (true)
    {
        os_semaphore_wait(ctx->pending_assets_counter);

        Pending_Asset asset;
        uint64_t asset_index;

        os_enter_critical_section(&ctx->pending_assets_cs);
        {
            asset_index = array_size(ctx->pending_assets) - 1;
            asset = ctx->pending_assets[asset_index];
        }
        os_leave_critical_section(&ctx->pending_assets_cs);

        Asset_Catalog_Callbacks *callbacks = &asset.catalog->callbacks;
        bool success = asset.data != 0 && callbacks->asset_load(asset.data, asset.size, asset.descriptor);

        os_enter_critical_section(&ctx->pending_assets_cs);
        {
            Pending_Asset *pending = &ctx->pending_assets[asset_index];
            pending->asset_state = success ? ASSET_STATE_VALID : ASSET_STATE_FAILED;
            pending->descriptor = asset.descriptor;
        }
        os_leave_critical_section(&ctx->pending_assets_cs);

        os_semaphore_add(ctx->loaded_assets_counter, 1);
    }
}

static void private__ensure_buffer_capacity(Asset_Catalog *catalog, uint64_t items_to_add)
{
    uint64_t to_at_least = catalog->size + items_to_add;
    if (to_at_least > catalog->capacity) 
    {
        Allocator *a = &catalog->data_allocator;
        const uint64_t min_capacity = catalog->capacity ? catalog->capacity * 2 : 16;
        const uint64_t new_capacity = min_capacity > to_at_least ? min_capacity : to_at_least;
        const uint64_t bytes_before = catalog->data ? catalog->asset_size * catalog->capacity : 0;
        const uint64_t bytes_after = catalog->asset_size * new_capacity;
        catalog->data = a->allocate_func(a, catalog->data, bytes_before, bytes_after, __FILE__, __LINE__);
        catalog->capacity = new_capacity;
    }
}

Asset_Catalog *make_asset_catalog(uint64_t reserve_count, Asset_Catalog_Interface *i)
{
    if (!asset_loader->initialized)
    {
        asset_loader->allocator = system_allocator;
        asset_loader->thread_handle = os_create_thread(asset_loader_thread_entry_point, asset_loader, MB(4));
        asset_loader->pending_assets_counter = os_create_semaphore(0);
        asset_loader->loaded_assets_counter = os_create_semaphore(0);
        os_create_critical_section(&asset_loader->pending_assets_cs);
        asset_loader->initialized = true;
    }

    Asset_Catalog *c = c_alloc(system_allocator, sizeof(*c));
    const uint64_t reserve_size = ALIGN_SIZE(i->asset_size * reserve_count, PAGE_SIZE);
    c->data_allocator = allocator_create_fixed_vm(reserve_size);
    c->hash_allocator = *system_allocator;
    c->asset_size = i->asset_size;
    c->descriptor_size = i->no_descriptor ? i->asset_size : i->descriptor_size;
    c->size = 0;
    c->capacity = 0;
    c->data = 0;
    c->name_to_slot = (Hash) { 0 };
    c->callbacks = i->callbacks;
    c->no_descriptor = i->no_descriptor;

    set_placeholder_asset(c, i->placeholder_asset);
    set_fallback_asset(c, i->fallback_asset);

    return c;
}

void set_placeholder_asset(Asset_Catalog *catalog, const void *asset)
{
    catalog->placeholder_asset = asset;
}

void set_fallback_asset(Asset_Catalog *catalog, const void *asset)
{
    catalog->fallback_asset = asset;
}

void free_asset_catalog(Asset_Catalog *catalog)
{
    hash_free(&catalog->name_to_slot, &catalog->hash_allocator);

    Asset_Catalog_Callbacks *callbacks = &catalog->callbacks;
    if (callbacks->asset_free != 0) 
    {
        for (uint32_t i = 0; i < catalog->size; ++i) 
        {
            void *asset = ((uint8_t *)catalog->data + catalog->asset_size * i);
            if (asset != 0)
            {
                callbacks->asset_free(asset);
            }
        }
    }

    c_free(&catalog->data_allocator, catalog->data, catalog->asset_size * catalog->capacity);
    c_free(system_allocator, catalog, sizeof(*catalog));
}

void *make_asset(Asset_Catalog *catalog, uint64_t hash_name)
{
    private__ensure_buffer_capacity(catalog, 1);

    void *asset = ((uint8_t *)catalog->data + catalog->asset_size * catalog->size);
    hash_add(&catalog->name_to_slot, hash_name, catalog->size, &catalog->hash_allocator);
    ++catalog->size;

    memset(asset, 0, catalog->asset_size);

    return asset;
}

void *find_or_load_asset(Asset_Catalog *catalog, const char *path, bool load_async)
{
    const uint64_t hash_name = murmur_hash64a_string(path);

    void *found_asset = find_asset(catalog, hash_name);
    if (found_asset != 0)
    {
        return found_asset;
    }

    log_info("Begin attempt to load asset '%s' (%zu) [async=%i]", path, hash_name, (int)load_async);
    
    Asset_Catalog_Callbacks *callbacks = &catalog->callbacks;
    if (callbacks->asset_load == 0 || (callbacks->asset_load_complete == 0 && !catalog->no_descriptor))
    {
        log_error("Cannot load asset '%s' due to missing load callback", path);
        return 0;
    }

    void *asset = make_asset(catalog, hash_name);
    Allocator *asset_allocator = system_allocator;

    if (load_async)
    {
        uint64_t size = 0;
        void *data = os_read_entire_file(path, &size, asset_allocator);
        
        Pending_Asset pending_asset;
        pending_asset.catalog = catalog;
        pending_asset.hash_name = hash_name;
        pending_asset.allocator = asset_allocator;
        pending_asset.data = data;
        pending_asset.size = size;
        pending_asset.asset_state = ASSET_STATE_PENDING;
        pending_asset.descriptor = c_alloc(asset_allocator, catalog->descriptor_size);

        os_enter_critical_section(&asset_loader->pending_assets_cs);
        {
            array_push(asset_loader->pending_assets, pending_asset, asset_loader->allocator);
        }
        os_leave_critical_section(&asset_loader->pending_assets_cs);

        if (catalog->placeholder_asset != 0)
        {
            memcpy(asset, catalog->placeholder_asset, catalog->asset_size);
        }

        os_semaphore_add(asset_loader->pending_assets_counter, 1);
    }
    else
    {  
        uint64_t size = 0;
        void *data = os_read_entire_file(path, &size, asset_allocator);
        void *descriptor = c_alloc(asset_allocator, catalog->descriptor_size);
        bool success = data != 0 && callbacks->asset_load(data, size, descriptor);
        if (success)
        {
            log_info("Loaded asset '%s' successfully", path);
            if (catalog->no_descriptor)
            {
                memcpy(asset, descriptor, catalog->descriptor_size);
            }
            else
            {
                callbacks->asset_load_complete(descriptor, asset);
            }
        }
        else
        {
            log_error("Failed to load asset '%s'", path);
            if (catalog->fallback_asset != 0)
            {
                memcpy(asset, catalog->fallback_asset, catalog->asset_size);
            }
        }
        c_free(asset_allocator, descriptor, catalog->descriptor_size);
        c_free(asset_allocator, data, size);
    }
    return asset;
}

void *find_or_make_asset(Asset_Catalog *catalog, uint64_t hash_name)
{
    void *found_asset = find_asset(catalog, hash_name);
    if (found_asset != 0)
    {
        return found_asset;
    }
    return make_asset(catalog, hash_name);
}

void *find_asset_by_name(Asset_Catalog *catalog, const char *path)
{
    const uint64_t hash_name = murmur_hash64a_string(path);
    return find_asset(catalog, hash_name);
}

void *find_asset(Asset_Catalog *catalog, uint64_t hash_name)
{
    const uint64_t slot_index = hash_get_default(&catalog->name_to_slot, hash_name, HASH_UNUSED);
    return slot_index != HASH_UNUSED ? ((uint8_t *)catalog->data + catalog->asset_size * slot_index) : 0;
}

void poll_async_assets()
{
    if (!asset_loader->initialized)
    {
        return;
    }

    if (!os_semaphore_poll(asset_loader->loaded_assets_counter))
    {
        return;
    }

    os_enter_critical_section(&asset_loader->pending_assets_cs);
    {
        bool all_handled = true;
        for (Pending_Asset *it = asset_loader->pending_assets; it != array_end(asset_loader->pending_assets); ++it)
        {
            if (it->asset_state == ASSET_STATE_PENDING)
            {
                all_handled = false;
                continue;
            }

            if (it->asset_state == ASSET_STATE_HANDLED)
            {
                continue;
            }

            Asset_Catalog *catalog = it->catalog;
            Asset_Catalog_Callbacks *callbacks = &catalog->callbacks;

            void *asset = find_asset(it->catalog, it->hash_name);
            fatal_check(asset != 0);

            if (it->asset_state == ASSET_STATE_VALID)
            {
                log_info("Loaded async asset %zu successfully", it->hash_name);
                if (catalog->no_descriptor)
                {
                    memcpy(asset, it->descriptor, catalog->descriptor_size);
                }
                else
                {
                    callbacks->asset_load_complete(it->descriptor, asset);
                }
            }
            else if (it->asset_state == ASSET_STATE_FAILED)
            {
                log_info("Failed to load async asset %zu", it->hash_name);
                if (catalog->fallback_asset != 0)
                {
                    memcpy(asset, catalog->fallback_asset, catalog->asset_size);
                }
            }

            it->asset_state = ASSET_STATE_HANDLED;
            c_free(it->allocator, it->descriptor, catalog->descriptor_size);
            c_free(it->allocator, it->data, it->size);
        }

        if (all_handled)
        {
            array_reset(asset_loader->pending_assets);
        }
    }
    os_leave_critical_section(&asset_loader->pending_assets_cs);
}
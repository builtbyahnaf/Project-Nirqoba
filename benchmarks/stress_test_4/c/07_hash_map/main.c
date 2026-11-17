#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

// High-performance open-addressing linear probing hash map
typedef struct {
    uint64_t key;
    uint64_t val;
    uint8_t used;
} HashEntry;

typedef struct {
    HashEntry* entries;
    size_t capacity;
    size_t size;
} HashMap;

static HashMap* create_map(size_t cap) {
    HashMap* map = (HashMap*)malloc(sizeof(HashMap));
    map->capacity = cap;
    map->size = 0;
    map->entries = (HashEntry*)calloc(cap, sizeof(HashEntry));
    return map;
}

static inline void map_insert(HashMap* map, uint64_t key, uint64_t val) {
    size_t mask = map->capacity - 1;
    size_t idx = (key * 11400714819323198485ULL) & mask;
    while (map->entries[idx].used) {
        if (map->entries[idx].key == key) {
            map->entries[idx].val = val;
            return;
        }
        idx = (idx + 1) & mask;
    }
    map->entries[idx].key = key;
    map->entries[idx].val = val;
    map->entries[idx].used = 1;
    map->size++;
}

static inline uint64_t map_lookup(HashMap* map, uint64_t key) {
    size_t mask = map->capacity - 1;
    size_t idx = (key * 11400714819323198485ULL) & mask;
    while (map->entries[idx].used) {
        if (map->entries[idx].key == key) {
            return map->entries[idx].val;
        }
        idx = (idx + 1) & mask;
    }
    return 0;
}

static inline void map_remove(HashMap* map, uint64_t key) {
    size_t mask = map->capacity - 1;
    size_t idx = (key * 11400714819323198485ULL) & mask;
    while (map->entries[idx].used) {
        if (map->entries[idx].key == key) {
            map->entries[idx].used = 0;
            map->size--;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

int main(int argc, char** argv) {
    size_t n = 5000000;
    size_t removals = 1000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
        removals = n / 5;
    }

    HashMap* map = create_map(16777216); // 16M slots power of two

    double t0 = bench_now_ms();
    for (size_t i = 0; i < n; i++) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        map_insert(map, key, (uint64_t)i);
    }
    double t1 = bench_now_ms();
    double insert_ms = t1 - t0;

    double t2 = bench_now_ms();
    uint64_t lookup_sum = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        lookup_sum += map_lookup(map, key);
    }
    double t3 = bench_now_ms();
    double lookup_ms = t3 - t2;

    double t4 = bench_now_ms();
    for (size_t i = 0; i < removals * 5; i += 5) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        map_remove(map, key);
    }
    double t5 = bench_now_ms();
    double remove_ms = t5 - t4;

    double total_ms = insert_ms + lookup_ms + remove_ms;
    printf("WORKLOAD: ST4-07\n");
    printf("LANGUAGE: C\n");
    printf("INSERT_MS: %.3f\n", insert_ms);
    printf("LOOKUP_MS: %.3f\n", lookup_ms);
    printf("REMOVE_MS: %.3f\n", remove_ms);
    printf("TIME_MS: %.3f\n", total_ms);
    printf("REMAINING: %zu\n", map->size);
    printf("CHECKSUM: %" PRIu64 "\n", lookup_sum);
    printf("STATUS: %s\n", map->size == (n - removals) ? "PASS" : "FAIL");

    free(map->entries);
    free(map);
    return 0;
}

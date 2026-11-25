/* nw_dict: add / set / get / find / del, growth, key distributions, iterator */
# include "common.h"
# include "nw_dict.h"

static void check_keys(uint64_t (*key_of)(uint64_t), const char *name)
{
    enum { N = 20000 };
    nw_dict *dict = nw_dict_create(0);
    CHECK(dict != NULL);
    for (uint64_t i = 0; i < N; ++i)
        CHECK_EQ(nw_dict_add(dict, key_of(i), (void *)(uintptr_t)(i + 1)), 0);
    CHECK_EQ(nw_dict_count(dict), N);
    CHECK(dict->size >= N);
    for (uint64_t i = 0; i < N; ++i)
        CHECK_EQ((uintptr_t)nw_dict_get(dict, key_of(i)), i + 1);

    /* the hash must spread the keys: no bucket chain gets long */
    uint32_t longest = 0;
    for (uint32_t b = 0; b < dict->size; ++b) {
        uint32_t len = 0;
        for (nw_dict_entry *e = dict->table[b]; e; e = e->next)
            len++;
        if (len > longest)
            longest = len;
    }
    fprintf(stderr, "%-12s %d keys, %u buckets, longest chain %u\n", name, N, dict->size, longest);
    CHECK(longest <= 16);

    for (uint64_t i = 0; i < N; i += 2)
        CHECK_EQ(nw_dict_del(dict, key_of(i)), 0);
    CHECK_EQ(nw_dict_count(dict), N / 2);
    for (uint64_t i = 0; i < N; ++i)
        CHECK_EQ(nw_dict_find(dict, key_of(i)) != NULL, i % 2 == 1);
    nw_dict_release(dict);
}

static uint64_t sequential(uint64_t i) { return i + 1; }
static uint64_t aligned(uint64_t i) { return i * 4096; }
static uint64_t high_bits(uint64_t i) { return i << 40; }
static uint64_t scrambled(uint64_t i) { return (i + 1) * 0xD6E8FEB86659FD93ULL; }

int main(void)
{
    nw_dict *dict = nw_dict_create(0);
    CHECK(dict != NULL);
    CHECK_EQ(dict->size, 64);
    CHECK_EQ(nw_dict_count(dict), 0);
    CHECK(nw_dict_get(dict, 1) == NULL);
    CHECK_EQ(nw_dict_del(dict, 1), -1);

    int a = 1, b = 2;
    CHECK_EQ(nw_dict_add(dict, 1, &a), 0);
    CHECK_EQ(nw_dict_add(dict, 1, &b), -1);             /* no duplicates */
    CHECK(nw_dict_get(dict, 1) == &a);
    CHECK_EQ(nw_dict_set(dict, 1, &b), 0);              /* overwrite */
    CHECK(nw_dict_get(dict, 1) == &b);
    CHECK_EQ(nw_dict_set(dict, 2, &a), 0);              /* insert */
    CHECK_EQ(nw_dict_count(dict), 2);

    /* key 0, the largest key and a NULL value are all ordinary */
    CHECK_EQ(nw_dict_add(dict, 0, NULL), 0);
    CHECK_EQ(nw_dict_add(dict, UINT64_MAX, &a), 0);
    CHECK(nw_dict_get(dict, 0) == NULL);
    CHECK(nw_dict_find(dict, 0) != NULL);
    CHECK(nw_dict_find(dict, 3) == NULL);
    nw_dict_find(dict, 0)->val = &b;
    CHECK(nw_dict_get(dict, 0) == &b);
    CHECK(nw_dict_get(dict, UINT64_MAX) == &a);

    CHECK_EQ(nw_dict_del(dict, 1), 0);
    CHECK_EQ(nw_dict_del(dict, 1), -1);
    CHECK(nw_dict_get(dict, 1) == NULL);
    CHECK_EQ(nw_dict_count(dict), 3);

    nw_dict_clear(dict);
    CHECK_EQ(nw_dict_count(dict), 0);
    CHECK(nw_dict_get(dict, 2) == NULL);

    /* iterate, deleting some of the entries on the way */
    for (uint64_t i = 1; i <= 1000; ++i)
        CHECK_EQ(nw_dict_add(dict, i, (void *)(uintptr_t)i), 0);
    nw_dict_iterator iter;
    nw_dict_entry *entry;
    uint64_t seen = 0, sum = 0;
    nw_dict_iter_init(&iter, dict);
    while ((entry = nw_dict_next(&iter)) != NULL) {
        CHECK_EQ((uintptr_t)entry->val, entry->key);
        seen++;
        sum += entry->key;
        if (entry->key % 3 == 0)
            CHECK_EQ(nw_dict_del(dict, entry->key), 0);
    }
    CHECK_EQ(seen, 1000);
    CHECK_EQ(sum, 500500);
    CHECK_EQ(nw_dict_count(dict), 1000 - 333);
    CHECK(nw_dict_next(&iter) == NULL);

    /* an empty dict iterates to nothing */
    nw_dict_clear(dict);
    nw_dict_iter_init(&iter, dict);
    CHECK(nw_dict_next(&iter) == NULL);
    nw_dict_release(dict);

    /* init_size is rounded up to a power of two */
    dict = nw_dict_create(1000);
    CHECK(dict != NULL);
    CHECK_EQ(dict->size, 1024);
    for (uint64_t i = 0; i < 1024; ++i)
        CHECK_EQ(nw_dict_add(dict, i, NULL), 0);
    CHECK_EQ(dict->size, 1024);                         /* no growth up to init_size */
    CHECK_EQ(nw_dict_add(dict, 5000, NULL), 0);
    CHECK_EQ(dict->size, 2048);
    nw_dict_release(dict);

    check_keys(sequential, "sequential");
    check_keys(aligned, "aligned");
    check_keys(high_bits, "high bits");
    check_keys(scrambled, "scrambled");

    return 0;
}

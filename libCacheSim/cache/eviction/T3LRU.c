//
//  T3LRU.c  —  Three-Tier LRU eviction policy for libCacheSim
//
//  Three tiers: Cold, Warm, Hot.  Objects enter Warm on insert.
//  Each access increments a decay-based usage counter; tier placement
//  is determined by usage thresholds.  Within each tier the order is
//  FIFO (no reordering on hit).  Capacity enforcement cascades:
//  Hot → Warm → Cold → evict.
//
//  Created on 12/4/18.  Rewritten for T3-LRU tiered design.
//

#include <math.h>

#include "dataStructure/hashtable/hashtable.h"
#include "libCacheSim/evictionAlgo.h"

#ifdef __cplusplus
extern "C" {
#endif

// ***********************************************************************
// ****                                                               ****
// ****                   function declarations                       ****
// ****                                                               ****
// ***********************************************************************

static void T3LRU_free(cache_t *cache);
static bool T3LRU_get(cache_t *cache, const request_t *req);
static cache_obj_t *T3LRU_find(cache_t *cache, const request_t *req,
                               bool update_cache);
static cache_obj_t *T3LRU_insert(cache_t *cache, const request_t *req);
static cache_obj_t *T3LRU_to_evict(cache_t *cache, const request_t *req);
static void T3LRU_evict(cache_t *cache, const request_t *req);
static bool T3LRU_remove(cache_t *cache, obj_id_t obj_id);
static void T3LRU_print_cache(const cache_t *cache);

/* internal helpers */
static void T3LRU_build_decay_table(T3LRU_params_t *params);
static void T3LRU_decay_usage(cache_t *cache, cache_obj_t *obj);
static int T3LRU_determine_target_tier(const T3LRU_params_t *params,
                                       uint8_t usage);
static void T3LRU_transfer_obj(cache_t *cache, cache_obj_t *obj, int to_tier);
static void T3LRU_demote_head(cache_t *cache, const request_t *req,
                              int from_tier);
static void T3LRU_enforce_capacity(cache_t *cache, const request_t *req);
static void T3LRU_parse_params(cache_t *cache, const char *params_str);

/* tier index helpers: Cold=0, Warm=1, Hot=2 */
static inline int tier_id_to_idx(int tier) {
  return tier - 1; /* T3LRU_TIER_COLD(1)→0, WARM(2)→1, HOT(3)→2 */
}

// ***********************************************************************
// ****                                                               ****
// ****                   DecayTable helpers                          ****
// ****                                                               ****
// ***********************************************************************

/**
 * @brief build the precomputed decay lookup table
 *
 * Generates 16 entries with ratios from 0.95 to 0.05, computes the
 * corresponding duration thresholds, deduplicates, and inserts a
 * special no-decay entry at index 0.
 *
 * @param params cache params containing half_life_ticks and decay_table
 */
static void T3LRU_build_decay_table(T3LRU_params_t *params) {
  uint32_t half_life = params->half_life_ticks;
  float ratios[T3LRU_DECAY_TABLE_SIZE];
  int n_entries = 0;

  /* generate 16 ratios from 0.95 down to 0.05, evenly spaced */
  for (int i = 0; i < T3LRU_DECAY_TABLE_SIZE; i++) {
    ratios[i] = 0.95f - i * (0.90f / (T3LRU_DECAY_TABLE_SIZE - 1));
  }

  /* compute thresholds and deduplicate */
  for (int i = 0; i < T3LRU_DECAY_TABLE_SIZE && n_entries < T3LRU_DECAY_TABLE_SIZE; i++) {
    uint32_t threshold = (uint32_t)ceilf(-log2f(ratios[i]) * (float)half_life);
    if (threshold == 0) threshold = 1;

    /* deduplicate: skip if same threshold as previous */
    if (n_entries > 0 &&
        params->decay_table[n_entries - 1].threshold == threshold) {
      params->decay_table[n_entries - 1].decay_ratio = ratios[i];
      continue;
    }

    params->decay_table[n_entries].threshold = threshold;
    params->decay_table[n_entries].decay_ratio = ratios[i];
    n_entries++;
  }

  /* special entry [0]: half of [1]'s threshold, ratio=1.0 (no decay) */
  if (n_entries >= 2) {
    /* shift everything right by one to make room */
    for (int i = n_entries; i > 0; i--) {
      params->decay_table[i] = params->decay_table[i - 1];
    }
    params->decay_table[0].threshold = params->decay_table[1].threshold / 2;
    if (params->decay_table[0].threshold == 0)
      params->decay_table[0].threshold = 1;
    params->decay_table[0].decay_ratio = 1.0f;
    n_entries++;
  }

  /* fill remaining slots with sentinel */
  for (int i = n_entries; i < T3LRU_DECAY_TABLE_SIZE; i++) {
    params->decay_table[i].threshold = UINT32_MAX;
    params->decay_table[i].decay_ratio = 0.01f;
  }
}

/**
 * @brief look up the decay ratio for a given duration
 *
 * Linear scan of the 16-entry table; returns the ratio of the first
 * entry whose threshold >= duration.  Falls back to 0.01 (MIN_RATIO).
 *
 * @param params
 * @param duration time elapsed since last decay
 * @return decay ratio in [0.01, 1.0]
 */
static float T3LRU_get_decay_ratio(const T3LRU_params_t *params,
                                   uint32_t duration) {
  for (int i = 0; i < T3LRU_DECAY_TABLE_SIZE; i++) {
    if (params->decay_table[i].threshold >= duration) {
      return params->decay_table[i].decay_ratio;
    }
  }
  return 0.01f;
}

/**
 * @brief apply halflife decay to an object's usage counter
 *
 * Computes duration since last decay, looks up the ratio in the decay
 * table, and sets usage = round(usage * ratio).  Short durations are
 * skipped (no decay).
 *
 * @param cache
 * @param obj the object to decay
 */
static void T3LRU_decay_usage(cache_t *cache, cache_obj_t *obj) {
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;
  uint32_t now = (uint32_t)cache->n_req;
  uint32_t duration = now - obj->T3LRU.timestamp;

  if (duration <= params->decay_table[0].threshold) {
    return; /* too short, no decay */
  }

  float ratio = T3LRU_get_decay_ratio(params, duration);
  obj->T3LRU.usage = (uint8_t)roundf((float)obj->T3LRU.usage * ratio);
  obj->T3LRU.timestamp = now;
}

/**
 * @brief determine the target tier based on usage value
 *
 * @param params cache params for threshold values
 * @param usage decay-based heat counter (0-15)
 * @return target tier id (T3LRU_TIER_NONE/COLD/WARM/HOT)
 */
static int T3LRU_determine_target_tier(const T3LRU_params_t *params,
                                       uint8_t usage) {
  if (usage == 0) return T3LRU_TIER_NONE;
  if (usage > params->hot_threshold) return T3LRU_TIER_HOT;
  if (usage > params->warm_threshold) return T3LRU_TIER_WARM;
  return T3LRU_TIER_COLD;
}

// ***********************************************************************
// ****                                                               ****
// ****                   Tier transfer helpers                       ****
// ****                                                               ****
// ***********************************************************************

/**
 * @brief move an object from its current tier to a target tier
 *
 * Removes the object from the source tier's linked list and appends it
 * to the target tier's tail (FIFO).  If to_tier is T3LRU_TIER_NONE,
 * the object is evicted via cache_evict_base.
 *
 * @param cache
 * @param obj the object to transfer
 * @param to_tier target tier id
 */
static void T3LRU_transfer_obj(cache_t *cache, cache_obj_t *obj, int to_tier) {
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;
  int from_idx = tier_id_to_idx(obj->T3LRU.tier);

  /* remove from current tier */
  if (obj->T3LRU.tier != T3LRU_TIER_NONE) {
    remove_obj_from_list(&params->tier_head[from_idx],
                         &params->tier_tail[from_idx], obj);
    params->tier_n_bytes[from_idx] -= obj->obj_size + cache->obj_md_size;
    params->tier_n_objs[from_idx]--;
  }

  if (to_tier == T3LRU_TIER_NONE) {
    cache_evict_base(cache, obj, true);
    return;
  }

  /* append to target tier tail (FIFO) */
  int to_idx = tier_id_to_idx(to_tier);
  append_obj_to_tail(&params->tier_head[to_idx], &params->tier_tail[to_idx],
                     obj);
  params->tier_n_bytes[to_idx] += obj->obj_size + cache->obj_md_size;
  params->tier_n_objs[to_idx]++;
  obj->T3LRU.tier = (int8_t)to_tier;
}

/**
 * @brief demote the head (oldest) object of a tier to the next lower tier
 *
 * Hot→Warm, Warm→Cold, Cold→evict.  Used by enforce_capacity.
 *
 * @param cache
 * @param req
 * @param from_tier the tier to demote from
 */
static void T3LRU_demote_head(cache_t *cache, const request_t *req,
                              int from_tier) {
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;
  int from_idx = tier_id_to_idx(from_tier);

  if (params->tier_head[from_idx] == NULL) return;

  cache_obj_t *obj = params->tier_head[from_idx];
  int target;

  if (from_tier == T3LRU_TIER_HOT) {
    target = T3LRU_TIER_WARM;
  } else if (from_tier == T3LRU_TIER_WARM) {
    target = T3LRU_TIER_COLD;
  } else {
    /* Cold tier: evict */
    T3LRU_transfer_obj(cache, obj, T3LRU_TIER_NONE);
    return;
  }

  T3LRU_transfer_obj(cache, obj, target);
}

/**
 * @brief enforce per-tier soft limits by cascading demotion
 *
 * Iterates Hot→Warm→Cold: while a tier exceeds its soft_limit, demote
 * its head object to the next lower tier.  Cold tier overflow triggers
 * eviction.
 *
 * @param cache
 * @param req
 */
static void T3LRU_enforce_capacity(cache_t *cache, const request_t *req) {
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;

  /* cascade: Hot → Warm → Cold → evict */
  while (params->tier_n_bytes[tier_id_to_idx(T3LRU_TIER_HOT)] >
         params->tier_soft_limit[tier_id_to_idx(T3LRU_TIER_HOT)]) {
    if (params->tier_head[tier_id_to_idx(T3LRU_TIER_HOT)] == NULL) break;
    T3LRU_demote_head(cache, req, T3LRU_TIER_HOT);
  }

  while (params->tier_n_bytes[tier_id_to_idx(T3LRU_TIER_WARM)] >
         params->tier_soft_limit[tier_id_to_idx(T3LRU_TIER_WARM)]) {
    if (params->tier_head[tier_id_to_idx(T3LRU_TIER_WARM)] == NULL) break;
    T3LRU_demote_head(cache, req, T3LRU_TIER_WARM);
  }

  while (params->tier_n_bytes[tier_id_to_idx(T3LRU_TIER_COLD)] >
         params->tier_soft_limit[tier_id_to_idx(T3LRU_TIER_COLD)]) {
    if (params->tier_head[tier_id_to_idx(T3LRU_TIER_COLD)] == NULL) break;
    T3LRU_demote_head(cache, req, T3LRU_TIER_COLD);
  }
}

// ***********************************************************************
// ****                                                               ****
// ****                   Parameter parsing                           ****
// ****                                                               ****
// ***********************************************************************

/**
 * @brief parse T3LRU-specific parameters from a comma-separated string
 *
 * Supported keys: half-life, tier-ratio (e.g. "4:5:1"),
 * hot-threshold, warm-threshold
 *
 * @param cache
 * @param params_str parameter string, may be NULL
 */
static void T3LRU_parse_params(cache_t *cache, const char *params_str) {
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;

  if (params_str == NULL || params_str[0] == '\0') return;

  char *str = strdup(params_str);
  char *old = str;

  while (str != NULL && str[0] != '\0') {
    char *key = strsep(&str, "=");
    char *value = strsep(&str, ",");

    while (str != NULL && *str == ' ') str++;

    if (strcasecmp(key, "half-life") == 0) {
      params->half_life_ticks = (uint32_t)strtoul(value, NULL, 0);
    } else if (strcasecmp(key, "tier-ratio") == 0) {
      /* format: "4:5:1" for Cold:Warm:Hot */
      int ratios[3] = {0, 0, 0};
      char *v = value;
      for (int i = 0; i < 3 && v != NULL; i++) {
        ratios[i] = (int)strtol(v, NULL, 0);
        v = strchr(v, ':');
        if (v) v++;
      }
      int sum = ratios[0] + ratios[1] + ratios[2];
      if (sum > 0) {
        for (int i = 0; i < 3; i++) {
          params->tier_soft_limit[i] =
              (int64_t)((double)ratios[i] / sum * cache->cache_size);
        }
      }
    } else if (strcasecmp(key, "hot-threshold") == 0) {
      params->hot_threshold = (uint8_t)strtol(value, NULL, 0);
    } else if (strcasecmp(key, "warm-threshold") == 0) {
      params->warm_threshold = (uint8_t)strtol(value, NULL, 0);
    }
  }

  free(old);
}

// ***********************************************************************
// ****                                                               ****
// ****                   end user facing functions                   ****
// ****                                                               ****
// ***********************************************************************

/**
 * @brief initialize a T3LRU cache
 *
 * @param ccache_params some common cache parameters
 * @param cache_specific_params T3LRU specific parameters, supports:
 *  half-life=N (decay half-life in requests, default 1000),
 *  tier-ratio=C:W:H (Cold:Warm:Hot capacity ratio, default 4:5:1),
 *  hot-threshold=N (usage threshold for Hot tier, default 10),
 *  warm-threshold=N (usage threshold for Warm tier, default 3)
 */
cache_t *T3LRU_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params) {
  cache_t *cache =
      cache_struct_init("T3LRU", ccache_params, cache_specific_params);
  cache->cache_init = T3LRU_init;
  cache->cache_free = T3LRU_free;
  cache->get = T3LRU_get;
  cache->find = T3LRU_find;
  cache->insert = T3LRU_insert;
  cache->evict = T3LRU_evict;
  cache->remove = T3LRU_remove;
  cache->to_evict = T3LRU_to_evict;
  cache->get_occupied_byte = cache_get_occupied_byte_default;
  cache->can_insert = cache_can_insert_default;
  cache->get_n_obj = cache_get_n_obj_default;
  cache->print_cache = T3LRU_print_cache;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = 8 * 2;
  } else {
    cache->obj_md_size = 0;
  }

  T3LRU_params_t *params = malloc(sizeof(T3LRU_params_t));
  memset(params, 0, sizeof(T3LRU_params_t));
  cache->eviction_params = params;

  /* default thresholds */
  params->hot_threshold = T3LRU_HOT_THRESHOLD;
  params->warm_threshold = T3LRU_WARM_THRESHOLD;
  params->half_life_ticks = T3LRU_DEFAULT_HALF_LIFE;

  /* default tier ratios: Cold:Warm:Hot = 4:5:1 */
  params->tier_soft_limit[0] = (int64_t)(cache->cache_size * 4.0 / 10.0);
  params->tier_soft_limit[1] = (int64_t)(cache->cache_size * 5.0 / 10.0);
  params->tier_soft_limit[2] = (int64_t)(cache->cache_size * 1.0 / 10.0);

  /* parse user params (may override defaults) */
  if (cache_specific_params != NULL) {
    T3LRU_parse_params(cache, cache_specific_params);
  }

  /* build decay table */
  T3LRU_build_decay_table(params);

  return cache;
}

/**
 * free resources used by this cache
 *
 * @param cache
 */
static void T3LRU_free(cache_t *cache) {
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;
  free(params);
  cache_struct_free(cache);
}

/**
 * @brief this function is the user facing API
 * it performs the following logic
 *
 * ```
 * if obj in cache:
 *    update_metadata
 *    return true
 * else:
 *    if cache does not have enough space:
 *        evict until it has space to insert
 *    insert the object
 *    return false
 * ```
 *
 * @param cache
 * @param req
 * @return true if cache hit, false if cache miss
 */
static bool T3LRU_get(cache_t *cache, const request_t *req) {
  return cache_get_base(cache, req);
}

// ***********************************************************************
// ****                                                               ****
// ****       developer facing APIs (used by cache developer)         ****
// ****                                                               ****
// ***********************************************************************

/**
 * @brief check whether an object is in the cache
 *
 * On a hit with update_cache=true, the object's usage is decayed based on
 * elapsed time, then incremented.  If the new usage qualifies for a higher
 * tier, the object is transferred (only upgrades, never downgrades on hit).
 * The object is NOT reordered within its current tier (FIFO semantics).
 *
 * @param cache
 * @param req
 * @param update_cache whether to update the cache,
 *  if true, decay usage, increment usage, and possibly upgrade tier
 * @return the cache object on hit, NULL on miss
 */
static cache_obj_t *T3LRU_find(cache_t *cache, const request_t *req,
                               bool update_cache) {
  cache_obj_t *obj = cache_find_base(cache, req, update_cache);
  if (obj == NULL || !update_cache) return obj;

  /* decay usage based on time elapsed */
  T3LRU_decay_usage(cache, obj);

  /* increment usage (heat counter) */
  if (obj->T3LRU.usage < T3LRU_USAGE_MAX) {
    obj->T3LRU.usage++;
  }

  /* determine target tier — only upgrade, never downgrade on hit */
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;
  int target = T3LRU_determine_target_tier(params, obj->T3LRU.usage);
  if (target > obj->T3LRU.tier) {
    T3LRU_transfer_obj(cache, obj, target);
  }

  return obj;
}

/**
 * @brief insert an object into the cache,
 * update the hash table and cache metadata
 *
 * The object is placed in the Warm tier with usage=2.  After insertion,
 * enforce_capacity is called which may cascade demote/evict objects.
 * This function assumes the cache has enough space
 * and eviction is not part of this function.
 *
 * @param cache
 * @param req
 * @return the inserted object
 */
static cache_obj_t *T3LRU_insert(cache_t *cache, const request_t *req) {
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;

  cache_obj_t *obj = cache_insert_base(cache, req);

  /* initialize T3LRU metadata */
  obj->T3LRU.tier = T3LRU_TIER_WARM;
  obj->T3LRU.usage = 2;
  obj->T3LRU.timestamp = (uint32_t)cache->n_req;

  /* append to Warm tier tail (FIFO) */
  int warm_idx = tier_id_to_idx(T3LRU_TIER_WARM);
  append_obj_to_tail(&params->tier_head[warm_idx], &params->tier_tail[warm_idx],
                     obj);
  params->tier_n_bytes[warm_idx] += obj->obj_size + cache->obj_md_size;
  params->tier_n_objs[warm_idx]++;

  /* enforce capacity limits (may cascade demote/evict) */
  T3LRU_enforce_capacity(cache, req);

  return obj;
}

/**
 * @brief find the object to be evicted
 * this function does not actually evict the object or update metadata
 *
 * Returns the tail of the lowest non-empty tier (Cold > Warm > Hot).
 *
 * @param cache the cache
 * @param req
 * @return the object to be evicted, or NULL if cache is empty
 */
static cache_obj_t *T3LRU_to_evict(cache_t *cache, const request_t *req) {
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;

  /* eviction priority: Cold first, then Warm, then Hot */
  for (int i = 0; i < T3LRU_N_TIERS; i++) {
    if (params->tier_tail[i] != NULL) {
      cache->to_evict_candidate_gen_vtime = cache->n_req;
      return params->tier_tail[i];
    }
  }
  return NULL;
}

/**
 * @brief evict an object from the cache
 * it needs to call cache_evict_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * Evicts from the lowest non-empty tier (Cold > Warm > Hot).
 *
 * @param cache
 * @param req not used
 */
static void T3LRU_evict(cache_t *cache, const request_t *req) {
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;

  /* find the lowest non-empty tier to evict from */
  cache_obj_t *obj_to_evict = NULL;
  int evict_tier_idx = -1;

  for (int i = 0; i < T3LRU_N_TIERS; i++) {
    if (params->tier_tail[i] != NULL) {
      obj_to_evict = params->tier_tail[i];
      evict_tier_idx = i;
      break;
    }
  }

  DEBUG_ASSERT(obj_to_evict != NULL);

  /* remove from tier list */
  remove_obj_from_list(&params->tier_head[evict_tier_idx],
                       &params->tier_tail[evict_tier_idx], obj_to_evict);
  params->tier_n_bytes[evict_tier_idx] -=
      obj_to_evict->obj_size + cache->obj_md_size;
  params->tier_n_objs[evict_tier_idx]--;

  cache_evict_base(cache, obj_to_evict, true);
}

/**
 * @brief remove the given object from the cache
 * note that eviction should not call this function, but rather call
 * `cache_evict_base` because we track extra metadata during eviction
 *
 * and this function is different from eviction
 * because it is used to for user trigger
 * remove, and eviction is used by the cache to make space for new objects
 *
 * it needs to call cache_remove_obj_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param obj
 */
static void T3LRU_remove_obj(cache_t *cache, cache_obj_t *obj) {
  assert(obj != NULL);
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;

  int idx = tier_id_to_idx(obj->T3LRU.tier);
  remove_obj_from_list(&params->tier_head[idx], &params->tier_tail[idx], obj);
  params->tier_n_bytes[idx] -= obj->obj_size + cache->obj_md_size;
  params->tier_n_objs[idx]--;

  cache_remove_obj_base(cache, obj, true);
}

/**
 * @brief remove an object from the cache
 * this is different from cache_evict because it is used to for user trigger
 * remove, and eviction is used by the cache to make space for new objects
 *
 * it needs to call cache_remove_obj_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param obj_id
 * @return true if the object is removed, false if the object is not in the
 * cache
 */
static bool T3LRU_remove(cache_t *cache, obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) return false;

  T3LRU_remove_obj(cache, obj);
  return true;
}

static void T3LRU_print_cache(const cache_t *cache) {
  T3LRU_params_t *params = (T3LRU_params_t *)cache->eviction_params;
  const char *tier_names[] = {"Cold", "Warm", "Hot"};

  for (int i = T3LRU_N_TIERS - 1; i >= 0; i--) {
    printf("%s: ", tier_names[i]);
    cache_obj_t *cur = params->tier_head[i];
    if (cur == NULL) {
      printf("empty\n");
      continue;
    }
    while (cur != NULL) {
      printf("%lu(u=%d)->", (unsigned long)cur->obj_id, cur->T3LRU.usage);
      cur = cur->queue.next;
    }
    printf("END\n");
  }
}

#ifdef __cplusplus
}
#endif

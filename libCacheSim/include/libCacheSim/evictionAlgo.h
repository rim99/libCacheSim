#pragma once

#include "cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  cache_obj_t *q_head;
  cache_obj_t *q_tail;
} FIFO_params_t;

/* used by LFU related */
typedef struct {
  cache_obj_t *q_head;
  cache_obj_t *q_tail;
} LRU_params_t;

/* T3LRU tier IDs */
#define T3LRU_TIER_NONE 0
#define T3LRU_TIER_COLD 1
#define T3LRU_TIER_WARM 2
#define T3LRU_TIER_HOT  3
#define T3LRU_N_TIERS   3  /* Cold, Warm, Hot */
#define T3LRU_DECAY_TABLE_SIZE 16
#define T3LRU_USAGE_MAX 15
#define T3LRU_HOT_THRESHOLD 10
#define T3LRU_WARM_THRESHOLD 3
#define T3LRU_DEFAULT_HALF_LIFE 1000

typedef struct {
  uint32_t threshold;  /* duration threshold in ticks */
  float decay_ratio;   /* multiplier for usage */
} T3LRU_decay_entry_t;

typedef struct {
  /* per-tier doubly linked lists: index 0=Cold, 1=Warm, 2=Hot */
  cache_obj_t *tier_head[T3LRU_N_TIERS];
  cache_obj_t *tier_tail[T3LRU_N_TIERS];
  int64_t tier_n_bytes[T3LRU_N_TIERS];
  int64_t tier_n_objs[T3LRU_N_TIERS];
  int64_t tier_soft_limit[T3LRU_N_TIERS]; /* in bytes */

  /* decay table */
  T3LRU_decay_entry_t decay_table[T3LRU_DECAY_TABLE_SIZE];
  uint32_t half_life_ticks;

  /* thresholds */
  uint8_t hot_threshold;
  uint8_t warm_threshold;
} T3LRU_params_t;

/* used by LFU related */
typedef struct freq_node {
  int64_t freq;
  cache_obj_t *first_obj;
  cache_obj_t *last_obj;
  int32_t n_obj;
} freq_node_t;

typedef struct {
  cache_obj_t *q_head;
  cache_obj_t *q_tail;
  // clock uses one-bit counter
  int32_t n_bit_counter;
  // max_freq = 1 << (n_bit_counter - 1)
  int32_t max_freq;
  int32_t init_freq;

  int64_t n_obj_rewritten;
  int64_t n_byte_rewritten;
} Clock_params_t;

cache_t *ARC_init(const common_cache_params_t ccache_params,
                  const char *cache_specific_params);

cache_t *ARCv0_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params);

cache_t *Belady_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params);

cache_t *BeladySize_init(const common_cache_params_t ccache_params,
                         const char *cache_specific_params);

cache_t *CAR_init(const common_cache_params_t ccache_params,
                  const char *cache_specific_params);

cache_t *Cacheus_init(const common_cache_params_t ccache_params,
                      const char *cache_specific_params);

cache_t *Clock_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params);

cache_t *Clock2QPlus_init(const common_cache_params_t ccache_params,
                          const char *cache_specific_params);

cache_t *ClockPro_init(const common_cache_params_t ccache_params,
                       const char *cache_specific_params);

cache_t *CR_LFU_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params);

cache_t *FIFO_Merge_init(const common_cache_params_t ccache_params,
                         const char *cache_specific_params);

cache_t *FIFO_Reinsertion_init(const common_cache_params_t ccache_params,
                               const char *cache_specific_params);

cache_t *FIFO_init(const common_cache_params_t ccache_params,
                   const char *cache_specific_params);

cache_t *flashProb_init(const common_cache_params_t ccache_params,
                        const char *cache_specific_params);

cache_t *GDSF_init(const common_cache_params_t ccache_params,
                   const char *cache_specific_params);

cache_t *Hyperbolic_init(const common_cache_params_t ccache_params,
                         const char *cache_specific_params);

cache_t *LeCaR_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params);

cache_t *LeCaRv0_init(const common_cache_params_t ccache_params,
                      const char *cache_specific_params);

cache_t *LFU_init(const common_cache_params_t ccache_params,
                  const char *cache_specific_params);

cache_t *LFUCpp_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params);

cache_t *LFUDA_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params);

cache_t *LHD_init(const common_cache_params_t ccache_params,
                  const char *cache_specific_params);

cache_t *LIRS_init(const common_cache_params_t ccache_params,
                   const char *cache_specific_params);

cache_t *LRU_Prob_init(const common_cache_params_t ccache_params,
                       const char *cache_specific_params);

cache_t *LRU_init(const common_cache_params_t ccache_params,
                  const char *cache_specific_params);

cache_t *T3LRU_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params);

cache_t *LRU_K_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params);

cache_t *LRUv0_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params);

cache_t *MRU_init(const common_cache_params_t ccache_params,
                  const char *cache_specific_params);

cache_t *nop_init(const common_cache_params_t ccache_params,
                  const char *cache_specific_params);

// plugin cache that allows user to implement custom cache
cache_t *pluginCache_init(const common_cache_params_t ccache_params,
                          const char *cache_specific_params);

cache_t *QDLP_init(const common_cache_params_t ccache_params,
                   const char *cache_specific_params);

cache_t *RandomLRU_init(const common_cache_params_t ccache_params,
                        const char *cache_specific_params);

cache_t *RandomTwo_init(const common_cache_params_t ccache_params,
                        const char *cache_specific_params);

cache_t *Random_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params);

cache_t *S3FIFO_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params);

cache_t *S3FIFOd_init(const common_cache_params_t ccache_params,
                      const char *cache_specific_params);

cache_t *S3FIFOv0_init(const common_cache_params_t ccache_params,
                       const char *cache_specific_params);

cache_t *S3LRU_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params);

cache_t *SFIFO_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params);

cache_t *SFIFOv0_init(const common_cache_params_t ccache_params,
                      const char *cache_specific_params);

cache_t *Sieve_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params);

cache_t *Size_init(const common_cache_params_t ccache_params,
                   const char *cache_specific_params);

cache_t *SLRU_init(const common_cache_params_t ccache_params,
                   const char *cache_specific_params);

cache_t *SLRUv0_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params);

cache_t *SR_LRU_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params);

cache_t *TwoQ_init(const common_cache_params_t ccache_params,
                   const char *cache_specific_params);

cache_t *WTinyLFU_init(const common_cache_params_t ccache_params,
                       const char *cache_specific_params);

#ifdef ENABLE_3L_CACHE
cache_t *ThreeLCache_init(const common_cache_params_t ccache_params,
                          const char *cache_specific_params);
#endif

#ifdef ENABLE_LRB
cache_t *LRB_init(const common_cache_params_t ccache_params,
                  const char *cache_specific_params);
#endif

#if defined(ENABLE_GLCACHE)

cache_t *GLCache_init(const common_cache_params_t ccache_params,
                      const char *cache_specific_params);

#endif

#ifdef __cplusplus
}
#endif

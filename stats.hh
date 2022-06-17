#ifndef _STATS_H
#define _STATS_H

#include "asic_core.hh"
#include "common.hh"

class asic_core;
class asic;
class network;
class stats;

class stats {
  friend class asic;

  public:
  stats(asic* host);
  void reset_freq_stats();
  void print_freq_stats();
  int get_bucket_num(int i);
  void reset_cache_hit_aware_stats();
  public:
  // temporary data
    int *_stat_extra_finished_edges;
    int _stat_ping_pong_vertices=0;
    int _stat_low_freq_vertices=0;
    int _stat_entry_cacheable=0;
    int _stat_entry_uncacheable=0;
    int _stat_cache_hits=0;


    int _stat_first_time=0;
    int _stat_high_deg_access=0;
    int _stat_high_deg_hit=0;
    int _stat_low_deg_access=0;
    int _stat_low_deg_hit=0;
    int _stat_num_clusters=0;

    int _stat_dram_add_on_access=0;

    int _stat_barrier_count=0;
    int _stat_agg_barrier=0;
    int _stat_num_misspeculations=0; // required for espresso
    uint64_t _stat_tot_created_data_parallel_tasks=0;
    uint64_t _stat_tot_created_double_buffer_data_parallel_tasks=0;
    uint64_t _stat_tot_created_edge_tasks=0;
    uint64_t _stat_tot_created_edge_second_buffer_tasks=0;
    uint64_t _stat_tot_data_parallel_tasks=0;
    uint64_t _stat_tot_finished_edges=0;
    uint64_t _stat_prev_finished_edges=0;
    uint64_t _stat_tot_finished_first_buffer_edge_tasks=0;
    uint64_t _stat_tot_finished_second_buffer_edge_tasks=0;
    uint64_t _stat_finished_coarse_tasks=0;
    uint64_t _stat_tot_finished_first_buffer_data_parallel_tasks=0;
    uint64_t _stat_tot_finished_second_buffer_data_parallel_tasks=0;
    uint64_t _stat_global_finished_edges=0;
    int _stat_sync_finished_edges=0;
    int _stat_extreme_finished_edges=0;

    // different work metric for triangle counting
    uint64_t _stat_tot_triangles_found=0;
 
    int _stat_tot_aborted_edges=0;
    int _stat_tot_tasks_dequed=0;
 
    int _stat_num_compulsory_misses=0;
    int _stat_num_accesses=0;
    int _stat_tot_vert_tasks=0;
    int _stat_conf_miss=0;
    int _stat_conf_hit=0;

    int _stat_dependent_task_erased=0;
    int _stat_abort_packets_sent=0;
    int _stat_executing=0;
    int _stat_not_found_in_task_queue=0;
    int _stat_not_found_in_local_dep_queue=0;
    int _stat_abort_with_invalid=0;
    int _stat_abort_with_distance=0;
    int _stat_deleted_task_entry=0;
    int _stat_deleted_agg_task_entry=0;
    int _stat_deleted_before_net=0;
    int _stat_task_stolen=0;
    int _stat_reorder_width=0;
    int _stat_bypass_count=0;
    int _stat_online_tasks=0;

    int _stat_tot_unused_line=0;
    int _stat_temporal_hits=0;
 
    int _stat_extra_mem_access_reqd=0;
    int _stat_blocked_with_conflicts=0;
    uint64_t _stat_dram_cache_accesses=0;
    uint64_t _stat_dram_bytes_requested=0;
    // int _stat_dram_accesses_requested=0;
    float _stat_cache_line_util=0;

    int _stat_high_prio_task=0;

    int _stat_vertices_done=0;
 
    int _stat_received_mem=0;
    int _stat_coalesced_mem=0;
    int _stat_delayed_cache_hit_tasks=0;
    int _stat_allowed_cache_hit_tasks=0;
    int _stat_correct_cache_hit_tasks=0;
    int _stat_batched_tasks=0;
    int _stat_cycles_batched=0;
    int _stat_delayed_batch_cycles=0;
    int _stat_wrong_cache_hit_tasks=0;
    int _stat_cycles_hit_queue_is_chosen=0;
    int _stat_cycles_miss_queue_is_chosen=0;

  int _stat_duplicate=0;
    int _stat_pushed_to_ps=0;
    int _stat_vec_mat=0;
    int _stat_fine_gran_lb_factor[core_cnt];
    int _stat_fine_gran_lb_factor_atomic[core_cnt];


    int _stat_tot_num_copy_updates=0;
    int _stat_global_num_copy_updates=0;
    int _stat_num_dupl_copy_vertex_updates=0;
    int _stat_remote_tasks_this_phase=0;
    int _stat_owned_tasks_this_phase=0;
    int _stat_transition_tasks=0;
    int _stat_overflow_tasks=0;

    int _stat_max_fifo_storage=0;

    int _stat_num_req_per_bank[num_banks];
    int _stat_dist_tasks_dequed[core_cnt];
    int _stat_dist_edges_dequed[core_cnt];
    int _stat_avg_reduce_tasks[core_cnt];

    // int _stat_reuse[E];
    // int _last_accessed_cycle[E];
    int _stat_enq_this_cycle[core_cnt];
    int _stat_tot_enq_per_core[core_cnt];
 
    int _stat_correct_edges[core_cnt];
    int _stat_incorrect_edges[core_cnt];
    int _stat_stall_thr[core_cnt];
    int _stat_mem_stall_thr[core_cnt];

    int _stat_finished_edges_per_core[core_cnt];

  int _stat_pending_cache_misses=0;
    int _stat_pending_colaesces=0;

    int *_stat_owned_vertex_freq_per_itn;
    int *_stat_boundary_vertex_freq_per_itn;
    int *_stat_edge_freq_per_itn;

    int *_stat_source_copy_vertex_count;
    bitset<V> *_is_source_done;

    int _tot_activated_vertices=0;
    int _stat_tot_completed_tasks=0;

    int _stat_edges_dropped_with_break=0;

    // cache-hit-aware scheduling
    int _stat_tot_priority_hit_updates_occupancy[core_cnt];
    int _stat_tot_hit_updates_occupancy[core_cnt];
    int _stat_tot_miss_updates_occupancy[core_cnt];
    int _stat_tot_prefetch_updates_occupancy[core_cnt];
    int _stat_tot_hit_updates_served[core_cnt];
    int _stat_tot_miss_updates_served[core_cnt];
    int _stat_tot_priority_hit_updates_served[core_cnt];

    int _stats_misses_in_cache_hit=0;
    int _stats_coalesces_in_cache_hit=0;
    int _stats_accesses_in_cache_hit=0;

    int _num_spatial_issue=0;
    int _num_temporal_issue=0;
    int _num_tasks_batched=0;

    int _cycles_in_flushing_reconfiguration=0;
    int *_start_cycle_per_query;


    asic *_asic;
};

#endif

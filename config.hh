#ifndef _CONFIG_H
#define _CONFIG_H

#include "asic_core.hh"
#include "common.hh"

class asic_core;
class asic;
class network;
class stats;
class config;

class config
{
  friend class asic;

public:
  config(asic *host);
  bool is_async();
  bool is_sync();
  bool is_vector();
  bool is_slice();
  bool is_non_frontier();
  bool no_swarm();

public:
  // dynamic graphs
  enum dyn_algo_type _dyn_algo;
  enum dyn_gen_type _dyn_gen;
  bool _update_batch = false;
  bool _dyn_graph = false;
  bool _seq_edge = false;
  bool _inc_dyn = false;
  bool _recomp_dyn = false;

  // hybrid execution
  int _slice_hrst = 0;
  int _heuristic = 0;
  bool _cache_no_switch = false;
  bool _async_no_switch = false;
  bool _hybrid_eval = false;
  bool _hybrid = false; /*not sure*/

  // stats
  bool _detailed_stats = false;
  bool _reuse_stats = false;
  bool _profiling = 0;

  // network module
  enum spatial_part_type _spatial_part;
  bool _blocked_dfs = false;
  bool _dfsmap = false;
  bool _bfs_map = 0;
  bool _modulo = 0;
  bool _linear = 0;
  bool _bdfs = 0;
  bool _bbfs = 0;
  bool _random_spatial = 0;
  bool _noload = 0;

  int _bd = 1000;

  // crossbar features?
  int _crossbar_lat = 0;
  int _crossbar_bw = 1;
  bool _model_net_delay = 0;
  int _network = 1;
  bool _hrc_xbar = false;

  enum net_traffic_type _net_traffic;
  bool _decomposable = 0;
  bool _worst_net = 0;
  bool _ideal_net = 0;

  bool _real_multicast = 0;
  bool _path_multicast = 0;

  bool _perfect_net = 0;
  bool _perfect_lb = 0;

  // computation module
  int _dfg_length = 8;
  int _process_thr = 1;
  bool _pull_mode = 0;

  // memory controller
  int _pred_thr = 0;
  bool _dramsim_enabled = 0;
  bool _numa_contention = 0;
  bool _all_edge_access = false;
  bool _cache_hit_aware_sched = 0;
  bool _mult_cache_hit_aware_sched = 0;
  bool _hats = false;
  bool _hybrid_hats_cache_hit = false;
  enum domain_type _domain = graphs;
  bool _hash_join = false;
  bool _tree_trav = false;

  // cache
  enum cache_repl_type _cache_repl;
  bool _banked = 0;
  bool _all_cache = 0;
  bool _resizing = 0;
  bool _lru = 0;
  bool _edge_cache = 0;
  bool _special_cache = 0; // TODO: this should be combined, separate modules?!!
  bool _working_cache = 0;
  bool _phi = 0;

  bool _allhits = 0;
  bool _allmiss = 0;

  // scratch
  int _slice_iter = 1;
  int _slice_count = 4;
  bool _random = 0; // TODO: not sure if this works!!
  bool _metis = 0;

  // task controller
  bool _dyn_load_bal = false;
  bool _edge_load_bal = false;
  bool _prac = 0;
  bool _presorter = false;
  bool _reorder = 0; // FIXME: not sure!!
  // types of scheduling policies
  bool _abcd = false;
  bool _distance_sched = 1;
  bool _fifo = 0;
  bool _entry_abort = 0;
  bool _update_coalesce = 0;
  bool _abort = 0;

  bool _central_batch = true;

  int _high_prio_reserve = 0;

  // taxonomy: this way will help us cover all workloads -- Do we want to
  // change taxonomy somehow?
  enum update_visibility _update_visible;
  enum task_sched_type _task_sched;
  enum slice_sched_type _slice_sched; // abcd, round-robin, reuse

  // graph specific details
  int _graph_dia = 1;
  bool _undirected = 0;
  bool _unweighted = 0;
  bool _csr = 0;

  // algorithm specific details
  bool _pr = false;
  bool _abfs = false;
  bool _acc = false;
  bool _astar = false;
  bool _gcn = false;
  bool _cf = false;
  bool _tc = false;
  bool _ladies_gcn = false;
  bool _gcn_matrix = false;
  bool _sync_sim = 0;

  int _src_loc = 0;
  int _gcn_layers = 0;
  int _feat_len = 8;
  bool _sgu_gcn_reorder = 0; /*FIXME: do we need this? or is it working?*/

  int _batched_cores = 1;

  // gcn
  enum sync_type _agg_mult_type = global;
  enum sync_type _mult_agg_type = global;
  bool _heter_cores = 0;

  bool _graphmat_cf = 0;

  // deprecated
  bool _gcn_swarm = 0;
  bool _cf_swarm = 0;
  int _lazy_cycles = 1000000000;
  int _dep_check_depth = 4;
  bool _chronos = 0;

  // algorithm optimizations
  bool _sync_scr = 0; // TODO: something like depth-wise GCN
  bool _dyn_reuse = 0;
  bool _extreme = 0; // FIXME: required?
  int _reuse = 1;

  // execution model
  enum exec_model_type _exec_model;
  bool _sgu = 0;
  bool _sgu_slicing = 0;
  bool _graphmat_slicing = 0;
  bool _swarm = 0;
  bool _espresso = 0;
  bool _graphlab = 0;
  bool _sgu_hybrid = 0;
  bool _tesseract = 0;
  bool _blocked_async = 0;
  bool _graphmat = 0;
  bool _preprocess = 0;

  // simulation details
  enum net_type _net;
  enum algo_type _algo;
  bool _prio_xbar = false;
  int _core_cnt = 16;
  int _num_rows = 4;
  int _num_banks = 32;
  int _l2size = 4096;
  int _task_queue_size = 512;
  int _bus_width = 16;
  int _num_tq_per_core = 1;
  bool _anal_mode = 0;
  int _lane_width = 1;
  int _mem_bw = 256; // FIXME: not sure if used
  int _scr_bw = 256; // FIXME: not sure if used

  bool _prefer_tq_latency = false;
  bool _inter_task_reorder = false;

  enum work_dist_type _work_dist;
  bool _work_stealing = 0; // TODO: not working...!!

  asic *_asic;
};

#endif

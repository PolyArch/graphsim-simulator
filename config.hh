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

  // these are the options to pick a spatial partitioning policy (only one of them should be set at a time)
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

  // these are crossbar features -- do not apply to PolyGraph's final design.
  int _crossbar_lat = 0;
  int _crossbar_bw = 1;
  bool _model_net_delay = 0;
  int _network = 1;
  bool _hrc_xbar = false;

  // these implement different kinds of network optimizations
  // 1. decomposable: multiple scalar networks
  // 2. worst_net: single scalar network
  // 3. ideal_net: this is depricated
  enum net_traffic_type _net_traffic;
  bool _decomposable = 0;
  bool _worst_net = 0;
  bool _ideal_net = 0;

  // these implements two kinds of multicast:
  // 1. real_multicast: this is the traditional algorithm where the packet is replicated when different destinations prefers different directions to minimize hops
  // 2. path_multicast: this is proposed in GraphVine where the packet is not replicated and traverses to minimize hops for only the destination at the top 
  bool _real_multicast = 0;
  bool _path_multicast = 0;

  // these configurations are used for ideal experiments
  // with perfect_net, all packets injected into the network reach their destination without any bandwidth or latency constraints
  // with perfect_lb, new update packet is sent to the next available bank irrespective of data mapping.
  bool _perfect_net = 0;
  bool _perfect_lb = 0;

  // these configurations are for the computation datapath
  // this is used for vector workloads where latency to perform computation may be large
  // this latency is used to resolve bank conflicts (conflicting requests are delayed by 8 cycles)
  int _dfg_length = 8;
  // this is the vector width of "process" computation
  int _process_thr = 1;
  // this implies the pull/push variant in graph processing
  bool _pull_mode = 0;

  // memory controller
  int _pred_thr = 0;
  // if set to 0, it will do atomic simulation of main memory where response is received using certain latency and bandwidth constraints.
  // if set to 1, it will use DRAMSim2 to simulate main memory
  bool _dramsim_enabled = 0;
  // it models request and response traffic from the requesting core to the corresponding memory bank. It becomes significant when dealing with large scale systems.
  bool _numa_contention = 0;
  // if set, it uses synchronization on the returned edge data. It will wait for all edge data to arrive, so it can batch them for multicast.
  // if reset, each edge would be requested as a separate data element.
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
  // is set, each core gets a private cache.
  // otherwise, there will be shared cache across all cores. 
  bool _banked = 0;
  // this is depricated
  bool _all_cache = 0;
  // this is set when a scratchpad can be configured to a cache
  // it implements dynamic switching in the slice scheduling variant
  bool _resizing = 0;
  // it sets whether to use LRU or MRU replacement policy
  bool _lru = 0;
  // if unset, we would like to cache vertex property only
  // if set, we would like to cache edge data as well
  bool _edge_cache = 0;  
  bool _special_cache = 0; // TODO: this should be combined, separate modules?!!
  // if set, the simulation will use cache mode throughout the algorithm
  bool _working_cache = 0;
  // if set, caches are not updated in case of a miss
  // this is deprecated -- may not be correct implementation
  bool _phi = 0;

  // these model the hypothetical cases where all are hits or misses in the cache
  bool _allhits = 0;
  bool _allmiss = 0;

  // these represent the number of temporal slices of a graph
  int _slice_count = 4;
  // this applies only to graph-synchronous and slice-synchronous variants
  // the parameters represent the number of times a slice should be executed before switching to next slice
  // it is the parameter for locality slice scheduling variant
  int _slice_iter = 1;

  // this implies random spatial partitioning scheme: this is worst case without any preprocessing
  bool _random = 0; // TODO: not sure if this works!!
  // this specifies whether to use metis partitioning results
  bool _metis = 0;

  // task controller
  // this implements an old optimization that implements a kind of "local" work-stealing
  // If the task queue is empty, it can steal data from the overflow buffer of any of the nearby cores
  // nearby cores help to not lose locality too much while maintaining load balance
  bool _dyn_load_bal = false;
  // this function implements the load balance on the source vertex side
  // edge data returned from memory is uniformly distributed across cores irrespective of which source cores the request originated from.
  bool _edge_load_bal = false;
  // if set, it inserts limit on the parallelization of the computation datapath
  // this includes task queue throughput and vector width of the prefetch and process units
  bool _prac = 0;
  // this is an old optimization where we implemented propagation blocking
  // vector updates are pushed into the aggregation buffer at the rate of 'vector_width_updates/cycle'. Then, this buffer is drained at the rate of task_queue_enqueue_throughput
  // This parameter specifies whether aggregation buffer shoud be popped in fifo fashion or priority order 
  bool _presorter = false;
  // this is deprecated
  bool _reorder = 0; // FIXME: not sure!!

  // types of scheduling policies
  // this implements the work-efficiency optimized slice scheduling algorithm variant
  bool _abcd = false;
  // this is deprecated!
  bool _distance_sched = 1;
  // if set, it implement the creation order vertex scheduling algorithm variant
  // if unset, this implements the work-efficiency optimized vertex scheduling algorithm variant
  bool _fifo = 0;
  
  // these implement different kinds of vertex coalescing policies
  // this coalesces the new active vertex if it has already been activated and not pushed into the hardware task queue yet (see coalescing in the PolyGraph paper)
  bool _entry_abort = 0;
  // this is deprecated but the idea was similar to GraphPulse paper where new updates to the same vertex may be performed separately without doing scratchpad access
  // the implications are similar to vertex coalescing except here instead of dropping, "reduce" computations like min needs to be performed.
  bool _update_coalesce = 0;
  // this is deprecated, it was used earlier when task queues used source vertex property from the original property
  // but in new design, we read updated vertex properties when pushing tasks from the overflow queue to the hardware task queue (TODO (@vidushi): confirm!) 
  bool _abort = 0;

  bool _central_batch = true;

  // this specifies the number of reserved entries in task queue for high priority tasks
  int _high_prio_reserve = 0;

  // taxonomy: this way will help us cover all workloads -- Do we want to
  // change taxonomy somehow?
  enum update_visibility _update_visible;
  enum task_sched_type _task_sched;
  enum slice_sched_type _slice_sched; // abcd, round-robin, reuse

  // graph specific details
  // this is used to pick an algorithm variant automatically
  int _graph_dia = 1;
  // this is used when preprocessing the graph (in preprocess.cpp)
  bool _undirected = 0;
  bool _unweighted = 0;
  bool _csr = 0;

  // algorithm specific details
  // this should be true for all dense frontier algorithms
  bool _pr = false;
  // if true, BFS is executed. if false, SSSP is executed.
  // Please note that both BFS and PR should not true, otherwise they will lead to undefined behavior.
  bool _abfs = false;
  // this represents connected components
  bool _acc = false;
  // this is deperecated. but it is a special shortest path where the source vertices also have a weight.
  bool _astar = false;
  // this should be true for all vector graph algorithms.
  bool _cf = false;
  // For GCN, both this and CF should be true.
  // For CF, only CF sould be true.
  bool _gcn = false;
  // this implies triangle counting
  bool _tc = false;
  // it can be true only with GCN
  // Ladies GCN is a graph sampling strategy that will prune the graph to the assigned number of nodes
  // the resulting graph is much more dense compared to the original graph as this algorithm keeps high degree vertices around
  bool _ladies_gcn = false;
  // this implies whether we would like to simulate the matrix-multiplication phase of GCN
  // Please note that this simulator is based on graph's vertex processing template.
  // Therefore, it analytically adds fixed number of cycles to model matrix-multiplication.
  bool _gcn_matrix = false;
  // this is deprecated but it was defined to simulate synchronous algorithms where new vertices should be activated during a synchronization phase
  bool _sync_sim = 0;

  // these are algorithm parameters
  // this is source vertex for single source shortest path algorithm
  int _src_loc = 0;
  // this implies number of layers in the GCN model
  int _gcn_layers = 0;
  // this implies feature length in GCN or CF
  int _feat_len = 8;
  // deprecated
  // this was implemented for vector algorithms when "reduce" is commutative and order across elements of the vector is not desired.
  // otherwise a reorder buffer will be required to reorder data from the network.
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
  // deprecated?
  int _reuse = 1;

  // execution model
  enum exec_model_type _exec_model;
  // this is asynchronous with no-slice algorithm variant
  bool _sgu = 0;
  // this is asynchronous with slicing algorithm variant
  bool _sgu_slicing = 0;
  // this is graph-synchronous or slice-synchronous algorithm variant with round-robin slicing
  bool _graphmat_slicing = 0;
  // this is graph-synchronous with no-slice algorithm variant
  bool _graphmat = 0;
  // this is slice-synchronous with with locality slice algorithm variant
  bool _blocked_async = 0;
  // these are deprecated
  bool _swarm = 0;
  bool _espresso = 0;
  bool _graphlab = 0;
  bool _sgu_hybrid = 0;
  bool _tesseract = 0;

  bool _preprocess = 0;

  // simulation details
  enum net_type _net;
  enum algo_type _algo;
  bool _prio_xbar = false; // models a case with priority ordering in bank queues
  int _core_cnt = 16; // total number of cores
  int _num_rows = 4; // number of rows in the mesh
  int _num_banks = 32; // number of scratchpad banks
  int _l2size = 4096; // l2 cache size
  int _task_queue_size = 512; // local task queue size
  int _bus_width = 16; // network width is 16 bytes
  int _num_tq_per_core = 1; // maintain 1 core with throughput of 1 enqueue/dequeue per 2 cycles
  bool _anal_mode = 0; // analysis mode, skip before simulation
  int _lane_width = 1; // number of independent lanes in a core, deprecated
  int _mem_bw = 256; // maximum memory bandwidth, used for atomic simulation of memory
  int _scr_bw = 256; // deprecated, number of banks indicate scratchpad bandwidth

  bool _prefer_tq_latency = false;
  bool _inter_task_reorder = false;

  enum work_dist_type _work_dist;
  bool _work_stealing = 0; // TODO: not working...!!

  asic *_asic;
};

#endif

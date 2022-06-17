#include "asic_core.hh"
#include "network.hh"
#include "stats.hh"
#include "config.hh"
#include "common.hh"
#include "asic.hh"

config::config(asic* host) : _asic(host) {
#if SWARM==1 || TESSERACT==1
  _lane_width=LANE_WIDTH;
#endif
  // dynamic graphs
  _update_batch=UPDATE_BATCH;
  _dyn_graph=DYN_GRAPH;
  _seq_edge=SEQ_EDGE;
  _inc_dyn=INC_DYN;
  _recomp_dyn=RECOMP_DYN;
  if(_inc_dyn) _dyn_algo=inc;
  else _dyn_algo=recomp;

  // hybrid execution
  _slice_hrst=SLICE_HRST;
  _heuristic=HEURISTIC;
  _cache_no_switch=CACHE_NO_SWITCH;
  _async_no_switch=ASYNC_NO_SWITCH;
  _hybrid_eval=HYBRID_EVAL;
  _hybrid=HYBRID; /*not sure*/

  // stats
  _detailed_stats=DETAILED_STATS;
  _reuse_stats=REUSE_STATS;
  _profiling=PROFILING;

  // network module
  _blocked_dfs=BLOCKED_DFS;
  _dfsmap=DFSMAP;
  _bfs_map=BFS_MAP;
  _modulo=MODULO;
  _linear=LINEAR;
  _bdfs=BDFS;
  _bbfs=BBFS;
  _random_spatial=RANDOM_SPATIAL;
  _noload=NOLOAD;

  if(_random_spatial) _spatial_part=random_spatial;
  else if(_linear) _spatial_part=linear;
  else if(_modulo) _spatial_part=modulo;
  else if(_dfsmap) _spatial_part=dfs_map;
  else if(_bfs_map) _spatial_part=bfs_map;
  else if(_bdfs) _spatial_part=bdfs;
  else if(_bbfs) _spatial_part=bbfs;
  else if(_blocked_dfs) _spatial_part=blocked_dfs;
  else if(_noload) _spatial_part=noload;
  else _spatial_part=linear; // default mapping

  _bd=1000;
  _hrc_xbar=HRC_XBAR;
  _crossbar_lat=CROSSBAR_LAT;
  _crossbar_bw=CROSSBAR_BW;
  _model_net_delay=MODEL_NET_DELAY;
  _network=NETWORK;
  if(_network==0) _net=crossbar;
  else _net=mesh;
  if(_hrc_xbar) _net=hrc_xbar;
  _prio_xbar=PRIO_XBAR;

  _decomposable=DECOMPOSABLE;
  _worst_net=WORST_NET;
  _ideal_net=IDEAL_NET;
  _perfect_net=PERFECT_NET;

  _real_multicast=REAL_MULTICAST;
  _path_multicast=PATH_MULTICAST;

  // TODO: this should really be a network packet type (for graphs, there is
  // only 1 category)
  // FIXME: we have to make sure anything is not missed because decomposable is
  // not ON
  if(_decomposable) _net_traffic=decomposable;
  if(_real_multicast) _net_traffic=real_multicast;
  if(_path_multicast) _net_traffic=path_multicast;
  if(_ideal_net) _net_traffic=ideal_net;

  _perfect_lb=PERFECT_LB;
  // TODO: PERFECT_LB is weird!!!

  // computation module
  _dfg_length=DFG_LENGTH;
  _process_thr=process_thr;
  _pull_mode=PULL;
   
  // memory controller
  _pred_thr=PRED_THR;
  _dramsim_enabled=DRAMSIM_ENABLED;
  _numa_contention=NUMA_CONTENTION;
  _cache_hit_aware_sched=CACHE_HIT_AWARE_SCHED;
  _mult_cache_hit_aware_sched=CACHE_HIT_AWARE_SCHED;
#if GCN==1
  _mult_cache_hit_aware_sched=0;
#endif
  _all_edge_access=ALL_EDGE_ACCESS;
  _hash_join=HASH_JOIN;
  _tree_trav=TREE_TRAV;
  if(_hash_join) {
    _domain=hashjoin;
  }
  if(_tree_trav) {
    _domain=tree;
  }
  _hats=HATS;
  _hybrid_hats_cache_hit=HYBRID_HATS_CACHE_HIT;

  // cache
  _banked=BANKED;
  _all_cache=ALL_CACHE;
  _resizing=RESIZING;
  _edge_cache=EDGE_CACHE;
  _special_cache=SPECIAL_CACHE; // TODO: this should be combined, separate modules?!!
  _working_cache=WORKING_CACHE;
  
  _lru=LRU;
  _phi=PHI;
  _allhits=ALLHITS;
  _allmiss=ALLMISS;
  if(_lru) _cache_repl=lru;
  else if(_phi) _cache_repl=phi;
  else if(_allhits) _cache_repl=allhits;
  else if(_allmiss) _cache_repl=allmiss;
  else _cache_repl=lru; // default

  // scratch
  _slice_iter=SLICE_ITER;
  _slice_count=SLICE_COUNT;
  _random=RANDOM; // TODO: not sure if this works!!
  _metis=METIS;

  // task controller
  _prac=PRAC;
  _presorter=PRESORTER;
  _reorder=REORDER; // FIXME: not sure!!
  // types of scheduling policies
  _abcd=ABCD;
  _distance_sched=DISTANCE_SCHED;
  _fifo=FIFO;
  _central_batch=CENTRAL_BATCH;
  _entry_abort=ENTRY_ABORT;
  _update_coalesce=UPDATE_COALESCE;
  _abort=ABORT;
  _dyn_load_bal=DYN_LOAD_BAL;
  _edge_load_bal=EDGE_LOAD_BAL;

  _prefer_tq_latency=PREF_TQ_LAT;

  // TODO: are they mutually exclusive?

  if(_abcd) _task_sched=abcd;
  else if(_distance_sched) _task_sched=datadep;
  else if(_fifo) _task_sched=fifo;

  _high_prio_reserve=HIGH_PRIO_RESERVE;

  // graph specific details
  _graph_dia=GRAPH_DIA;
  _undirected=UNDIRECTED;
  _unweighted=UNWEIGHTED;
  _csr=CSR;

  // algorithm specific details
  _pr=PR;
  _abfs=ABFS;
  _acc=ACC;
  _gcn=GCN;
  _cf=CF;
  _tc=TC;
  _ladies_gcn=LADIES_GCN;
  _astar=ASTAR;

  _algo=sssp;
  if(_pr) _algo=pr;
  if(_abfs) _algo=bfs;
  if(_acc) _algo=cc;
  if(_cf) _algo=cf;
  if(_gcn) _algo=gcn;
  if(_tc) _algo=tc;
  if(_astar) _algo=astar;

  _gcn_matrix=GCN_MATRIX;
  _sync_sim=SYNC_SIM;
  
  _src_loc=SRC_LOC;
  _gcn_layers=GCN_LAYERS;
  _feat_len=FEAT_LEN;
  _sgu_gcn_reorder=SGU_GCN_REORDER; /*FIXME: do we need this? or is it working?*/

  _graphmat_cf=GRAPHMAT_CF;
  _batched_cores=BATCHED_CORES;

#if AGG_MULT_TYPE==1
  _agg_mult_type=fine;
#elif AGG_MULT_TYPE==2
  _agg_mult_type=coarse;
#endif


#if MULT_AGG_TYPE==1
  _mult_agg_type=fine;
#elif MULT_AGG_TYPE==2
  _mult_agg_type=coarse;
#endif

  _heter_cores=HETRO;

  // deprecated
  _gcn_swarm=GCN_SWARM;
  _cf_swarm=CF_SWARM;
  _lazy_cycles=LAZY_CYCLES;
  _dep_check_depth=DEP_CHECK_DEPTH;
  _chronos=CHRONOS;

  // algorithm optimizations
  _sync_scr=SYNC_SCR; // TODO: something like depth-wise GCN
  _dyn_reuse=DYN_REUSE;
  _extreme=EXTREME; // FIXME: required?
  _reuse=REUSE;

  // simulation details
  _core_cnt=core_cnt;
  _num_rows=num_rows;
  _num_banks=num_banks;
  _l2size=L2SIZE;
  _task_queue_size=TASK_QUEUE_SIZE;
  _bus_width=bus_width;
  _num_tq_per_core=NUM_TQ_PER_CORE;
  _anal_mode=ANAL_MODE;
  _mem_bw=mem_bw; // FIXME: not sure if used
  _scr_bw=scr_bw; // FIXME: not sure if used

  _work_stealing=WORK_STEALING; // TODO: not working...!!
  if(_work_stealing) _work_dist=work_steal;
  else _work_dist=addr_map; // FIXME: this is wrong

  // execution models
#if SGU==1
  _sgu=1;
  _exec_model=async;
#endif
#if SGU_SLICING==1
  _sgu_slicing=1;
  _exec_model=async_slicing;
#endif

  // have to be in sequence because of my declaration
#if GRAPHMAT==1
  _graphmat=1;
  _exec_model=sync;
#endif
#if GRAPHMAT_SLICING==1
  _graphmat_slicing=1;
  _exec_model=sync_slicing;
#endif
#if SWARM==1
  _swarm=1;
  _exec_model=swarm;
#endif
#if ESPRESSO==1
  _espresso=1;
  _exec_model=espresso;
#endif
#if GRAPHLAB==1
  _graphlab=1;
  _exec_model=graphlab;
#endif
#if SGU_HYBRID==1
  _sgu_hybrid=0;
  _exec_model=async_hybrid;
#endif
#if TESSERACT==1
  _tesseract=1;
  _exec_model=tesseract;
#endif
#if BLOCKED_ASYNC==1
  _blocked_async=1;
  _exec_model=blocked_async;
#endif
// #if DYN_GRAPH==1
//   _exec_model=dyn_graph;
// #endif

  // there can be 3x3 options (TODO: I need to set these options as well!!
  // also, can i try to combine the original code around this -- may be better
  // data)
  // TODO: implement task_sched = vid
  // Okay, slice sheduling schemes can be implemented in "simulate_slice" and
  // other schemes would determine: 1. data structure, 2. whether or not to create tasks dynamically, 3. sync steps (copy and create new tasks or reload worklist data -- these factors should be considered)
  /*if(_update_visible==synch && _slice_sched==round) {
   * if(_config->_algo==pr && new_task.dist==0) continue;
    _exec_model = sync_slicing;
    _reuse = 1;
  } else if(_update_visible==synch && _slice_sched==locality) {
    _exec_model = sync_slicing;
    _reuse = REUSE;
  } else if(_update_visible==synch && _slice_sched==priority) {
    _exec_model = sync_slicing;
    _reuse = 1;
    _abcd = 1;
  } else if(_update_visible==asynccoarse && _slice_sched==round) {
    _exec_model = blocked_async;
  } else if(_update_visible==asynccoarse && _slice_sched==locality) {
    _exec_model = blocked_async; // TODO: implement it and get data!!
    _reuse = REUSE;
  } else if(_update_visible==asynccoarse && _slice_sched==priority) {
    _exec_model = blocked_async;
    _abcd = 1;
  } else if(_update_visible==asyncfine && _slice_sched==round) {
    assert(0 && "async will create tasks, cannot schedule in round-robin"); // NO SENSE
  } else if(_update_visible==asyncfine && _slice_sched==locality) {
    _exec_model = async_slicing;
  } else if(_update_visible==asyncfine && _slice_sched==priority) {
    _exec_model = async_slicing;
    _abcd = 1;
  }*/
  _inter_task_reorder = INTERTASK_REORDER;

#if PREPROCESS==1
  _preprocess=1;
#endif

}

bool config::is_async() {
  // return (_exec_model==async || _exec_model==async_slicing || _exec_model==blocked_async);
  return (_exec_model==async || _exec_model==async_slicing); // || _exec_model==blocked_async);
}

bool config::is_sync() {
  return (_exec_model==sync || _exec_model==sync_slicing);
}

bool config::is_vector() {
  return (_algo==cf || _algo==gcn || _algo==tc);
}

bool config::is_slice() {
  return (_exec_model==async_slicing || _exec_model==sync_slicing || _exec_model==blocked_async || _exec_model==dyn_graph);
}

bool config::is_non_frontier() {
  return (_algo==pr || _algo==cc || _algo==cf || _algo==gcn || _algo==tc);
}

bool config::no_swarm() {
  bool swarm = _swarm || _espresso || _gcn_swarm;
  return !swarm;
}

#ifndef _ASIC_CORE_H
#define _ASIC_CORE_H

#include "asic.hh"
#include "common.hh"

class asic;
class asic_core;
class network;
class stats;

class asic_core
{
  friend class asic;

public:
  asic_core();

  // Reading operand 2 in process stage
  // this function reads data of first operand either from memory or scratchpad
  virtual void set_src_dist(task_entry cur_task, pref_tuple &next_tuple);

  // this is deprecated over the above function
  virtual void set_dist_at_dequeue(task_entry &cur_task, pref_tuple &next_tuple);

  // Dynamic task creation stage
  // this function pushes the new task at a fixed task enqueue rate
  // for asynchronous slice variants, it also checks whether the updates belong to the current working slice or not. If no, new updates are pushed to the buffer in memory
  virtual void create_async_task(DTYPE priority_order, task_entry new_task);
  
  // Aggregation buffer stage
  // this is deprecated for now, it only works as a latency hiding buffer
  virtual void push_live_task(DTYPE priority_order, task_entry new_task);
  virtual void arob_to_pref();

  // Prefetching stage
  // first two function: access edges either independently or together (so that we multicast them together)
  // Third function: access source vertex property for synchronous algorithms that maintain two copies
  virtual int access_edge(task_entry cur_task, int edge_id, pref_tuple next_tuple);
  virtual int access_all_edges(int line_addr, task_entry cur_task, pref_tuple next_tuple);
  virtual void access_src_vertex_prop(task_entry cur_task, pref_tuple next_tuple);

  // consumes the data received from memory. LSQ keeps the returned edges.
  virtual void push_lsq_data(pref_tuple cur_tuple);
  virtual void consume_lsq_responses(int lane_id);
  
  
  // Task queue dispatch: initial stage
  // It pops from the tasks and pushes to the prefetch stage
  // If the task queue has space, it pulls new tasks from the overflow buffer
  virtual void dispatch(int tqid, int lane_id);
  // this function pops tasks from the overflow buffer and inserts them into the hardware task queue using the latest vertex property from scratchpad
  virtual void fill_task_queue();

  // For pull variants, we might need to read vertex property, of destination vertices, from the remote scratchpad. This function send remote scratchpad read request
  virtual void pull_scratch_read(); // Should be sent to pull after this...
  void push_scratch_data(pref_tuple cur_tuple);
  void push_scratch_data_to_pipeline();

  // Process computation stage
  virtual void process(int lane_id);

  // Reduce task creation stage
  // Here the returned edge data is encapsulated as task and sent as independent tasks over the network
  // Or multiple edge data are combined to multicast update tasks over the network
  virtual void reduce(int lane_id);

  // Reduce computation stage
  // This stage reads data from scratchpad and updates it using the new vertex property
  virtual void serve_atomic_requests();
  // it is the same as old function, but if the vertex property is updated, it creates the new task instantly
  virtual void spec_serve_atomic_requests();
  
  // Deprecated.
  // When new vertices are activated, they may be pushed to the aggregation buffer before the task queue.
  virtual void drain_aggregation_buffer(int index);
  virtual void arbit_aggregation_buffer(int index);
  
  // these make packets during the reduce operation to be sent over the network
  virtual void send_multicast_packet(pref_tuple cur_tuple, int start_offset, int end_offset);
  virtual void send_scalar_packet(pref_tuple cur_tuple);

  virtual void commit(); // int lane_id);// {};
  virtual bool cycle()
  {
    cout << "Wrong cycle function\n";
    return 0;
  };
  // these are special functions for vector workloads
  
  // the update operation now consumes latency higher than 1 cycle.
  // this function considers the latency and throughput of updating a vector
  virtual void insert_vector_task(pref_tuple cur_tuple, bool spawn, int dfg_id = -1);
  // this function pushes the updates to the next stage when latency number of cycles have passed
  virtual void forward_atomic_updates(int dfg_id, int lane_id);
  // this function implements a multicast packet where payload is a vector
  virtual void generate_gcn_net_packet(pref_tuple cur_tuple, int start_offset, int end_offset);

  // reduce now needs to send vector multicast update packets
  virtual void cf_reduce(int lane_id, int dfg_id = -1);
  // this checks for conflicts when executing multiple update tasks
  virtual bool check_atomic_conflict(int lane_id, int feat_len, int cur_vid);

  // these check whether no data is available in the datapath pipeline
  // this is a condition to complete the program
  virtual bool pipeline_inactive(bool show);
  virtual bool local_pipeline_inactive(bool show);

  // implements the queues between datapath pipeline stages
  virtual bool can_push_in_prefetch(int lane_id);
  virtual bool can_push_in_process(int lane_id);
  virtual bool can_push_in_reduce(int lane_id);
  virtual bool can_push_in_local_bank_queue(int bank_id);
  virtual bool can_push_in_gcn_bank();
  virtual bool can_push_to_cgra_event_queue();
  virtual bool serve_priority_hits();
  
  void access_cache_for_gcn_updates(pref_tuple cur_tuple, int start_offset, int end_offset);
  virtual bool can_serve_hit_updates();
  virtual bool can_serve_miss_updates();
  virtual void split_updates_in_cache();
  // virtual queue<pref_tuple>* select_queue_for_cache_hit();
  virtual bool select_queue_for_cache_hit();
  virtual bool is_queue_head_hit(deque<pref_tuple> *target_queue);
  virtual void push_gcn_cache_hit(pref_tuple cur_tuple);
  virtual void push_gcn_cache_miss(pref_tuple cur_tuple);

  virtual void issue_requests_from_pending_buffer(int lane_id);
  virtual void forward_multiply_opn(int lane_id);
  
  virtual void empty_crossbar_input();
  virtual void automatic_partition_profile();
  virtual void update_affinity_on_evict(int evict_vid, int old_core, int new_core);
  virtual int get_evict_core(int evict_vid);
  virtual int choose_eviction_cand();

  virtual void recursively_erase_dependent_tasks(int dst_id, int tid, int dst_timestamp, int rem_depth);
  virtual void delete_one_timestamp(int dst_id, int tid, int dst_timestamp);

  // this implements the break optimization for the pull algorithm variant
  virtual void trigger_break_operations(int vid);
  virtual int allocate_cb_entry_for_partitions(bool first_entry, int dfg_id, int index, bool last_entry);

  virtual void insert_prefetch_process(int lane_id, int priority, pref_tuple cur_tuple);
  virtual void pop_from_prefetch_process();
  virtual void serve_prefetch_reqs();

  // utility functions
  // int find_left_with_src_id(int src_id);
  // void reduce_both(task_id t); // reduces both finished and committed
  // void reduce_committed(task_id t); // reduces both finished and committed
  // void reduce_finished(task_id t);

public:
  int _fine_grain_throughput = process_thr;

  // const int STORE_LEN = DFG_LENGTH*FEAT_LEN/_graph_verticesEC_LEN;
  // pair<int,bool> _pending_stores[DFG_LENGTH*FEAT_LEN/_graph_verticesEC_LEN]; // address stored at cycle i

  // const int STORE_LEN = DFG_LENGTH*256*256*256/_graph_verticesEC_LEN;
  // pair<int,bool> _pending_stores[LANE_WIDTH][DFG_LENGTH*256*256*256/_graph_verticesEC_LEN]; // address stored at cycle i
  // pref_tuple _pending_mirror[LANE_WIDTH][DFG_LENGTH*256*256*256/_graph_verticesEC_LEN];

  int _last_requested_src_id = -1;
  int _edges_read_sent = 0;

  pair<int, bool> _pending_stores[LANE_WIDTH][STORE_LEN];
  pref_tuple _pending_mirror[LANE_WIDTH][STORE_LEN];
  bool _break_this_cycle = false;
  const int num_packets = FEAT_LEN * message_size / bus_width;
  const int _agg_fifo_length = AGG_FIFO_DEP_LEN;

  // cycles when it is empty
  int _cycles_mem_edge_empty = 0;
  int _cycles_reorder_full = 0;
  int _cycles_net_full = 0;

  // int _cur_cycle_ptr=0;
  int _busy_cycle = 0;
  int _tq_empty = 0;
  int _cb_full = 0;
  int _no_slot_for_disp = 0;
  bool _odd_cycle = false;
  int _current_edges = 0; // used for accumulating pull mode values (assuming flexible network that can merge any values)

  int _prev_core_migrated = 0;
  int _last_break_id = -1; // for direction-optimizing bfs
  int _last_processed_id = -1;

  int _mem_responses_this_slice = 0;
  int _mem_requests_this_slice = 0;
  int _hit_queue_empty_cycles = 0;
  bool _hit_queue_was_empty_last_time = false;
  int _edges_done_this_slice = 0; // required for asynchronous implementations

  queue<pending_mem_req> _pending_swarm[LANE_WIDTH];
  bool _swarm_pending_request[LANE_WIDTH];

  struct gcn_update
  {
    int vid;
    bool spawn;
    pref_tuple prod_tuple;
    bool second_buffer = false;
    int ready_cycle;
    float prio;
    int thr = 0;
    gcn_update(int v, bool s, pref_tuple p, int r, float norm)
    {
      vid = v;
      spawn = s;
      prod_tuple = p;
      ready_cycle = r;
      prio = norm;
    }
  };
  // required to check for conflict
  list<gcn_update> _cgra_event_queue[MAX_DFGS];
  int _cur_free_cycle[MAX_DFGS];
  int _last_dfg_id = 0;
  list<gcn_update> _coarse_cgra_event_queue;
  int _coarse_cur_free_cycle = 0;

  // float _pend_counter=1;
  // int _pend_counter=FEAT_LEN/_graph_verticesEC_LEN;

  // reduce commit is sorted as timestamp (should I do this as
  // a priority queue?)
  // queue<task_entry> _task_queue[MAX_TIMESTAMP];
  // map<int, queue<task_entry>> _task_queue[LANE_WIDTH];
#if DISTANCE_SCHED == 1
  // can I ensure that the list is sorted? this could help crossbar case better
  map<DTYPE, list<task_entry>> _task_queue[NUM_TQ_PER_CORE][LANE_WIDTH];
#else
  // want to delete in between? No, I guess -- make new imp
  list<task_entry> _task_queue[LANE_WIDTH];
  // queue<task_entry> _task_queue[LANE_WIDTH];
#endif

  task_entry _pending_task[NUM_TQ_PER_CORE];
  queue<task_entry> _fifo_task_queue;
  queue<pref_tuple> _pref_lsq[LANE_WIDTH];
  // deque<pref_tuple> _prefetch_process[LANE_WIDTH]; // this should be a priority queue
  map<int, list<pref_tuple>> _prefetch_process[LANE_WIDTH];

  queue<pref_tuple> _prefetch_scratch;
  queue<pref_tuple> _scratch_process;
  queue<red_tuple> _process_reduce[LANE_WIDTH];

  queue<pref_tuple> _hol_conflict_queue;
  int _prev_start_time[LANE_WIDTH];

  cb_entry _atomic_rob[AROB_SIZE]; // FIXME: hardware needs to make sure that it doesn't overflow
  queue<pref_tuple> _crossbar_input;
  int _cur_arob_pop_ptr = 0;
  int _cur_arob_push_ptr = 0; // location where new entry should be pushed

  // it should check in all cores: probably have common queues only
  // FIXME: fix this for dijkstra
  // queue<spec_tuple> _reduce_commit[LANE_WIDTH]; // finished tasks

  // 4 data structures required from espresso
  unordered_map<int, meta_info> _meta_info_queue;
  unordered_map<int, int> _conflict_queue;
  // unordered_map<int,commit_info> _commit_queue;
  // priority_queue<commit_pair, vector<commit_pair>, greater<commit_pair>> _commit_queue;
  map<int, queue<commit_info>> _commit_queue;

  // TODO: (source id, (invalid flag, assigned lane), (core id, dst_id) of its dest tasks)
  // (source id, (invalid flag, tid), (core id, dst_id) of its dest tasks)
#define dPair pair<pair<bool, int>, vector<forward_info>>
  multimap<int, dPair> _dependence_queue; // can allow required number of entries now
                                          // (tid, (invalid flag, vid), (core id, dst_id) of its dest tasks)
                                          // unordered_map<int, pair<pair<bool,int>,vector<forward_info>>> _dependence_queue;

  // only 1 vid allowed at a time (vid, (cycles_left,red_tuple)
  // age (multimap), vid search in all, red_tuple
  // If I do this also in order of timestamp? and prefer ready nodes -- help
  // us to move closer to dijkstra
  // (timestamp, list (cycles_left, red_tuple)
#if REORDER == 1
  map<int, list<pair<int, red_tuple>>> _stall_buffer;
#else
  list<pair<int, red_tuple>> _stall_buffer;
#endif
  // aggregation buffer
  list<pair<int, task_entry>> _aggregation_buffer[NUM_TQ_PER_CORE];
  int _agg_push_index = 0; // its value ranges from 0 to NUM_TQ
  int _agg_pop_index = 0;  // its value ranges from 0 to NUM_TQ
  // global current highest priority
  int _local_min_dist = INF;

#if ABFS == 1
  const int _max_elem_mem_load = mem_bw / (2 * sizeof(int)); // TODO: change it for graphmat
#elif GRAPHMAT == 1
  const int _max_elem_mem_load = mem_bw / (3 * sizeof(int)); // (weight, dst id, src_dist) -- we save it?
                                                             // const int _max_elem_mem_load = mem_bw/(2*sizeof(int)); // (weight, dst id, src_dist) -- we save it?
#else
  const int _max_elem_mem_load = mem_bw / (2 * sizeof(int)); // size of edge prop (at least weight, dst id)
#endif
  const int _max_cache_bw = 16; // 64 bytes
  // const int _max_elem_scr_load = scr_bw/(core_cnt*sizeof(int)); // size of vertex_prop

  // #if NETWORK == 1
  // int _local_scratch[LOCAL_SCRATCH_SIZE];
  // list<red_tuple> _local_bank_queues[num_banks]; // to model the crossbar
  queue<red_tuple> _local_bank_queues[num_banks]; // to model the crossbar
  deque<pref_tuple> _priority_hit_gcn_updates;    // for GCN
  deque<pref_tuple> _hit_gcn_updates;             // for GCN
  deque<pref_tuple> _miss_gcn_updates;            // for GCN
  bool _slice_execution = false;
  // #endif

  // Information about espresso or tesseract
  bool _in_order_flag[LANE_WIDTH]; // =true;
  float _waiting_count[LANE_WIDTH];
  // tracking cold misses for each core on dest vertex id
  bool _is_hot_miss[MAX_CACHE_LINES];

  // actively used only in espresso
  int _active_process_thr = process_thr;
  // int _active_process_thr=num_banks;
  // required only for tesseract, send requests over the network only which
  // are edge miss (let's still keep the assumption of 4 mem ctrl)
  // bool _is_edge_hot_miss[E/8];
  // int _dep_check_depth=0;
  // int _oldest_dependence_id=-1;

  int _core_id;
  asic *_asic;
};

class graphmat : public asic_core
{

public:
  graphmat(asic *host)
  {
    _asic = host;
    for (int i = 0; i < STORE_LEN; ++i)
    {
      for (int j = 0; j < LANE_WIDTH; ++j)
      {
        _pending_stores[j][i] = make_pair(-1, false);
      }
    }
    for (int j = 0; j < LANE_WIDTH; ++j)
    {
      _swarm_pending_request[j] = false;
    }
    for (int i = 0; i < MAX_DFGS; ++i)
    {
      _cur_free_cycle[i] = 0;
    }
  }
  virtual bool cycle();
};

// asynchronous bellman ford: option -- not considering for now
// Note: We will do asynchronous dijkstra - vertex-centric model
// Note: We are not able to capture it extra predictability of data access
// efficiency:  TODO: (we need to consider prefetch latency for that)
// 1 idea: _src_vertex_data[_graph_vertices] and _dst_vertex_data[_graph_vertices] => 2 copies to avoid conflicts
// challenge to model rd-wr conflicts here (data cannot be read when it is being atomically updated)
class graphlab : public asic_core
{

public:
  graphlab(asic *host)
  {
    _asic = host;
  }
  virtual bool cycle();
};

// Note: this is dijkstra parallel (we do same timestamp tasks in parallel)
class dijkstra : public asic_core
{

public:
  dijkstra(asic *host)
  {
    _asic = host;
  }
  virtual void commit();
  virtual bool cycle();
};

// model in-order core + cache hierarchy (somehow need to model memory system
// -- model same as network)
class swarm : public asic_core
{

public:
  swarm(asic *host)
  {
    _asic = host;
    for (int i = 0; i < STORE_LEN; ++i)
    {
      for (int j = 0; j < LANE_WIDTH; ++j)
      {
        _pending_stores[j][i] = make_pair(-1, false);
      }
    }
    for (int j = 0; j < LANE_WIDTH; ++j)
    {
      _swarm_pending_request[j] = false;
    }
  }
  virtual bool cycle();
};

class sgu : public asic_core
{

public:
  sgu(asic *host)
  {
    _asic = host;
    for (int i = 0; i < AROB_SIZE; ++i)
    {
      _atomic_rob[i].valid = false;
    }
  }

  virtual bool cycle();
  // virtual void reduce2(int lane_id);
  // virtual void cf_reduce();
};

class espresso : public asic_core
{

public:
  espresso(asic *host)
  {
    _asic = host;
  }

  virtual bool cycle();
};

/*class cf : public asic_core {

  public:
  cf(asic* host) {
    _asic=host;
  }

  virtual bool cycle();
};*/

#endif

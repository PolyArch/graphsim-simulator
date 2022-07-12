#ifndef _ASIC_H
#define _ASIC_H

#include "simple_cache.h"
#include "asic_core.hh"
#include "multiply.hh"
#include "network.hh"
#include "stats.hh"
#include "config.hh"
#include "common.hh"

class asic_core;
class network;
class stats;
class asic;

// FIXME: ensure cache-hit-aware is different for local and remote coalescer..
class coalescing_buffer
{
public:
  coalescing_buffer(asic *host, int core);
  void cycle();
  void insert_leaf_task(int leaf_id, int req_core, int cb_entry, int dfg_id);
  void free_batch_entry(int index_into_batched);
  void fill_updates_from_overflow_buffer();
  void update_priorities_using_cache_hit();
  bool pipeline_inactive(bool show);
  void push_local_read_responses_to_cb(int index_into_batched);
  void send_batched_read_requests(int leaf_id, int index_into_batched);
  void send_batched_read_response(int index_into_batched);
  void send_batched_compute_request(int leaf_id, pref_tuple cur_tuple, int index_into_batched);
  int get_avg_batch_size();
  pair<int, int> pop();
  // pair<int, int> peek();

public:
  // int _cur_local_coalescer_ptr=0;
  map<int, list<pair<int, int>>> _priority_update_queue; // pointer and leaf id (leaf_id should be in number of batched)
  int _entries_in_priority_update = 0;
  int _total_current_entries = 0;
  int *_num_batched_tasks_per_index; // updated from a remote task (store existance here)

  remote_read_task *_data_of_batched_tasks[BATCH_WIDTH];
  queue<remote_read_task> _overflow_for_updates; // leaf_id (but we need req_core, cb_entry)
  queue<int> _batch_free_list;
  bitset<1000> _is_leaf_present; // bitset<1000> _is_leaf_valid; // valid can be different entries??

  int _batch_length = 256; // 16; // FIFO_DEP_LEN;
  int _batch_width = BATCH_WIDTH;
  float _multicast_batch_size = MULTICAST_BATCH; // if 0, set it to 0.5
  const int _dot_unit = FEAT_LEN;
  const int num_cache_lines_per_weight = ceil(_dot_unit / float((line_size / message_size)));
  const int num_cache_lines_per_leaf = (NUM_DATA_PER_LEAF * message_size) / line_size;
  int _core_id = -1;
  asic *_asic;
};

class completion_buffer
{
public:
  completion_buffer();
  // This function pushes the data received from memory (edge data here) to a fifo buffer called lsq which next pushes the data to the process stage of the vertex processing pipeline. 
  void cb_to_lsq();
  // Here we studied an optimization where completion buffer is split into partitions with each maintaining edges of a single vertex 
  void cbpart_to_lsq();
  // This function checks allocates an entry in completion buffer before sending a request to main memory.
  // Arguments:
  // Waiting count: This indicates number of entries to be allotted. It also helps to model implicit synchronization, for example, when the computation should start when the required number of elements have arrived.
  int allocate_cb_entry(int waiting_count);
  // This function deallocates the entry when the data is ready to be pushed to the computation pipeline.
  int deallocate_cb_entry(int waiting_count);
  // This function check whether there are available entries in the completion buffer.
  // If not, new memory request cannot be send.
  bool can_push();
  bool can_push(int reqd_entries);
  bool pipeline_inactive(bool show);
  int peek_lsq_dfg();
  // This function receives the information from memory. It also contains the meta information that includes operand value (in dataflow, we would know by one-to-one mapping), and source core location to direct data from memory controller to its correct completion buffer entry. This is traditionally implemented in memory controllers. 
  pref_tuple receive_lsq_responses();
  // pref_tuple insert_in_pref_lsq(pref_tuple next_tuple);
  // This function checks whether the computation pipeline has space to receive the data responses.
  bool can_push_in_pref_lsq();

public:
  // common interface for this? (and define multiple cb instances for each?)
  int _cur_cb_pop_ptr = 0;
  int _cur_cb_push_ptr = 0; // location where new entry should be pushed
  int _entries_remaining = COMPLETION_BUFFER_SIZE;
  int _cb_size = COMPLETION_BUFFER_SIZE;
  int _core_id = -1;

  cb_entry _reorder_buf[COMPLETION_BUFFER_SIZE];
  // just wait for whole, not send one-by-one in this case
  vector<pair<int, int>> _cb_start_partitions[MAX_DFGS]; // static configurable constant
  vector<int> _cur_start_ptr[MAX_DFGS];
  queue<pair<int, bool>> _free_cb_parts; // dynamically variable (Second is true for dynamic)

  queue<pref_tuple> _pref_lsq;
};

// includes cache and access to dramsim
class memory_controller
{
public:
  memory_controller(asic *host, stats *stat);
  void empty_delay_buffer();

  void receive_cache_miss(int line_addr, int req_core_id);
  void update_cache_on_new_access(int core_id, int edge_id, int cur_prio_info);
  void reset_compulsory_misses();
  // These are interface functions for DRAMSim2
  static void power_callback(double a, double b, double c, double d);
  void read_complete(unsigned data, uint64_t address, uint64_t clock_cycle);
  void write_complete(unsigned id, uint64_t address, uint64_t clock_cycle);

  // this function checks whether any request for the same cache line is pending. It coalesces those requests so that the same response can be used for all requests.
  bool send_mem_request_actual(bool isWrite, uint64_t line_addr, pref_tuple cur_tuple, int streamid);
  // this function checks whether the request is a cache hit/miss. If hit, it is served by the cache. Otherwise, it sends request to main memory using DRAMSim2.
  bool send_mem_request(bool isWrite, uint64_t line_addr, pref_tuple cur_tuple, int streamid);
  // When using cache algorithm variant, for any request to memory, it check whether the request is a cache hit or not.
  bool is_cache_hit(int core, Addr_t paddr, UINT32 accessType, bool updateReplacement, UINT32 privateBankID);
  // this function captures the overhead to load data in scratchpad at program initiation
  void load_graph_in_memory();
  // this function prints the statistics related to memory like cache hit rate, memory bandwidth utilization, etc. For detailed explanation, check stats.hh
  void print_mem_ctrl_stats();

  // this function performs "address translation". Here we assume simple placement of data-structures in memory.
  uint64_t get_vertex_addr(int vertex_id);
  uint64_t get_edge_addr(int edge_id, int vid);

  void initialize_cache();
  void schedule_pending_memory_addresses();

public:
  bool *_is_edge_hot_miss = NULL;
  queue<pair<uint64_t, pref_tuple>> _pending_mem_addr[MAX_STREAMS]; // TODO: associate data-structures with the strem id
  map<int, list<pref_tuple>> _outstanding_mem_req;                  // FIXME: should there by multiple outstanding reqs?
  int _num_pending_cache_reqs = 0;
  SIMPLE_CACHE *_l2_cache;                  // global cache for now
  SIMPLE_CACHE *_banked_l2_cache[core_cnt]; // global cache for now
  bool _banked = false;
  int _first_access_cycle[1]; // FIXME: not needed now
  int _cur_writes = 0;
  queue<pending_mem_req> _l2cache_delay_buffer[CACHE_LATENCY + 1];

  int64_t _l2hits = 0;
  int64_t _l2hits_per_core[core_cnt];
  uint64_t _l2accesses_per_core[core_cnt];
  int64_t _l2coalesces = 0;
  uint64_t _l2accesses = 0;
  uint64_t _prev_l2hits = 0;
  uint64_t _prev_l2accesses = 0;
  int _cur_cache_delay_ptr = 0;

  // Define address offsets for different data-structures
  uint64_t _edge_offset = 0;
  uint64_t _vertex_offset = 0;
  uint64_t _weight_offset = 0;
  int _mem_reqs_this_cycle = 0;

  // data structure for cache
  // direct-mapped cache - tag, cache line (pair could be a vector for associaive)
  // index: core_id, set_id
  struct cache_set _local_cache[core_cnt][num_sets][assoc];

  asic *_asic;
  stats *_stats;
  MultiChannelMemorySystem *_mem;
  MultiChannelMemorySystem *_numa_mem[core_cnt];
};

// accessing task queues (TODO: what about local task queue?)
// TODO: how to size various pipeline buffers for equivalent throughput
class task_controller
{
public:
  task_controller(asic *host);

  int get_cache_hit_aware_priority(int core_id, bool copy_already_present, int pointer_into_batch, int leaf_id, int batch_width, int num_in_pointer);
  void task_queue_stealing();
  void reset_task_queues();

  // Tasks that do not fit in hardware task queue, are stored in main memory in fifo fashion.
  // This function pulls those tasks from memory to hardware task queue.
  // Currently this function do not model the cycles taken to read these tasks from memory
  void pull_tasks_from_fifo_to_tq(int core_id, int tqid);
  // this function checks whether hardware task queues have space
  bool can_pull_from_overflow(int core_id);
  DTYPE calc_cache_hit_priority(int vid);
  // this function inserts a new task in the hardware priority queue with "priority"
  // A single core can have multiple task queues (indexed by tqid), they are selected in round-robin fashion for new task insertion.
  void insert_new_task(int tqid, int lane_id, int core_id, DTYPE priority, task_entry cur_task);
  void insert_global_task(DTYPE timestamp, task_entry cur_task);
  void insert_local_task(int tqid, int lane_id, int core_id, DTYPE priority, task_entry cur_task, bool from_fifo = false);

  void insert_local_coarse_grained_task(int core, int row, int weight_rows_done, bool second_buffer);
  int find_min_coarse_task_core();
  int find_max_coarse_task_core();
  void insert_coarse_grained_task(int row);
  bool can_push_coarse_grain_task();
  mult_task schedule_coarse_grain_task();

  // This function implements the slice scheduling variant where a new slice may be chosen in round-robin fashion or using a work-efficiency-optimized priority order
  int chose_new_slice(int old_slice);

  // PolyGraph implement also maintains worklist for each of the graph slices
  // When switching a graph slice, all active tasks in the hardware task queue or the overflow fifo queue, are sent to the worklist.
  void push_task_into_worklist(int wklist, DTYPE priority, task_entry cur_task);
  // This function checks whether there are any pending tasks left for the current slice
  bool worklists_empty();
  // When the new slice starts execution, this function loads the worklist tasks to the task queue
  int move_tasks_from_wklist_to_tq(int slice_id);
  // This functions returns total number of pending tasks in a program 
  int tot_pending_tasks();

  // TODO: not sure what they are doing?
  void distribute_lb();
  void distribute_prio();
  void distribute2();
  void distribute_one_task_per_core();
  int get_middle_rank(int factor);

  void central_task_schedule();

  bool is_local_coarse_grain_task_queue_full(int core_id);
  bool is_central_coarse_grain_task_queue_full();
  // functions related to abort

  int get_live_fine_tasks();
  int get_live_coarse_tasks();

public:
  // same task queue?

  queue<task_entry> _pending_coarse_buffer; // pending agg tasks for double buffer 2
  queue<mult_task> _coarse_task_queue;
  map<DTYPE, list<task_entry>> _global_task_queue;
  queue<pref_tuple> _pending_updates;
  // distributed?
  queue<pair<DTYPE, task_entry>> _worklists[SLICE_COUNT];
  int _remaining_task_entries[core_cnt][NUM_TQ_PER_CORE];

  int _remaining_local_coarse_task_entries[core_cnt];
  int _remaining_central_coarse_task_entries;

  // not sure why they are here...!!!
  int _which_core = 0;
  int _num_edges = 0;
  int _avg_timestamp = MAX_TIMESTAMP; // -1;
  int chose_smallest_core();
  int chose_nearby_core(int core_id);

  int _tasks_moved = 0;
  int _tasks_inserted = 0;

  bitset<V> _present_in_queue;
  bitset<V> _present_in_local_queue[core_cnt];
  // this includes both actual requests, and the pending memory requests
  // so set at push, unset when memory request is served
  // change logic of pushing into this queue (make a common function)
  int *_present_in_miss_gcn_queue;
  bitset<V> _check_worklist_duplicate[SLICE_COUNT];
  bitset<V> _check_vertex_done_this_phase;

  asic *_asic;
};

// bank queue and spatial partitioning
// or map_grp_to_core??
/* PENDING IDEAS:
 * NEW IDEAS:
 */
class scratch_controller
{
public:
  scratch_controller(asic *host, stats *stat);
  void push_dangling_vertices(bool second_buffer);
  // this function performs the spatial partitioning and renames the vertices such that vertices is each partition are ordered
  void perform_spatial_part();
  // this function implements the chunk-based partitioning in Gemini
  void perform_linear_load_balancing(int slice_id);
  // this function implements modulo partitioning where vertices are interleaved at a single vertex granularity
  void perform_modulo_load_balancing(int slice_id);
  // this function performance bdfs traversal as in HATS paper
  void perform_bdfs_load_balancing(int slice_id);
  // this function purely optimizes for load balancing where it creates partitions such that each has similar number of low, medium and high degree vertices
  void perform_load_balancing2(int slice_id);
  // this is similar to above but splits vertices in only two categories: low and high degree
  void perform_load_balancing(int slice_id);
  // this function uses the traditional METIS for spatial partitioning as well
  void optimize_load();
  // above spatial partitioning policies only renames the vertices. this function maps those to the cores. We tried different placement so that partitions that have more dependencies are spatially close to each other, but it is not longer active.
  void map_grp_to_core();
  // this function prints statistics to test how much does the current spatial partitioning policy optimizes for locality.
  // Statistic is the ratio of remote edges to local edges.
  void locality_test(int slice_id);
  // this function performs linear mapping, similar to linear_load_balancing
  // TODO (@vidushi): remove redundancy
  void linear_mapping();

  // these are utility functions to implement the recursive graph traversal algorithms
  void bdfs_algo(int s);
  void BDFSUtil(int v);
  void DFS(int s, int slice_id);
  void DFSUtil(int v);
  void BFS(int s, int slice_id);

  // this is an old function where we attempted partitioning by using the probability of a vertex being accessed
  void read_mapping();

  // this function converts the metis output to a graph traversal algorithm
  void generate_metis_format();
  void read_mongoose_slicing();
  void read_metis_slicing();

  // this function prints the statistics for the number of nodes that have edges outgoing to a different slice
  // this statistic is to test when to switch slices
  // TODO (@vidushi): these three functions also seem redundant. Check!
  void get_sync_boundary_nodes();
  void get_linear_sgu_boundary_vertices();
  void get_slice_boundary_nodes();

  // Note: These functions are hacks to model memory request traffic and latency overheads.
  // We assume that there are four memory controllers for 16 PolyGraph cores.
  // This function returns where in the system should request be routed to talk to the corresponding memory controller.
  int get_mc_core_id(int dst_id);
  // This function returns out of four memory controllers, where should the memory request be directed to
  int get_mem_ctrl_id(int dst_id);
  // This function maps the graph traversal to the scratchpad banks.
  // Specifically, it implements cluster size in multi-level spatial partitioning.
  int get_grp_id(int dst_id);
  // this function internally calls get_grp_id to return the mapped core.
  int get_local_scratch_id(int dst_id, bool compute_placement = true);
  int avg_remote_latency();
  // this function implements the hilbert algorithm so that the partitions are placed in cores so that consecutive partitions are spatially close to each other
  int get_hilbert_core(int cluster_id);
  void rot(int n, int *x, int *y, int rx, int ry);

  // this function considers how a graph's spatial partitioned in placed across scratchpad banks.
  // this function assumes global scratchpad across cores, where cores communicate to this scratchpad via giant crossbar.
  int get_global_bank_id(int dst_id);
  // this function return the bank id within the scratchpad bank at a core
  int get_local_bank_id(int dst_id);
  // this function implements a special data mapping scheme that stochastically minimizes bank conflicts (important in graph processing)
  int use_knh_hash(int i);
  // this returns the slice number using the spatial partitioning policy
  int get_slice_num(int vid);
  // this functions distributes the vertices in buckets with different degrees. It can potentially be used for future load balancing strategies.
  void fix_in_degree();
  // these functions are deprecated, these print statistics of the number of unique vertices that have incoming edges from different sources (add all those sources)
  void graphicionado_slice_fill();
  void sgu_slice_fill();
  // this function tests the effectiveness of the spatial partitioning policy in terms of the total number of incoming and outgoing edges within a slice
  void load_balance_test();
  // this function prints the output of spatial partitioning policy: original vertex id and renamed vertex id.
  void print_mapping();

  // this function sends and receives memory requests from the scratchpad
  // these implement the pull variant in graph processing
  void send_scratch_request(int req_core_id, uint64_t scratch_addr, pref_tuple cur_tuple);
  void receive_scratch_request();

public:
  int _slice_size = 0;
  int _metis_slice_size = 0;
  int *_mapping;
  int *_mapping_bfs;
  int *_mapping_dfs;

  // track the shared dynamic state
  bitset<V> _is_visited; // a vertex should be committed only once (dijkstra)
  bool *_bfs_visited;
  // int *_cluster_elem_done;
  bool *_visited; // for dfs
  int *_map_grp_to_core;

  int _vertices_visited = 0;
  int _edges_visited = 0;
  int _global_ind = -1;
  int *_num_spec;
  int *_num_boundary_nodes_per_slice;
  unordered_map<int, int> _old_vertex_to_copy_location[SLICE_COUNT];
  bitset<V> _node_done;
  int *_num_slices_it_is_copy_vertex;
  vector<pair<int, vector<int>>> _map_boundary_to_slice;
  int _scratch_reqs_this_cycle = 0;
  queue<net_packet> _net_data;

  vector<int> low_degree;
  vector<int> high_inc_degree;
  vector<int> high_out_degree;

  int *_mapping_new_to_old;
  int *_mapping_old_to_new;
  int *_slice_vids[SLICE_COUNT];
  uint16_t *_slice_num;
  int *_temp_edge_weights;
  int *_in_degree; // [SLICE_COUNT];
  bitset<SLICE_COUNT> *_done;
  bitset<V> _present_in_bank_queue[num_banks];

  asic *_asic;
  stats *_stats;
};

class asic
{

public:
  asic();

  void test_mult_task();
  
  bool is_leaf_node(int vid);
  void seq_edge_update_stream(int cur_edges, int prev_edges, int original_graph_vertices);
  void rand_edge_update_stream(int part);
  void assign_rand_tstamp();
  void reset_algo_switching();
  void vertex_memory_access(red_tuple cur_tuple);
  void graphmat_slice_configs(red_tuple cur_tuple);
  
  // initializes vertex property data structure
  void init_vertex_data();
  // generates random numbers that may be required for initialization in ML
  // algorithms like machine learning (ML) and collaborative filtering (CF)
  double randdouble();
  // fills correct vertex property to check whether the accelerator calculated
  // the correct answer
  // this applies only to workloads with fixed answer lke BFS, SSSP, and CC.
  int fill_correct_vertex_data();
  // reads input graph file which is in the format <src_id, dst_id, wgt>.
  // TODO (@vidushi): For certain graphs, we have hardcoded some patterns. We
  // will make them read modes.
  void read_graph_structure(string file, int *offset, edge_info *neighbor, bool initialize_vd, bool is_csc_file);
  
  int get_degree(int vid);
  void ladies_sampling();
  void write_csr_file();
  int get_middle_rank(int factor);
  void distribute_one_task_per_core();
  
  // insert initial tasks to implement a workload.
  void initialize_simulation();
  // this function is used to calculate per-cycle statistics to study dynamic
  // behavior and do cycle-level performance debugging
  void update_stats_per_cycle();

  void central_task_schedule();
  int reconfigure_fine_core(int core_id, int cores_to_mult);
  void flush_multiplication_pipeline(int core_id);
  void update_heterogeneous_core_throughput();
  
  // it prints intermediate metrics to know how the algorithm is progressing
  // it is useful for simulations that take long time
  void print_simulation_status();

  void check_async_slice_switch();
  
  void schedule_tasks(float &edges_served, int &max_edges_served, float &lbfactor);
  // it is called at coarse-grained phases to check whether an algorithm should
  // be switched
  void perform_algorithm_switching(int slice_id, int mem_eff);
  int calc_dyn_reuse();

  // prints the error in collaborative filtering -- it is useful to track the
  // algorithm convergence
  void print_cf_mean_error();

  void simulate_dyn_graph();

  void call_simulate();
  // It is a central function that calls all architecture components.
  // It also performs data and task orchestration when switching slices
  void simulate();

  void simulate_ideal();
  
  // It checks whether the vertex property is updated enough to active
  // a vertex.
  // It also checks whether reuse_factor allows to activate this new vertex now
  // or later.
  bool should_spawn_sync_task(int vid, int reuse_factor);
  void initialize_stats_per_graph_round();
  int task_recreation_during_sync();
  // It reads the pending tasks corresponding to a slice_id into current task
  // queue. It also checks whether to push new tasks in the worklist.
  // Returns: whether any tasks are pushed for execution.
  int task_recreation_during_sync(int slice_id, bool push_in_worklist, int reuse_factor);
  void update_gradient_of_dependent_slices(int slice_id);

  void synchronizing_sync_reuse_lost_updates(int reuse_factor);
  // This functions implements the functionality of control core where it calls
  // slices according to "slice scheduling" variant and also performs
  // bookkeeping when switching slices.
  void simulate_slice(); // trying to write a general function for different slice scheduling and update visibility techniques to find any bugs/opportunities
  void simulate_sgu_slice();
  // TODO: write this function
  void simulate_graphmat_slice();
  void simulate_blocked_async_slice();

  // It prints final statistics and check for the correctness of the simulated
  // algorithm.
  void finish_simulation();

  // These functions implement workload-specific properties.
  // First, they return the computation output of three function in vertex processing template: process on edges,
  // reduce on vertex property,and apply for propagating updates.
  // Should_spawn_task returns whether a new vertex can be updated.
  DTYPE cf_update(int src_id, int dst_id, DTYPE edge_wgt);
  DTYPE process(DTYPE edge_wgt, DTYPE dist);
  DTYPE reduce(DTYPE old_value, DTYPE new_value);
  DTYPE apply(DTYPE new_dist, int updated_vid);
  bool should_spawn_task(float a, float b);
  bool should_cf_spawn_task(int vid);
  // These two functions are parameters to the vertex reduce function.
  // Returns: latency/throughput of the operation performed in reduce.
  int get_update_operation_latency();
  int get_update_operation_throughput(int src_id, int dst_id);
 
  // utility functions
  bool correctness_check();
  void print_output();
  void load_balance_test();
  void print_extra_work_info();
  void print_local_iteration_stats(int slice_id, int reuse);
  void print_stats();
  void slicing_stats();
  bool no_update();
  int calc_least_busy_core();
  void push_pending_task(DTYPE priority, task_entry cur_task);
  void push_dummy_packet(int src_core, int dest_core, bool two_sided);
  void print_sensitive_subgraph();

  // Spatial partitioning modifies vertex_id such that vertices in a single
  // partition also have vertex IDs closeby.
  // This function distributes them in partition, and considers slight variants
  // is partition size.
  // Returns: Scratchpad bank a destination will will be stored.
  int get_grp_id(int dst_id);
  void save_reuse_info();

  void generate_metis_format();
  int check_new_slice(int old_slice);

  // cache functions
  void update_cache_on_new_access(int core_id, int edge_id, int cur_prio_info);
  bool should_abort_first(DTYPE a, DTYPE b);

  // These functions interface to DRAMSim2.
  // Read_complete is called when the response for an address is available in
  // the "data" variable at DRAMSim2's clock_cycle
  // Write_complete is called on writes but instead of data, it returns id (?)
  void read_complete(unsigned, uint64_t, uint64_t);
  void write_complete(unsigned, uint64_t, uint64_t);

  // This function simulates initial operations like loading data from memory
  // to scratchpads.
  void load_graph_in_memory();
  // When a request is miss in the cache, certain hints to the scheduler may be
  // sent (not applicable to PolyGraph)
  void receive_cache_miss(int line_addr, int req_core_id);
  void slice_init();

  void read_mongoose_slicing();
  // This function reads the output from METIS partitioner and updates
  // vertex_id such that each temporal slice has vertices with sequential ID.
  // This helps to use modulo functions for both spatial scheduling and
  // blocking updates to vertices in different slices.
  void read_metis_slicing();
  // This function implement KNH hash mapping of data (indexed by i) across 16 banks. 
  // Returns: bank id where i is stored.
  int use_knh_hash(int i);
  // void serve_mshr_requests();
  void common_sampled_nodes();

  void print_mapping();
  void reset_compulsory_misses();

  void scalability_test(int scale, int delay);
  void execute_func(int src_core, int func, red_tuple cur_tuple);
  void cycle_pending_comp();
  bool can_push_to_bank_crossbar_buffer();

  bool can_push_in_global_bank_queue(int bank_id);
  bool should_break_in_pull(int parent_id);
  void calc_and_print_training_error();
  void insert_global_bank(int bank_id, DTYPE priority, red_tuple cur_tuple);

  void generate_memory_accesses(default_random_engine generator, normal_distribution<double> distribution);

  // FIXME: not sure when to use protected/private
public:
  int _graph_vertices = -1;
  int _non_dangling_graph_vertices = 0;
  vector<int> _dangling_queue;
  int _graph_edges = -1;
  // bool _is_high_degree[_graph_vertices]; // useful for power-law graphs
  int _max_elem_dispatch_per_core = 0;

  int _prev_commit_id = 0;        // src location
  int _prev_commit_core_id = 0;   // src location
  int _prev_commit_timestamp = 0; // src location
  int _virtual_finished_counter = 0;
  int _which_disp_ptr = 0;
  int _elem_dispatch = 0;
  int _dep_check_depth = 0;
  int _work_steal_depth = 0;
  uint64_t _cur_cycle = 0;
  uint64_t _max_iter = (((uint64_t)1) << 63);
  uint64_t _add_on_cycles = 0;
  uint64_t _bdary_cycles = 0;
  uint64_t _extreme_cycles = 0;

  int _space_allotted = 0;
  int _slicing_threshold = 0;
  int _current_slice = 0;
  int _current_reuse = 0;

  int _avg_task_time = 0;

  int _last_bank = -1;
  int _last_bank_per_core[core_cnt];
  int _last_core = -1;
  int _tot_banks = -1;      // num_banks*core_cnt;
  int _banks_per_core = -1; // num_banks*core_cnt;
  int _cur_mshr_ptr = -1;

  int _slice_count = 0;
  int _metis_slice_count = 0;

  int _tot_edges_done = 0;

  // graph data structures, TODO: data size would change with algorithm
  int *_correct_vertex_data;
  int *_correct_vertex_data_double_buffer;
  DTYPE *_vertex_data_vec[FEAT_LEN];
  DTYPE *_vertex_data;
  int *_offset;
  int *_mod_offset;
  edge_info *_neighbor;

  // still needed for pull implementation
  int *_csr_offset;
  edge_info *_csr_neighbor;

  int *_mapped_core;
  int *_map_vertex_to_core;
  int *_edge_freq;

  const int _max_elem_mem_load = mem_bw / (1 * sizeof(int));  // size of edge prop (at least weight, dst id)
  const int _max_elem_mem_load2 = mem_bw / (1 * sizeof(int)); // size of edge prop (at least weight, dst id)

  int *_atomic_issue_cycle; // to maintain read-write dependencies (3 cycle lock)
  bool *_update;            // to converge graphlab
#if PRIO_XBAR == 0
  list<red_tuple> _bank_queues[num_banks]; // to model the crossbar
#else
  map<DTYPE, list<red_tuple>> _bank_queues[num_banks]; // to model the crossbar
#endif
      // stats
  // bool _edge_traversed[E];
  // queue<pair<int, int>> _unique_edges_traversed;
  int *_inc_freq[core_cnt];
  // int _stat_tot_mem_loads=0; // same as above
  int _tot_active_cycles[core_cnt];

  // only abort of duplicated tasks
  int *_mismatch_slices; // number of slices its edges are incident on
  bitset<SLICE_COUNT> *_counted;

  int *_gcn_minibatch; // [MINIBATCH_SIZE];
  int *_gcn_updates;
  int *_prev_gcn_updates;
  // std::queue<std::pair<int,int>> _mshr[CACHE_LATENCY];

  int _slice_size_exact[SLICE_COUNT];
  int _sampled_nodes[GCN_LAYERS + 1][LADIES_SAMPLE];

  // need to store laplacian matrix for each GCN layer in graph format, for this simulator, these should be copied to the original versions at the start of each layer
  vector<edge_info> _sampled_neighbor[GCN_LAYERS]; // edges unknown
  vector<int> _sampled_offset[GCN_LAYERS];
#if LADIES_GCN == 1
  int _interim_matrix[V][LADIES_SAMPLE];
#endif
  bitset<LADIES_SAMPLE> _is_dest_visited;
  int _slice_fill_addr[SLICE_COUNT];
  // list<pair<pair<int, int>, task_entry>> _per_cycle_tasks;
  int _src_vid = SRC_LOC;

  int _graphmat_extra_traffic = 0;
  int _graphmat_required_traffic = 0;

  // this is a location in memory, update graph so that copy vertices are
  // _graph_vertices+x...
  // <vid, current_slice> -> x (paddr = _graph_vertices+x)
  // vector<int> _copy_vertices[SLICE_COUNT][SLICE_COUNT];

  int _debug_local = 0;
  int _debug_remote = 0;
  int _transactions_this_cycle = 0;
  int _receipt_this_cycle = 0;
  vector<pair<int, DTYPE>> _delta_updates[REUSE]; // stored in the sparse format

  uint64_t _global_iteration = 0;
  uint64_t _local_iteration = 0;
  uint64_t _tot_mem_reqs_per_iteration = 0;
  uint64_t _cycles_per_iteration = 0;
  uint64_t _edges_last_iteration = 0;
  uint64_t _edges_created_last_iteration = 0;
  uint64_t _coarse_tasks_last_iteration = 0;
  uint64_t _coarse_created_last_iteration = 0;
  uint64_t _cycles_last_iteration = 0;
  uint64_t _start_active_vertices = 0;
  uint64_t _start_copy_vertex_update = 0;
  DTYPE _grad[SLICE_COUNT];

  bool _finishing_phase = false;
  bool _async_finishing_phase = false;
  bool _extreme_iterations = false;
  int _num_extreme_iterations = 0;

  queue<red_tuple> _bank_to_crossbar[MAX_NET_LATENCY];
  queue<red_tuple> _router_crossbar[MAX_NET_LATENCY];
  int _pending_updates = 0;

  int _bank_queue_to_mem_latency = 0;
  int _bank_xbar_ptr = 0;
  int _router_xbar_ptr = 0;
  int _delayed_net_tasks = 0;
  int _xbar_bw = 1;
  int _async_tipping_point = -1;
  int _sync_tipping_point = -1;
  int _cache_tipping_point = -1;
  int _active_vert_last_iteration = 0;
  int _mem_eff_last_iteration = 0;

  bool _switched_cache = false;
  bool _switched_async = false;
  // Meaning: 1. no dynamic tasks (go to serve_atomic_tasks), 2. go to phase
  // 2 task creation
  bool _switched_sync = false;
  bool _switched_bulk_async = false;
  int _cur_used_vert = 0; // used for dynamic graphs
  int *_assigned_tstamp = 0;
  edge_info *_original_neighbor; // TODO: can we make neighbor array as the pointer?
  int _half_vertex = 0;
  int _tot_map_ind[8];
  int _dyn_batch_size = 0;
  int _dyn_tot_parts = 0;
  bool _inc_comp = false;

  DTYPE *_scratch;               // [SCRATCH_SIZE]; // this should be malloc actually
  DTYPE *_scratch_vec[FEAT_LEN]; // this should be malloc actually
  int _edges_served = 0;
  int _tot_useful_edges_per_iteration = 0;

  task_controller *_task_ctrl;
  memory_controller *_mem_ctrl;
  scratch_controller *_scratch_ctrl;

  completion_buffer *_compl_buf[core_cnt];
  completion_buffer *_coarse_compl_buf[core_cnt][MAX_DFGS];
  // hopefully it works: only latency changes (different because definitely
  // different data-structures and address location)
  completion_buffer *_coarse_reorder_buf[core_cnt][MAX_DFGS];
  completion_buffer *_fine_reorder_buf[core_cnt];
  asic_core *_asic_cores[core_cnt];
  multiply *_mult_cores[core_cnt];
  coalescing_buffer *_remote_coalescer[core_cnt];
  coalescing_buffer *_local_coalescer[core_cnt];
  bool _gcn_even_phase = true;
  int _even_phase_matrix_mult = 0;
  int _odd_phase_matrix_mult = 0;
  int _fine_alloc[core_cnt]; // throughput assigned
  int _coarse_alloc[core_cnt];
  int _last_dispatch_core = 0;
  int _stat_bank_coalesced = 0;
  bool _already_printed_stats = false;
  int _stat_extra_edges_during_pull_hack = 0;

  network *_network;
  stats *_stats;
  config *_config;

  DTYPE _prev_gradient = 0;
  int _unique_vertices = 0;
  DTYPE _prev_error = 0;
  int _last_cb_core[core_cnt / 4];
  queue<uint64_t> _hit_addresses;
  queue<uint64_t> _miss_addresses;
  int _unique_mem_reqs_this_phase = 0;
  int _cycles_this_phase = 0;

  int _unique_leaves = 0;
  int *_leaf_access_count;
  int _prev_leaf_id = 0;
  int _pending_hats_requests = 0;

  const int num_kdtree_leaves = knnData / NUM_DATA_PER_LEAF; // 132; // 66
  const int kdtree_depth = log2(num_kdtree_leaves);          // pow(2,kdtree_depth+1); // 6; // 11
  // bitset<num_kdtree_leaves> _is_leaf_done;
  bitset<469> _is_leaf_done;
  int _phase_cycle = 0;
  int _round = 0;
  const int max_rounds = 1;
  DTYPE _gama = gamma;
  DTYPE _pre_error = 1000;

  int _dfg_to_agg = core_cnt;
  int _dfg_to_mult = core_cnt;

  int _total_tasks_moved_during_reconfig = 0;
  bool _fine_inactive[core_cnt];
  bool _reconfig_flushing_phase = false;
};
#endif

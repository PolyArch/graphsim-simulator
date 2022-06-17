#ifndef _COMMON_H
#define _COMMON_H

#include <iostream>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <queue>
#include <stack>
#include <unordered_map>
#include <map>
#include <bitset>
#include <string.h>
#include <list>
#include <algorithm>
#include <assert.h>
#include <cmath>
#include "random"
#include "DRAMSim2/CommandQueue.h"
#include "DRAMSim2/BusPacket.h"
#include "DRAMSim2/AddressMapping.h"
#include "DRAMSim2/CSVWriter.h"
#include "DRAMSim2/DRAMSim.h"

// arob, local_flag
using namespace std;
using namespace DRAMSim;

#define default_dataset_path "/data/vidushi/polygraph_data/datasets/directed_power_law/"
#define MULTICAST_LIMIT 4
#define BDCAST_WAIT 1 // 40 // 2 // 4 // 0 // 2
#define MAX_DELAY 0 // 10
#define MEMSIZE 131072 // 8388608
#define NUMA_MEMSIZE 131072
#define NUMA_MEM 0

// #define SPATIAL_STRIDE_FACTOR 16

#define MAX_STREAMS 10
#define EDGE_SID 0
#define VERTEX_SID 1
#define WEIGHT_SID 2

#if FEAT_LEN==16 && GCN==1
#define STATS_INTERVAL 1000
#else
#define STATS_INTERVAL 10000
#endif

#define MAX_DEGREE 16384
#define DEFAULT_DATASET 1

// #if GCN==1
// #define BATCH_WIDTH (MULTICAST_BATCH)
// #else
// #define BATCH_WIDTH (4*BATCHED_CORES)
// #endif

// #define MAX_DFGS 2 // 2 // 32
// #define MULTICAST_BATCH 256
// #define NUM_KNN_QUERIES 1000
// #define NUM_DATA_PER_LEAF 1024
// #define KDTREE_DEPTH 6 // 11
//  #define NUM_KDTREE_LEAVES 132 // 66

// #define GIANT_XBAR_TO_BANK 0
// #define MESH_XBAR_TO_BANK 1
#define BANK_TO_XBAR 0
#define ROUTER_XBAR_TO_BANK 1
#define MAX_NET_LATENCY 33 // 129 // 33
// #define CROSSBAR_BW 1

#define IND_FACTOR 10
#define STEAL_VICTIM_LIM 2
#define STEAL_THIEF_COUNT 1 // 4
#define SLICE_NUM_ITER 500000
#define CACHE_LATENCY 1
#define INF 100000
#if CF==1 && TC==1
#define MAX_ITER (E*100)
#elif CF==1
#define MAX_ITER (E*3000)
#else
#define MAX_ITER (E*300)
#endif
#define SCRATCH_SIZE 4847600 // 1048576
#define LOCAL_SCRATCH_SIZE (SCRATCH_SIZE/core_cnt)
// can change this with the model to save memory
#define MAX_TIMESTAMP 100000000 // window size
#define MAX_LABEL 1000
#define NUM_MC (core_cnt) // 4
// depends on datatype size also
#define MAX_CACHE_LINES (V/16)
#if TESSERACT == 1
#define LANE_WIDTH 32
#else
#define LANE_WIDTH 4 // 8 
#endif
#define LINK_BW 16
#define MAX_DEP_QUEUE_SIZE 64
#define STALL_CYCLES 1
#define num_sets 1 // 16
#define assoc 1 // 4
#define line_size 64 // 32

#define COARSE_TASK_QUEUE_SIZE 2048000

#if ABFS==0 && CF==0 && ACC==0 // SSSP and PR
#define message_size 8
#else
#define message_size 4
#endif

#define MINIBATCH_SIZE (V)
#define SAMPLE_SIZE 1000000 // 10 // 64 // 10 // 64 // 10 // 64 // 10 // 64 // 10 // 64 // 10

// TODO: find constants for page rank
#define alpha 0.85
// 0.85 is used by people at google for decent convergence
// https://www.cise.ufl.edu/~adobra/DaMn/talks/damn05-santini.pdf
// https://www.cs.princeton.edu/~chazelle/courses/BIB/pagerank.htm
// #define epsilon 0.000001 //; // 5
// #define epsilon 0.0001 //; // 5
#define epsilon (0.001*EPSILON) // 0.01 // 0.001 // 01 //; // 5
#define gamma 0.0001 // 0.001
// #define epsilon 0.000001 //; // 5
// #define epsilon 0.00001 //; // 5
// #define epsilon 0.000000001 //; // 5
// 0.001 here http://infolab.stanford.edu/~taherh/papers/extrapolation.pdf
// this is like 10^-6 for around 85 iterations (might try on rome first)
#define DTYPE float

// for vector algorithms
#define VEC_LEN (bus_width)
#define STORE_LEN (DFG_LENGTH*FEAT_LEN*FEAT_LEN/VEC_LEN)

#define LADIES_SAMPLE 512 // 2708 // 512 // 256 // 512 // 256 // 512
#define FEAT_LEN1 (FEAT_LEN)
#define FEAT_LEN2 (FEAT_LEN)

// #define COMPLETION_BUFFER_SIZE 1024 // 256 (number of 4-byte elements)
// #define FIFO_DEP_LEN 1024
#define AROB_SIZE 1

#define MAX_CACHE_MISS_ALLOWED 16384 // 1024
#define MAX_COALESCES_ALLOWED 16384 // 1024 // 65536
#define MAX_NET_PACKETS_ALLOWED 1638400

typedef pair<int, int> iPair;
typedef pair<float, int> fPair;

// TODO: A lot of these structs and many of its arguments are not used now,
// delete them to save some memory
enum domain_type {graphs, tree, hashjoin};
enum algo_type {sssp, pr, bfs, cc, cf, gcn, ladies_gcn, astar, tc};
enum net_type {mesh, crossbar, hrc_xbar};
enum net_traffic_type {decomposable, worst_net, real_multicast, path_multicast, ideal_net};
// how can networks be both vector, multicast, etc? different networks for each
// type?
enum net_packet_type {vectorized, vectorizedRead, memRequest, memResponse, scalarRequest, scalarResponse, scalarUpdate, astarUpdate, dummy};
// data-structures for which I prefer different reorder/completion buffers
enum cb_type {fineMem, coarseMem, fineScratch, coarseScratch, dummyCB};

enum spatial_part_type {random_spatial, linear, modulo, dfs_map, bfs_map, bdfs, bbfs, blocked_dfs, noload, automatic}; // make sure the last one works!!
enum exec_model_type {dyn_graph, async, async_slicing, sync, sync_slicing, blocked_async, swarm, espresso, tesseract, graphlab, async_hybrid, dijkstra};
enum dyn_gen_type {seq_edge};
enum dyn_algo_type {inc, recomp};
enum task_sched_type {datadep, fifo, abcd, vertex_id};
enum slice_sched_type {roundrobin, locality, priority};
enum update_visibility {synch, asynccoarse, asyncfine};
enum sync_type {global, fine, coarse};
enum work_dist_type {addr_map, work_steal}; // will having them mutually exclusive fix them? , perfect_lb};
enum cache_repl_type {lru, phi, allhits, allmiss}; // TODO: assuming only a single cache, not copies?


struct remote_read_task {
  // int index_into_local;
  int req_core;
  int leaf_id; // only required for overflow buffer

  // only useful for local coalescer
  int dfg_id;
  int cb_entry; // serves as index_into_local for remote_read_task..
  // bool second_buffer; // return data needs this information? (should have been stored along with completion buffer entry)
  remote_read_task(int c, int d, int cb) {
    req_core=c; dfg_id=d; cb_entry=cb;
  }
};

/*
struct local_read_task {
  // int req_core;
  int dfg_id;
  int cb_entry;
  local_read_task(int d, int cb) {
    dfg_id=d; cb_entry=cb;
  }
};*/

struct cache_meta {
  int tag;
  int priority_info; // residual, lru info
  cache_meta() { tag=-1; }
  cache_meta(int x, int y) {
    tag=x; priority_info=y;
  }
};

struct cache_set {
  cache_meta meta_info;
  bool polluted_entry; // should be false by default
  vector<int> cache_line;
  vector<bool> first_access;
  cache_set() { polluted_entry=true; }
};

// TODO: add print status everywhere
struct task_id {
  int timestamp;
  int virtual_ind;
  // int core_id; // because multiple cores could be issuing at the same time (not sure how efficient this is)
  task_id() {}
  task_id(int x, int y) { // , int z) {
    timestamp=x; virtual_ind=y; // core_id=z;
  }
};

struct mult_task {
  int row;
  int weight_rows_done=0;
  int entry_cycle;
  bool second_buffer=false;
  int dfg_id=0;
};

struct mult_data {
  vector<int> vec1;
  vector<int> vec2;
  uint64_t insert_cycle=0;;
  int row;
  int dotp;
  int dfg_id=0;
  bool second_buffer;
  mult_data() {
    vec1.clear();
    vec2.clear();
  }
};

struct task_entry {
  int vid; // source id
  DTYPE dist; // priority or distance
  int delta_prio; // abcd-like priority
  
  // to keep track of work left -- only required for simulation purposes
  int start_offset;

  // target double buffer
  bool second_buffer=false;

  // all required for speculation -- these seem not usedul currently
// #if SWARM==1 || ESPRESSO==1
// #endif
  int tid=0;
  // int src_tile;
  // int parent_id;
  // int parent_vid;

  // set for work-stealing but not required now
  // bool stolen;
  // int degree;

  task_entry(int x, int y) {
    vid=x; start_offset=y; 
  }
  task_entry() {}
};

// TODO: make wgt datatype int as a macro
struct edge_info {
  int src_id; // for software
  int dst_id;
  DTYPE wgt;
  // int hops; // hops the edge is distributed around -- may be required for spatial partitioning policy??
  // bool done; // required to keep track of fine-grained work-eff
  // int id; // for extra work info
  // edge_info(int x, int y) {
  //   dst_id=x; wgt=y;
  // }
};

// this is the returned information required from a memory request (used to read vertex/edge data)
struct pref_tuple {

  int dfg_id=0;

  // returned data (these are the types)
  edge_info edge; // edge dst_id, wgt
  DTYPE src_dist; // maybe row_id for multiply
  DTYPE vertex_data[FEAT_LEN]; // vertex data for vector workloads
  
  // can be stored in the pipeline
  int src_id;
  int update_width;

  // information about the requesting core
  int cb_entry;
  int req_core;
  enum cb_type core_type; // DEFAULT: fine-grained
  bool second_buffer=false;
  // int lane_id;

  // for keeping track of what computation this belongs to -- should have been
  // in the pipeline
  int start_edge_served;
  int end_edge_served; // as row_size in multiply.cpp

  bool invalid=false; // useful in dataflow-based computations
  int tid=0;
  bool update=true; // FIXME: can be a part of width

  int repeat_times=1;
  bool last=false;

  pref_tuple() {}
  pref_tuple(int a, int b, int z) {
    edge.dst_id=a; edge.wgt=b; // tid=z;
  }
};

struct cb_entry {
  bool valid; // if the real data is here
  pref_tuple cur_tuple; // real data
  int waiting_addr; // only for simulation purposes to emulate the effect of vector port synchronization
  bool last_entry=false;
};

/*struct net_request_packet {
  int req_core_id;
  int cb_entry;

  int src_id; // final addr (to read)
  int dst_id; // final addr (to update)

  int dest_core_id;
  int label;

  int dfg_id=0;
  int tid=0;
  
  // for stats -- for keeping track of network latency
  int push_cycle;
};*/

// rename and then ds to net_packet; also check all calls to create_packet() ==
// destination core is slightly tricky
struct net_packet {
  // either of these three is used
  // information when data is returned back
  int req_core_id;
  int cb_entry;
  // int lane_id; -- this is a part of the destination

  bool second_buffer=false;

  // information about performing a function in single (one is addr, other is
  // to identify when sufficient has arrived, this is like the length stream --
  // can be replaced in the pipeline)
  int src_id; // final addr (to read)
  int dst_id; // final addr (to update)

  // data to send with the update function
  // DTYPE *vertex_data = NULL;
  DTYPE vertex_data[FEAT_LEN]={0};
  DTYPE new_dist=-1; // TODO: see its use...at least model update delay in the network????

  // information about performing a function in multicast
  // vector<int> multicast_dst;
  vector<iPair> multicast_dst_wgt;
  // vector<int> multicast_wgt; // this is similar to dst_od
 
  // redundant derivable factors -- should be from certain bits...
  int dest_core_id;
  vector<int> multicast_dst_core_id;
  vector<int> multicast_dfg_id;

  enum net_packet_type packet_type; // should consume different types of bandwidth and different functionality at network controller
  // for simulation purposes -- for encoding concurrency that a single packet must not be covered twice
  int label;

  bool last=false;
  int dfg_id=0;
  int tid=0;
  
  // for stats -- for keeping track of network latency
  int push_cycle;
  net_packet() { 
    packet_type=scalarUpdate; req_core_id=-1; dest_core_id=-1;
  }
};

// task_id is parent task id
struct red_tuple {

  // update value
  DTYPE vertex_data[FEAT_LEN];
  DTYPE new_dist;

  // update address
  int dst_id;
  int src_id;

  // unused I think -- these are for speculation maybe
  int tid;
  int src_tile;

  red_tuple() { }
  red_tuple(DTYPE x, int y) {
    new_dist=x; dst_id=y;
  }
  red_tuple(DTYPE transfer_vertex_data[FEAT_LEN], int y) {
    for(int i=0; i<FEAT_LEN; ++i) {
      vertex_data[i] = transfer_vertex_data[i];
    }
    dst_id=y;
  }
};

// child_task_id is the new entry to commit queue (it should be the index)
/*struct spec_tuple {
  task_id parent_tid;
  task_id own_tid;
  int finished_left;
  int committed_left;
  int src_id;
  int vid; // commit info (this is same as wr index) (dest_id)
  int new_dist;
  bool spec_flag;
  spec_tuple() {
    finished_left=0;
  }
};*/

struct meta_info {
  int finished_left;
  int committed_left;
  meta_info(int x, int y) {
    finished_left=x; committed_left=y;
  }
};

struct commit_info {
  int vid;
  // int timestamp;
  int parent_tid;
  int own_tid;
  commit_info(int x, int y, int z, int a) {
    vid=x; // timestamp=y; 
    parent_tid=z; own_tid=a;
  }
};

struct forward_info {
  int core_id;
  int task_id;
  int dst_id;
  int dst_timestamp; // just to reduce time now
  forward_info(int x, int y, int z, int a) {
    core_id=x; dst_timestamp=z; 
    dst_id=y; task_id=a;
  }
};

struct pending_mem_req {
  bool isWrite;
  int line_addr;
  pref_tuple cur_tuple;
  pending_mem_req(bool isWrite1, int line_addr1, pref_tuple cur_tuple1) {
    isWrite = isWrite1;
    line_addr = line_addr1;
    cur_tuple = cur_tuple1;
  }
};

typedef pair<int, commit_info> commit_pair;

#endif

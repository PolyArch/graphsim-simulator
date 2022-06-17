#include "asic_core.hh"
#include "network.hh"
#include "stats.hh"
#include "common.hh"
#include "asic.hh"

stats::stats(asic* host) : _asic(host) {
#if SGU_SLICING==1 || SGU==1 || GRAPHMAT_SLICING==1 || GRAPHMAT==1 || BLOCKED_ASYNC==1 // remapping is used in both cases
  _stat_extra_finished_edges = (int*)malloc(E*sizeof(int));
#endif

#if TREE_TRAV==1
  _start_cycle_per_query = (int*)malloc(NUM_KNN_QUERIES*sizeof(int));
#endif
    
    for(int i=0; i<core_cnt; ++i) {
      _stat_correct_edges[i]=0;
      _stat_incorrect_edges[i]=0;
      _stat_stall_thr[i]=0;
      _stat_finished_edges_per_core[i]=0;
    }
    _stat_num_clusters=0;

#if REUSE_STATS==1
    _stat_owned_vertex_freq_per_itn = (int*)malloc(_asic->_graph_vertices*sizeof(int));
    _stat_boundary_vertex_freq_per_itn = (int*)malloc(_asic->_graph_vertices*sizeof(int));
    _stat_edge_freq_per_itn = (int*)malloc(E*sizeof(int));
    _stat_source_copy_vertex_count = (int*)malloc(_asic->_slice_count*sizeof(int));
    _is_source_done = (bitset<V>*)malloc(_asic->_slice_count*sizeof(bitset<V>));
    reset_freq_stats();
#endif
    reset_cache_hit_aware_stats();
 
}

void stats::reset_freq_stats() {
  for(int i=0; i<_asic->_graph_vertices; ++i) {
    _stat_owned_vertex_freq_per_itn[i]=0;
    _stat_boundary_vertex_freq_per_itn[i]=0;
  }
  for(int i=0; i<_asic->_graph_edges; ++i) {
    _stat_edge_freq_per_itn[i]=0;
  }
   _stat_source_copy_vertex_count[_asic->_current_slice]=0;
   _is_source_done[_asic->_current_slice].reset();
}

int stats::get_bucket_num(int i) {
  if(i==0) return 0;
  if(i==1) return 1;
  int x = log2(i); // if 2,3 it will output 1+2=3, if 4 it will output 
  if(x+2>9) x=7;
  return x+2;
}

/*Expectations for different algos: (Should have slicing)
 *Sync: owned vertices should be high, low for bdary and 0/1 for edges
 *Sync-iter: same as above, edges should have higher
 *Async: all same as above (freq should be slightly higher for all)
 *Bulk-async: same as sync
 *Also area should vary with the algorithm
 */

// TODO: maybe need to find average across slices...
void stats::print_freq_stats() {
  int freq[10];
  for(int i=0; i<10; ++i) freq[i]=0;
  cout << "Owned vertices\n";
  for(int i=0; i<_asic->_graph_vertices; ++i) {
    freq[get_bucket_num(_stat_owned_vertex_freq_per_itn[i])]++;
    // cout << _stat_owned_vertex_freq_per_itn[i] << " ";
  }
  for(int i=0; i<10; ++i) {
    int id = pow(2,i-1); // for 0, this will be 0.5
    cout << "ovindex: " << id << ": " << freq[i] << endl;
    // if(i==0) cout << "ovindex: 0: " << freq[i] << endl;
    // if(i==1) cout << "ovindex: 1: " << freq[i] << endl;
    // if(i>1) cout << "start:" << pow(2,i-1) << " end: " << pow(2,i) << " ovindex: " << freq[i] << endl;  
    freq[i]=0;
  }
  cout << endl;
  cout << "Boundary vertices\n";
  for(int i=0; i<_asic->_graph_vertices; ++i) {
    freq[get_bucket_num(_stat_boundary_vertex_freq_per_itn[i])]++;
    // cout << _stat_boundary_vertex_freq_per_itn[i] << " ";
  }
  for(int i=0; i<10; ++i) {
    int id = pow(2,i-1); // for 0, this will be 0.5
    cout << "bvindex: " << id << ": " << freq[i] << endl;
    // if(i==0) cout << "bvindex: 0: " << freq[i] << endl;
    // if(i==1) cout << "bvindex: 1: " << freq[i] << endl;
    // if(i>1) cout << "start:" << pow(2,i-1) << " end: " << pow(2,i) << " bvindex: " << freq[i] << endl;  
    freq[i]=0;
  }
  cout << endl;
  cout << "Edges\n";
  for(int i=0; i<_asic->_graph_edges; ++i) {
    freq[get_bucket_num(_stat_edge_freq_per_itn[i])]++;
    // cout << _stat_edge_freq_per_itn[i] << " ";
  }
  for(int i=0; i<10; ++i) {
    int id = pow(2,i-1); // for 0, this will be 0.5
    cout << "eindex: " << id << ": " << freq[i] << endl;
    // if(i==0) cout << "eindex: 0: " << freq[i] << endl;
    // if(i==1) cout << "eindex: 1: " << freq[i] << endl;
    // if(i>1) cout << "start:" << pow(2,i-1) << " end: " << pow(2,i) << " efreq: " << freq[i] << endl;  
    freq[i]=0;
  }
  cout << endl;
  cout << "Source boundary vertices\n";
  // copy vertex count for the current slice (from prev iterations)
  // FIXME: It should be average over all slices (no concept of per_slice)
  for(int i=0; i<_asic->_graph_vertices; ++i) {
      if(_is_source_done[_asic->_current_slice].test(i)) {
          int bucket = get_bucket_num(_asic->_offset[i+1]-_asic->_offset[i]);
          freq[bucket]++;
      }
  }
  for(int i=0; i<10; ++i) {
    int id = pow(2,i-1); // for 0, this will be 0.5
    cout << "sbindex: " << id << ": " << freq[i] << endl;
    // if(i==0) cout << "sbindex: 0: " << freq[i] << endl;
    // if(i==1) cout << "sbindex: 1: " << freq[i] << endl;
    // if(i>1) cout << "start:" << pow(2,i-1) << " end: " << pow(2,i) << " sbfreq: " << freq[i] << endl;  
    freq[i]=0;
  }
  cout << endl;
}

void stats::reset_cache_hit_aware_stats() {
  for(int c=0; c<core_cnt; ++c) {
    _stat_tot_prefetch_updates_occupancy[c]=0;
    _stat_tot_priority_hit_updates_occupancy[c]=0;
    _stat_tot_hit_updates_occupancy[c]=0;
    _stat_tot_miss_updates_occupancy[c]=0;
    _stat_tot_hit_updates_served[c]=0;
    _stat_tot_miss_updates_served[c]=0;
    _stat_tot_priority_hit_updates_served[c]=0;
  }
}

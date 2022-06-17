## Build
1. Make sure you have the source code, updated DRAMSim2 and example datasets at this path
2. Do make libdramsim.so to build; simulator builds for every new run -- no runtime parameters
3. Update LD_LIBRARY_PATH from Makefile.inc to the correct dramsim path...
4. Let's do an example test: run BFS with everything ideal for an input dataset, below is the command:

make -f Makefile.inc sim-graphmat-slice FIFO=1 NUM_TQ_PER_CORE=1 TASK_QUEUE_SIZE=512 REUSE=1 SYNC_SIM=1 WORKING_CACHE=0 NETWORK=1 num_banks=256 MODULO=1 ABFS=1 input_file=\\\"/data/vidushi/polygraph_data/datasets/directed_power_law/fb_artist.csv\\\" csr_file=\\\"/data/vidushi/polygraph_data/datasets/undirected/soc-orkut.mtx\\\"  csc_file=\\\"/data/vidushi/polygraph_data/datasets/undirected/orkut_csc\\\" ans_file=\\\"/data/vidushi/polygraph_data/datasets/directed_power_law/fb_ans\\\" V=2997166 E=106349209 core_cnt=16 num_rows=4 DRAMSIM_ENABLED=1 WORK_STEALING=1 metis_file=\\\"\\\" L2SIZE=16384 process_thr=16 BANKED=1 REUSE_STATS=0 bus_width=64 SLICE_HRST=1 SLICE_COUNT=1 REAL_MULTICAST=1 OUTFILE=sim-pr-allowed-v_2997166_time_2_algo_0_spatial_0_wkld_1 SRC_LOC=0 DECOMPOSABLE=1

You should see output like this:
Cycles: 13993004
Flush cycles: 0
Boundary cycles: 421470
Total cycles: 14414480
L2 hit rate: 0.620209
L2 accesses/edges: 106335206 and size: 16384 kB
tasks created online: 0
Time: 1.3993e+07 ns
GTEPS: 7.59917
Real GTEPS: 7.60017

## Ideal experiments
1. BFS: make -f Makefile.ideal sim-graphmat-slice csr_file=\\\"flickr_csr\\\"  V=820878 E=9837214
make -f Makefile.ideal sim-sgu-slice csr_file=\\\"flickr_csr\\\"  V=820878 E=9837214 ABFS=1
2. TC: need to do only one execution model here: make -f Makefile.ideal sim-sgu csr_file=\\\"cora_csr\\\"  V=2708 E=10556 CF=1 TC=1 ALL_EDGE_ACCESS=1 MAX_DFGS=256

(PS: Try sensitivity to more resources by assigning both process_thr and
num_banks to the same value (default is 256). For TC, this will be process_thr
and MAX_DFGS)

## Realish experiments

1. Define latency to access local SRAM vertices (update MAX_NET_LATENCY in
   common.hh if you want to try for higher latencies)
For BFS, make -f Makefile.ideal sim-sgu csr_file=\\\"flickr_csr\\\"  V=820878 E=9837214 ABFS=1 CROSSBAR_LAT=32 MODEL_NET_DELAY=1 FIFO_DEP_LEN=8 AGG_FIFO_DEP_LEN=8

For TC, make -f Makefile.ideal sim-sgu csr_file=\\\"cora_csr\\\"  V=2708 E=10556 CF=1 TC=1 ALL_EDGE_ACCESS=1 DFG_LENGTH=8 FIFO_DEP_LEN=8 AGG_FIFO_DEP_LEN=8

2. Use practical network with NUMA memory congestion and update packet
   congestion (change bus_width for sensitivity but not required for now)
For BFS,  make -f Makefile.ideal sim-sgu-slice csr_file=\\\"flickr_csr\\\"  V=820878 E=9837214 ABFS=1 PERFECT_NET=0 REAL_MULTICAST=1 DECOMPOSABLE=1 NUMA_CONTENTION=1 bus_width=64
For TC,  make -f Makefile.ideal sim-sgu csr_file=\\\"cora_csr\\\"  V=2708 E=10556 CF=1 TC=1 ALL_EDGE_ACCESS=1 MAX_DFGS=16 PERFECT_NET=0 REAL_MULTICAST=1 DECOMPOSABLE=1 SGU_GCN_REORDER=1 NUMA_CONTENTION=1 bus_width=64

# Simulator Usage

## Statistics

* Cycles
* GTEPS = based on number of edges 'required' to be traversed
* Total requests per bank = to create the network distribution graph
* Local updates = efficiency of mapping algorithm (change with get_local_scratch_id() and get_local_bank_id())
* Compulsory miss rate = used for cache architectures like Espresso, Tesseract
* Load per core = tasks executed in terms of vertices (good?)
 
## Nobs

* core_cnt = number of cores in the final system
* num_rows = number of rows in the final mesh (only square mesh supported)
* mem_bw = memory bandwidth
* process_thr = number of edges processed per core per cycle
* scr_bw = number of requests pushed in banks
* num_banks = for crossbar, total and for mesh, number of banks per core
* message_size = size of a vertex distance unit (currently only simulated for 4)
* bus_width = used for network utilization
* NETWORK = chose mesh over crossbar
* ASTAR = extra accesses to destination vertices as compared to SSSP
* DIJKSTRA = Only same timestamp vertices are active at a time
* GRAPHMAT = graphmat execution model (results without calculating active vertex calc cycles overhead)
* GRAPHMAT_CF = correctly model a crossbar for GCN
* GRAPHLAB = no barrier, reads old distance directly from scratchpad and update results
* ESPRESSO = speculative execution model, mesh of in-order cores, only compulsory misses, perfect conflict detection
* SGU = speculative execution model, mesh of CGRAs of given throughput, perfect conflict detection
* TESSERACT = graphmat, mesh of lane width (32) in-order cores at a node, compulsory misses?
* DECOMPOSABLE = send packets through the bus until whole bandwidth is utilized
* WORST_NET = combine packets only from same incoming buffer

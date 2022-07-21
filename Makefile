ifndef REAL_MULTICAST
REAL_MULTICAST=1
endif

ifndef bus_width
bus_width=64 # 16
endif

ifndef ABFS
ABFS=1
endif

ifndef WORKING_CACHE
WORKING_CACHE=0
endif

ifndef FIFO
FIFO=1
endif

ifndef BLOCKED_DFS
BLOCKED_DFS=0
endif

ifndef SRC_LOC
SRC_LOC=0
endif

ifndef SPATIAL_STRIDE_FACTOR
SPATIAL_STRIDE_FACTOR=8
endif

ifndef PROFILING
PROFILING=0
endif

ifndef PERFECT_NET
PERFECT_NET=0
endif

ifndef DECOMPOSABLE
DECOMPOSABLE=1
endif

ifndef process_thr
process_thr=16
endif

ifndef core_cnt
core_cnt=16
endif

ifndef num_rows
num_rows=4
endif

# should i keep it equal to core_cnt?
ifndef num_banks
num_banks=16
endif

ifndef MODULO
MODULO=0
endif



ifndef NETWORK
NETWORK=1
endif



ifndef ALL_EDGE_ACCESS
ALL_EDGE_ACCESS=0
endif

ifndef PRAC
PRAC=1
endif

ifndef PERFECT_LB
PERFECT_LB=0
endif

ifndef TASK_QUEUE_SIZE
TASK_QUEUE_SIZE=256
endif

ifndef COMPLETION_BUFFER_SIZE
COMPLETION_BUFFER_SIZE=256 # 1024
endif

ifndef NUMA_CONTENTION
NUMA_CONTENTION=0
endif

ifndef HRC_XBAR
HRC_XBAR=0
endif

ifndef CROSSBAR_LAT
CROSSBAR_LAT=0
endif

ifndef CROSSBAR_BW
CROSSBAR_BW=1
endif

ifndef MODEL_NET_DELAY
MODEL_NET_DELAY=0
endif

ifndef BANKED
BANKED=0
endif

ifndef input_file
input_file=\"example.csv\"
endif

ifndef metis_file
metis_file=\"\"
endif

ifndef csr_file
csr_file=\"example.csv\"
endif

ifndef csc_file
csc_file=\"example.csv\"
endif

ifndef ans_file
ans_file=\"example.csv\"
endif

ifndef WORK_STEALING
WORK_STEALING=1
endif

# ------------relevant parameters for ideal experiments

ifndef EPSILON
EPSILON=10
endif

ifndef INTERTASK_REORDER
INTERTASK_REORDER=0
endif

ifndef BATCH_WIDTH
BATCH_WIDTH=4
endif

ifndef BATCHED_CORES
BATCHED_CORES=1
endif

ifndef HYBRID_HATS_CACHE_HIT
HYBRID_HATS_CACHE_HIT=0
endif

ifndef CENTRAL_BATCH
CENTRAL_BATCH=1
endif

ifndef PREF_TQ_LAT
PREF_TQ_LAT=0
endif

ifndef MAX_DFGS
MAX_DFGS=1
endif

ifndef MULTICAST_BATCH
MULTICAST_BATCH=2
endif

ifndef LSQ_WAIT
LSQ_WAIT=0
endif

ifndef knnData
knnData=1000
endif

ifndef NUM_KNN_QUERIES
NUM_KNN_QUERIES=100
endif

ifndef NUM_DATA_PER_LEAF
NUM_DATA_PER_LEAF=1024
endif

ifndef LOCAL_HATS
LOCAL_HATS=0
endif

ifndef HATS
HATS=0
endif

ifndef AGG_HATS
AGG_HATS=0
endif

ifndef HASH_JOIN
HASH_JOIN=0
endif

ifndef FIFO_DEP_LEN
FIFO_DEP_LEN=1024
endif

ifndef AGG_FIFO_DEP_LEN
AGG_FIFO_DEP_LEN=102400
endif

ifndef TREE_TRAV
TREE_TRAV=0
endif

ifndef CACHE_HIT_AWARE_SCHED
CACHE_HIT_AWARE_SCHED=0
endif

ifndef HETRO
HETRO=1
endif

ifndef AGG_MULT_TYPE
AGG_MULT_TYPE=0
endif

ifndef MULT_AGG_TYPE
MULT_AGG_TYPE=0
endif

ifndef DYN_LOAD_BAL
DYN_LOAD_BAL=0
endif

ifndef EDGE_LOAD_BAL
EDGE_LOAD_BAL=0
endif

ifndef PRIO_XBAR
PRIO_XBAR=0
endif

ifndef XBAR_ABORT
XBAR_ABORT=0
endif

ifndef GRAPHPULSE
GRAPHPULSE=0
endif

ifndef HILBERT_MAP
HILBERT_MAP=0
endif

ifndef MTX
MTX=0
endif

ifndef PULL
PULL=0
endif

ifndef SYNC_NO_SWITCH
SYNC_NO_SWITCH=0
endif

ifndef ASTAR
ASTAR=0
endif

ifndef UPDATE_BATCH
UPDATE_BATCH=1
endif

ifndef DYN_GRAPH
DYN_GRAPH=0
endif

ifndef DETAILED_STATS
DETAILED_STATS=1
endif

ifndef SEQ_EDGE
SEQ_EDGE=1
endif

ifndef INC_DYN
INC_DYN=0
endif

ifndef RECOMP_DYN
RECOMP_DYN=0
endif

ifndef SLICE_HRST
SLICE_HRST=0
endif

ifndef HEURISTIC
HEURISTIC=0
endif

ifndef DYN_THRES
DYN_THRES=0
endif

ifndef GCN_MATRIX
GCN_MATRIX=0
endif

ifndef PRED_THR
PRED_THR=0
endif

ifndef ABCD
ABCD=0
endif

ifndef PRESORTER
PRESORTER=0
endif

ifndef CACHE_NO_SWITCH
CACHE_NO_SWITCH=0
endif

ifndef ASYNC_NO_SWITCH
ASYNC_NO_SWITCH=0
endif

ifndef SYNC_SCR
SYNC_SCR=0
endif

ifndef HYBRID_EVAL
HYBRID_EVAL=0
endif

ifndef HYBRID
HYBRID=0
endif

ifndef REUSE_STATS
REUSE_STATS=0
endif

ifndef GRAPH_DIA
GRAPH_DIA=1
endif


ifndef DYN_REUSE
DYN_REUSE=0
endif

ifndef ALL_CACHE
ALL_CACHE=0
endif

ifndef EXTREME
EXTREME=0
endif

ifndef DFG_LENGTH
DFG_LENGTH=1
endif


ifndef SYNC_SIM
SYNC_SIM=0
endif

ifndef RESIZING
RESIZING=0
endif

ifndef OUTFILE
OUTFILE=name
endif

# ifndef V
# V=11
# endif
# 
# ifndef E
# E = 20
# endif

# in bytes per cycle
ifndef mem_bw
mem_bw=256
endif

# in bytes per cycle
ifndef scr_bw
scr_bw=256
endif

ifndef UNDIRECTED
UNDIRECTED=0
endif

ifndef UNWEIGHTED
UNWEIGHTED=0
endif

ifndef REUSE
REUSE=1
endif


ifndef WORST_NET
WORST_NET=0
endif

ifndef IDEAL_NET
IDEAL_NET=0
endif

ifndef GCN
GCN=0
endif

ifndef GCN_LAYERS
GCN_LAYERS=1
endif

ifndef SGU_GCN_REORDER
SGU_GCN_REORDER=0
endif

ifndef GCN_SWARM
GCN_SWARM=0
endif

ifndef GRAPHMAT_CF
GRAPHMAT_CF=0
endif

ifndef CF_SWARM
CF_SWARM=0
endif

ifndef ACC
ACC=0
endif

ifndef LAZY_CYCLES
LAZY_CYCLES=1000000000
endif

ifndef DEP_CHECK_DEPTH
DEP_CHECK_DEPTH=4
endif

ifndef REORDER
REORDER=0
endif

ifndef DISTANCE_SCHED
DISTANCE_SCHED=1
endif

ifndef DFSMAP
DFSMAP=0
endif

ifndef PR
PR=0
endif

ifndef SPECIAL_CACHE
SPECIAL_CACHE=0
endif

ifndef EDGE_CACHE
EDGE_CACHE=0
endif

ifndef DRAMSIM_ENABLED
DRAMSIM_ENABLED=1
endif

ifndef LRU
LRU=0
endif

ifndef LADIES_GCN
LADIES_GCN=0
endif

ifndef CENTRAL_FIFO
CENTRAL_FIFO=0
endif

ifndef BFS_MAP
BFS_MAP=0
endif

ifndef ENTRY_ABORT
ENTRY_ABORT=1
endif

ifndef UPDATE_COALESCE
UPDATE_COALESCE=0
endif

ifndef CF
CF=0
endif

ifndef ABORT
ABORT=0
endif

ifndef CSR
CSR=0
endif

ifndef CHRONOS
CHRONOS=0
endif

ifndef ARBIT
ARBIT=0
endif

ifndef PATH_MULTICAST
PATH_MULTICAST=0
endif

ifndef PHI
PHI=0
endif

ifndef SLICE_ITER
SLICE_ITER=1
endif

ifndef SLICE_COUNT
SLICE_COUNT=1
endif

ifndef METIS_SLICE_COUNT
METIS_SLICE_COUNT=1
endif

ifndef HIGH_PRIO_RESERVE
HIGH_PRIO_RESERVE=2
endif

ifndef L2SIZE
L2SIZE=4096
endif

ifndef ALLHITS
ALLHITS=0
endif

ifndef ALLMISS
ALLMISS=0
endif

ifndef NUM_TQ_PER_CORE
NUM_TQ_PER_CORE=1
endif

ifndef ANAL_MODE
ANAL_MODE=0
endif

ifndef FEAT_LEN
FEAT_LEN=16
endif

ifndef LINEAR
LINEAR=0
endif

ifndef BDFS
BDFS=0
endif


ifndef BBFS
BBFS=0
endif

ifndef METIS
METIS=0
endif

ifndef BD
BD=1000
endif

ifndef RANDOM
RANDOM=0
endif

ifndef RANDOM_SPATIAL
RANDOM_SPATIAL=0
endif

ifndef NOLOAD
NOLOAD=0
endif

ifndef TC
TC=0
endif

ifndef DEFAULT_DATASET
DEFAULT_DATASET=0
endif

sim-files=asic.cpp asic_core.cpp network.cpp stats.cpp config.cpp simple_cache.cpp replacement_state.cpp multiply.cpp
CC=g++ #gcc
FLAGS=-std=c++11 -ggdb -g

MACROS=-Dcsr_file=$(csr_file) -Dans_file=$(ans_file) -DV=$(V) -DE=$(E) -Dcore_cnt=$(core_cnt) -Dmem_bw=$(mem_bw) -Dscr_bw=$(scr_bw) -Dnum_banks=$(num_banks) -DWORK_STEALING=$(WORK_STEALING) -Dinput_file=$(input_file) -Dprocess_thr=$(process_thr) -DNETWORK=$(NETWORK) -Dnum_rows=$(num_rows) -DUNDIRECTED=$(UNDIRECTED) -DDECOMPOSABLE=$(DECOMPOSABLE) -DWORST_NET=$(WORST_NET) -DIDEAL_NET=$(IDEAL_NET) -DLAZY_CYCLES=$(LAZY_CYCLES) -DDEP_CHECK_DEPTH=$(DEP_CHECK_DEPTH) -DREORDER=$(REORDER) -DDISTANCE_SCHED=$(DISTANCE_SCHED) -DDFSMAP=$(DFSMAP) -DPERFECT_NET=$(PERFECT_NET) -DABFS=$(ABFS) -DPR=$(PR) -DSPECIAL_CACHE=$(SPECIAL_CACHE) -DLRU=$(LRU) -DPRAC=$(PRAC) -DDRAMSIM_ENABLED=$(DRAMSIM_ENABLED) -DWORKING_CACHE=$(WORKING_CACHE) -DFIFO=$(FIFO) -DABORT=$(ABORT) -DBFS_MAP=$(BFS_MAP) -DENTRY_ABORT=$(ENTRY_ABORT) -DCF=$(CF) -DGCN=$(GCN) -DGCN_SWARM=$(GCN_SWARM) -DACC=$(ACC) -DCF_SWARM=$(CF_SWARM) -DGRAPHMAT_CF=$(GRAPHMAT_CF) -DSGU_GCN_REORDER=$(SGU_GCN_REORDER) -DPERFECT_LB=$(PERFECT_LB) -DARBIT=$(ARBIT) -Dmetis_file=$(metis_file) -DREAL_MULTICAST=$(REAL_MULTICAST) -DPATH_MULTICAST=$(PATH_MULTICAST) -DPHI=$(PHI) -DSLICE_ITER=$(SLICE_ITER) -DSLICE_COUNT=$(SLICE_COUNT) -DHIGH_PRIO_RESERVE=$(HIGH_PRIO_RESERVE) -DL2SIZE=$(L2SIZE) -DALLHITS=$(ALLHITS) -DALLMISS=$(ALLMISS) -DTASK_QUEUE_SIZE=$(TASK_QUEUE_SIZE) -Dbus_width=$(bus_width) -DNUM_TQ_PER_CORE=$(NUM_TQ_PER_CORE) -DANAL_MODE=$(ANAL_MODE) -DFEAT_LEN=$(FEAT_LEN) -DMODULO=$(MODULO) -DLINEAR=$(LINEAR) -DBDFS=$(BDFS) -DMETIS=$(METIS) -DBD=$(BD) -DRANDOM=$(RANDOM) -DRANDOM_SPATIAL=$(RANDOM_SPATIAL) -DBBFS=$(BBFS) -DNOLOAD=$(NOLOAD) -DCHRONOS=$(CHRONOS) -DPROFILING=$(PROFILING) -DLADIES_GCN=$(LADIES_GCN) -DCSR=$(CSR) -DUNWEIGHTED=$(UNWEIGHTED) -DREUSE=$(REUSE) -DEDGE_CACHE=$(EDGE_CACHE) -DSYNC_SIM=$(SYNC_SIM) -DRESIZING=$(RESIZING) -DOUTFILE=$(OUTFILE) -DEXTREME=$(EXTREME) -DGCN_LAYERS=$(GCN_LAYERS) -DDYN_REUSE=$(DYN_REUSE) -DALL_CACHE=$(ALL_CACHE) -DDFG_LENGTH=$(DFG_LENGTH) -DGRAPH_DIA=$(GRAPH_DIA) -DREUSE_STATS=$(REUSE_STATS) -DSYNC_SCR=$(SYNC_SCR) -DBANKED=$(BANKED) -DHYBRID=$(HYBRID) -DMODEL_NET_DELAY=$(MODEL_NET_DELAY) -DCROSSBAR_BW=$(CROSSBAR_BW) -DHYBRID_EVAL=$(HYBRID_EVAL) -DCROSSBAR_LAT=$(CROSSBAR_LAT) -DCACHE_NO_SWITCH=$(CACHE_NO_SWITCH) -DHRC_XBAR=$(HRC_XBAR) -DASYNC_NO_SWITCH=$(ASYNC_NO_SWITCH) -DABCD=$(ABCD) -DSRC_LOC=$(SRC_LOC) -DPRESORTER=$(PRESORTER) -DBLOCKED_DFS=$(BLOCKED_DFS) -DPRED_THR=$(PRED_THR) -DHEURISTIC=$(HEURISTIC) -DGCN_MATRIX=$(GCN_MATRIX) -DSLICE_HRST=$(SLICE_HRST) -DDYN_GRAPH=$(DYN_GRAPH) -DINC_DYN=$(INC_DYN) -DRECOMP_DYN=$(RECOMP_DYN) -DDETAILED_STATS=$(DETAILED_STATS) -DSEQ_EDGE=$(SEQ_EDGE) -DUPDATE_BATCH=$(UPDATE_BATCH) -DASTAR=$(ASTAR) -DPULL=$(PULL) -Dcsc_file=$(csc_file) -DSYNC_NO_SWITCH=$(SYNC_NO_SWITCH) -DMTX=$(MTX) -DDYN_LOAD_BAL=$(DYN_LOAD_BAL) -DHILBERT_MAP=$(HILBERT_MAP) -DSPATIAL_STRIDE_FACTOR=$(SPATIAL_STRIDE_FACTOR) -DCOMPLETION_BUFFER_SIZE=$(COMPLETION_BUFFER_SIZE) -DGRAPHPULSE=$(GRAPHPULSE) -DXBAR_ABORT=$(XBAR_ABORT) -DPRIO_BANK=$(PRIO_BANK) -DPRIO_XBAR=$(PRIO_XBAR) -DDYN_THRES=$(DYN_THRES) -DCENTRAL_FIFO=$(CENTRAL_FIFO) -DHETRO=$(HETRO) -DAGG_MULT_TYPE=$(AGG_MULT_TYPE) -DMULT_AGG_TYPE=$(MULT_AGG_TYPE) -DALL_EDGE_ACCESS=$(ALL_EDGE_ACCESS) -DCACHE_HIT_AWARE_SCHED=$(CACHE_HIT_AWARE_SCHED) -DMETIS_SLICE_COUNT=$(METIS_SLICE_COUNT) -DTREE_TRAV=$(TREE_TRAV) -DUPDATE_COALESCE=$(UPDATE_COALESCE) -DFIFO_DEP_LEN=$(FIFO_DEP_LEN) -DHASH_JOIN=$(HASH_JOIN) -DEDGE_LOAD_BAL=$(EDGE_LOAD_BAL) -DHATS=$(HATS) -DNUM_KNN_QUERIES=$(NUM_KNN_QUERIES) -DLSQ_WAIT=$(LSQ_WAIT) -DNUM_DATA_PER_LEAF=$(NUM_DATA_PER_LEAF) -DknnData=$(knnData) -DPREF_TQ_LAT=$(PREF_TQ_LAT) -DAGG_FIFO_DEP_LEN=$(AGG_FIFO_DEP_LEN) -DMULTICAST_BATCH=$(MULTICAST_BATCH) -DMAX_DFGS=$(MAX_DFGS) -DCENTRAL_BATCH=$(CENTRAL_BATCH) -DHYBRID_HATS_CACHE_HIT=$(HYBRID_HATS_CACHE_HIT) -DBATCHED_CORES=$(BATCHED_CORES) -DBATCH_WIDTH=$(BATCH_WIDTH) -DINTERTASK_REORDER=$(INTERTASK_REORDER) -DAGG_HATS=$(AGG_HATS) -DLOCAL_HATS=$(LOCAL_HATS) -DEPSILON=$(EPSILON) -DTC=$(TC) -DDEFAULT_DATASET=$(DEFAULT_DATASET) -DNUMA_CONTENTION=$(NUMA_CONTENTION)

sim-baseline: dijkstra.cpp
	$(CC) dijkstra.cpp $(FLAGS) $(MACROS) -o $(@) # -DTOPO_SORT
	./$(@)

sim-preprocess: preprocess.cpp
	$(CC) preprocess.cpp $(FLAGS) $(MACROS) -o $(OUTFILE)
	./$(OUTFILE)

sim-graphmat: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS)  -L/home/vidushi/graphsim/DRAMSim2/ -ldramsim -o $(OUTFILE) -DGRAPHMAT
	./$(OUTFILE)

sim-graphmat-slice: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS)  -L/home/vidushi/graphsim/DRAMSim2/ -ldramsim -o $(OUTFILE) -DGRAPHMAT -DGRAPHMAT_SLICING
	./$(OUTFILE)

sim-polygraph: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS) -L/home/vidushi/graphsim/DRAMSim2/ -ldramsim -o $(OUTFILE) -DSGU -DSGU_SLICING
	./$(OUTFILE)

# iterations in a round-robin manner (scheduling is done offline only), only
# slicing (must be a different cycle function) -- no abort
sim-blocked-async: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS) -L/home/vidushi/graphsim/DRAMSim2/ -ldramsim -o $(OUTFILE) -DBLOCKED_ASYNC
	./$(OUTFILE)


sim-tesseract: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS) -L/home/vidushi/graphsim/DRAMSim2/ -ldramsim -o $(OUTFILE) -DGRAPHMAT -DTESSERACT
	./$(OUTFILE)

sim-sgu: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS) -L/home/vidushi/graphsim/DRAMSim2/ -ldramsim -o $(OUTFILE) -DSGU
	./$(OUTFILE)

# currently works only for PRAC=0
sim-sgu-hybrid: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS) -L/home/vidushi/graphsim/DRAMSim2/ -ldramsim -o $(OUTFILE) -DSGU -DSGU_HYBRID
	./$(OUTFILE)

# need to include libdramsim.so

sim-espresso: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS) -L/home/vidushi/graphsim/DRAMSim2/ -ldramsim -o $(@) -DESPRESSO
	./$(@)

sim-swarm: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS) -L/home/vidushi/graphsim/DRAMSim2/ -ldramsim -o $(OUTFILE) -DSWARM
	./$(OUTFILE)

sim-graphlab: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS) -o $(@) -DGRAPHLAB
	./$(@)

sim-dijkstra: $(sim-files) *.hh $(@)
	$(CC) $(sim-files) $(FLAGS) $(MACROS) -L/home/vidushi/graphsim/DRAMSim2/ -ldramsim -o $(@) -DDIJKSTRA
	./$(@)

clean:
	rm *.log
	rm sim-*

import subprocess
import math
from multiprocessing import Process
from multiprocessing import Pool
import os.path
from os import path


cases = []

cases.append(("datasets/directed_power_law/fb_artist.csv","datasets/undirected/soc-orkut.mtx","datasets/undirected/orkut_csc","datasets/directed_power_law/fb_ans", 2997166, 106349209, "",4096,1))

cases.append(("datasets/directed_uniform/USA-road-d.E.gr","datasets/directed_uniform/Erd_csr","datasets/directed_uniform/Erd_csr","datasets/directed_uniform/Erd_ans", 3598623, 8778114, "", 4096, 1))

cases.append(("/home/vidushi/ss-stack/ss-workloads/graph-benchmarks/pagerank/datasets/soc-LiveJournal1.txt","datasets/directed_power_law/lj_csr","datasets/directed_power_law/lj_csc","/home/vidushi/ss-stack/ss-workloads/graph-benchmarks/pagerank/datasets/lj_ans", 4847571, 68993773,"",4096,2))

cases.append(("datasets/directed_uniform/USA-road-d.E.gr","datasets/metis_inputs/indochina-2004.mtx","datasets/metis_inputs/indo_csc","datasets/directed_uniform/Erd_ans", 7414866, 194109311, "",4096,2))

cnt = (16)
rows = (4)

workload = ['ABFS=0','ABFS=1','ACC=1', 'PR=1']
# sync, sync-reuse, blocked-async, async
algorithm = []
algorithm.append(('sim-graphmat-slice FIFO=1 NUM_TQ_PER_CORE=1 TASK_QUEUE_SIZE=512 REUSE=1 SYNC_SIM=1')) # sync-round-robin
algorithm.append(('sim-graphmat-slice FIFO=1 NUM_TQ_PER_CORE=1 TASK_QUEUE_SIZE=512 RESIZING=1 SYNC_SIM=1 REUSE=4')) # sync-slice-iter
algorithm.append(('sim-sgu-slice ABORT=1 ENTRY_ABORT=1 NUM_TQ_PER_CORE=2 TASK_QUEUE_SIZE=256 HIGH_PRIO_RESERVE=32 PRAC=1')) # async
algorithm.append(('sim-blocked-async FIFO=1 NUM_TQ_PER_CORE=1 TASK_QUEUE_SIZE=512')) # blocked-async
algorithm.append(('sim-sgu-slice ABORT=0 ENTRY_ABORT=0 FIFO=1 NUM_TQ_PER_CORE=1 TASK_QUEUE_SIZE=512 HIGH_PRIO_RESERVE=0 PRAC=0')) # async-fifo

algorithm.append(('sim-sgu-slice ABORT=0 ENTRY_ABORT=0 FIFO=1 NUM_TQ_PER_CORE=1 TASK_QUEUE_SIZE=512 HIGH_PRIO_RESERVE=0 PRAC=0 ABCD=1')) # bulk-async-abcd
algorithm.append(('sim-sgu-slice ABORT=1 ENTRY_ABORT=1 FIFO=0 NUM_TQ_PER_CORE=2 TASK_QUEUE_SIZE=1024 HIGH_PRIO_RESERVE=32 PRAC=1 ABCD=1')) # async-abcd


# algo_6: async+ABCD
algorithm.append(('sim-sgu-slice ABORT=1 ENTRY_ABORT=1 FIFO=0 NUM_TQ_PER_CORE=2 TASK_QUEUE_SIZE=256 HIGH_PRIO_RESERVE=32 PRAC=1 ABCD=1')) # async
# algo_7: bulk-async+ABCD
algorithm.append(('sim-blocked-async FIFO=1 NUM_TQ_PER_CORE=1 TASK_QUEUE_SIZE=512 ABCD=1'))


algorithm.append(('sim-sgu-slice ABORT=0 ENTRY_ABORT=0 NUM_TQ_PER_CORE=1 TASK_QUEUE_SIZE=512 HIGH_PRIO_RESERVE=0 PRAC=1')) # chronos
# algo_9 + spatial=2
# algorithm.append(('sim-sgu-slice ABORT=0 ENTRY_ABORT=0 NUM_TQ_PER_CORE=2 TASK_QUEUE_SIZE=256 HIGH_PRIO_RESERVE=0 PRAC=1')) # +2 TQ
algorithm.append(('sim-sgu-slice ABORT=1 ENTRY_ABORT=1 NUM_TQ_PER_CORE=2 TASK_QUEUE_SIZE=256 HIGH_PRIO_RESERVE=32 PRAC=1')) # async

# only cache, cache with slicing, scratchpad, hybrid scratch-cache (where we can
# both pin and use it as edge cache)
temporal_slice = ['WORKING_CACHE=1 SLICE_COUNT=1 EDGE_CACHE=1','WORKING_CACHE=1 EDGE_CACHE=1','WORKING_CACHE=0','DYN_REUSE=1 EDGE_CACHE=1']
# crossbar, mesh
spatial_slice = ['NETWORK=0 num_banks=256 MODULO=1','NETWORK=1 num_banks=16 BDFS=1 DECOMPOSABLE=1','NETWORK=1 num_banks=16 BLOCKED_DFS=1 BDFS=1 NOOAD=1 DECOMPOSABLE=1']
# spatial_slice = ['NETWORK=0 num_banks=256 BDFS=1 DECOMPOSABLE=1']


def graph_dse(env, log_file):
    f = open(log_file,'w+')
    subprocess.check_call(env, shell=True, stdout=f)
    f.close()

algo_list = [0, 2, 3]
time_list = [0, 2]


if __name__ == '__main__':
    pool = Pool(processes=64)
    spatial=2
    for inp, csr, csc, ans, v, e, metis, l2, count in cases: # 4
        for time in time_list: # 2
            for wkld in range(len(workload)): # 4
                for algo in range(0,8): # 10 (will do by 10 later)
                    env_dse_comb = "make -f Makefile.inc " + algorithm[algo] + ' ' + temporal_slice[time] + ' ' + spatial_slice[spatial] + ' ' + workload[wkld] + ' PULL=1'
                    env_graph = "input_file=\\\\\\\"%s\\\\\\\" csr_file=\\\\\\\"%s\\\\\\\"  csc_file=\\\\\\\"%s\\\\\\\" ans_file=\\\\\\\"%s\\\\\\\" V=%d E=%d core_cnt=16 num_rows=4 DRAMSIM_ENABLED=1 WORK_STEALING=1 metis_file=\\\\\\\"%s\\\\\\\" L2SIZE=16384 process_thr=16" % (inp, csr, csc, ans, v, e, metis)
                    env = env_dse_comb + ' ' + env_graph + ' BANKED=1 REUSE_STATS=0'
                    if(time>0):
                        env = env + ' SLICE_COUNT=%d' %(count)
                    outfile = " OUTFILE=sim-bfs-v_%d_time_%d_algo_%d_spatial_%d_wkld_%d" % (v, time, algo, spatial, wkld)
                    env = env + outfile + ' SRC_LOC=0'
                    log_file = 'isca21_pull_data/per_iter_pull_v_%d_time_%d_algo_%d_spatial_%d_wkld_%d'% (v, time, algo, spatial, wkld)
                    # print(env)
                    result = pool.apply_async(graph_dse, args=(env,log_file,))
        pool.close()
        pool.join()

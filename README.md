## Source file information
1. Currently Makefile is set to asynchronousupdates/priority-vertex-scheduling/graph-slicing PolyGraph model.
2. To run, first build DRAMSim2: `make libdramsim.so` and do `export LD_LIBRARY_PATH=path-to-dramsim2-folder/:$LD_LIBRARY_PATH` (you may put this in bashrc)
3. Let's try a simple example using cora dataset in `sample_dataset` folder (format: src dst):
```
make sim-polygraph csr_file=\\\"/home/vidushi/graphsim-simulator/sample_datasets/cora_csr\\\"  V=2708 E=10556
```
For other variants, find details below:

4. PolyGraph with cache of 4 MB (4096 kB) for uniform-degree graphs:
```
make sim-polygraph csr_file=\\\"/home/vidushi/graphsim-simulator/sample_datasets/cora_csr\\\"  V=2708 E=10556 WORKING_CACHE=1 L2SIZE=4096
```

Note: The heuristic graph is not encoded in the simulator properly yet, the programmer specifies which variant to use, when to switch dynamically (later one is a little complex, please contact for more details).

You should see output like this:
```
Cycles: 861
Barrier cycles: 42
Total cycles: 903
L2 hit rate: 0.984996
L2 accesses/edges: 11330 and size: 4096 kB
tasks created online: 5414
Time: 861 ns
GTEPS: 0
Real GTEPS: 12.2602
```

# Simulator Usage

## Statistics
Here is the description of the most commonly used statistics.

* Cycles = cycles taken by the algorithm
* Barrier cycles = these are cycles taken for switching slices (see Figure 8).
* Total cycles = addition of the actual computation and barrier cycles.
* GTEPS = number of giga-edges traversed per second (assuming the frequency of 1 GHz)
* Real GTEPS = GTEPS normalized to work-efficiency of the algorithm
* Local/remote updates = number of atomic update tasks (Figure 7) that were local/remote to the access-task core
* Load per core = number of access-vertex tasks executed at a core
* Exact hit rate = number of hits to the cache divided by the total number of accesses to the cache
 
## Simulator knobs
Below I put the most common knobs used to try different algorithm variants:
* Update-visibility = for async: use sim-polygraph, for sync-slice: ise sim-polygraph with ABCD=1, for graph-sync: use sim-graphmat mode for
* FIFO = round-robin or priority vertex scheduling
* WORKING_CACHE = whether to use non-sliced or sliced
* SLICE_COUNT = number of slices for slice scheduling
* PULL = pull or push (push by default)

These are the knobs provided by the simulator to change the studied algorithm or evaluated architecture (all declared in config.cpp).

| Feature           | Allowed input dimensions                                                                                | Representative                                      |
|-------------------|---------------------------------------------------------------------------------------------------------|-----------------------------------------------------|
| Graph_shape       | Low dia, high dia                                                                                       | Road, amazon or Synthetic                           |
| GP-size           | Fit in memory, not                                                                                      | 2MB, 2GB, 4GB                                       |
| algo_order        | Order sens (sp, pr, cf, astar) or not (bfs, cc, gcn+ladies sample)                                      | Sp, bfs, pr                                         |
| Frontier          | Frontier (sssp, bfs, cc), Non-frontier (pr, , cf, gcn)                                                  | Sp, bfs, pr                                         |
| Feature           | Allowed architecture dimensions                                                                         | Idealized dimension                                 |
| net_topology      | mesh, crossbar, hrc_xbar                                                                                | xbar                                                |
| net_traffic_type  | Basic_net, decomposable, real_multicast, path_multicast, ideal_net                                      | Infinite bandwidth                                  |
| cache_repl_type   | lru, phi, allhits, allmiss                                                                              | All hits                                            |
| mem_type          | Dram_mem, ideal_mem                                                                                     | Inf memory bandwidth                                |
| spatial_part_type | random_spatial, linear, modulo, dfs_map, bfs_map, bdfs, bbfs, blocked_dfs, noload, remapping, automatic | Always local mapping                                |
| work_dist_type    | addr_map, work_steal                                                                                    | Single core with combined compute bw (all can fwd?) |
| temporal_sched    | Prio, non-prio                                                                                          | prio-inf-length                                     |
| Feature           | Allowed programming dimensions (assum flex arch)                                                        | Take max of these?                                  |
| task_sched_type   | datadep, fifo, abcd, vertex_id                                                                          | (indep of architecture??)                           |
| slice_sched_type  | roundrobin, locality, priority                                                                          |                                                     |
| update_visibility | synch, async-coarse, asyncfine, dyn_graph fine-sync (dijkstra?), speculative                            |                                                     |
| dyn_algo_type     | inc, recomp                                                                                             |                                                     |

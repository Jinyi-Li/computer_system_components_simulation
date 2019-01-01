## Simulated Computer System Components

The project was inspired by 15-513 Introduction to Computer Systems offered by Randy Bryant and Phil Gibbons, Carnegie Mellon University. It was developed in C to understand the foundations of computer systems.


cache_simulator.c - 

Simulate the cache behaviors on Load and Store requests, and count the numbers of hits, misses, evictions, dirty bytes in cache and evicted.


mm.c - 

Simulate the behaviors of linux methods malloc, calloc, realloc and free. It supports a full 64-bit address space, with a trade-off of correctness, space utilization and throughput. The experiment environment was Intel(R)Xeon(R)CPUE5520@2.27GHz, Intel Xeon, E5520, 2.27GHz. The performance was measured by both memory and CPU cycles. The benchmark results were provided by the embeded linux methods in the experiment environment.




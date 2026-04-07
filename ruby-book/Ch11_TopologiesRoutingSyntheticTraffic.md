# Chapter 11: Topologies, Routing, and Synthetic Traffic

> *A network is not one thing; it is a topology, a routing policy, a buffering policy, and a workload pattern interacting.*

This chapter will cover gem5's topology library (Crossbar, Mesh_XY, Mesh_westfirst, MeshDirCorners, Pt2Pt, Cluster, CustomMesh) and the GarnetSyntheticTraffic tester.
It will teach readers to generate latency-throughput curves via injection-rate sweeps, compare routing algorithms, and diagnose network saturation and hotspots.
Readers will run `garnet_synth_traffic.py` experiments with different topologies and traffic patterns (uniform random, tornado, bit-complement, etc.).
The failure-mode section will explain why studying only application workloads prevents the reader from ever learning the raw network limit or the onset of saturation.
By the end, readers can generate, interpret, and defend latency-throughput curves and diagnose network bottlenecks with purpose-built synthetic traffic.

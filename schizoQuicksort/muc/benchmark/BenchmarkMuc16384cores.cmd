#!/bin/bash
#@ job_type = parallel
#@ class = large
#@ node = 1024
#@ island_count = 2
#@ tasks_per_node = 16
#@ wall_clock_limit = 0:01:00
#@ job_name = BenchmarkMuc16384Cores
#@ network.MPI = sn_all,not_shared,us
#@ initialdir = $(home)/ShizoQuicksort/muc/benchmark
#@ output = job_$(job_name)_$(jobid).out
#@ error = job_$(job_name)_$(jobid).err
#@ notification=always
#@ notify_user=uydof@student.kit.edu
#@ queue

poe ~/ShizoQuicksort/build/benchmark

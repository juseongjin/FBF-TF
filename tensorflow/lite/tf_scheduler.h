#pragma once
#include <ctime>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <cstdarg>
#include <vector>
#include <utility>
#include <queue>
#include "condition_variable"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <error.h>
#include "opencv2/opencv.hpp"
#include <functional>
#include "thread"
#include "future"
#include "tensorflow/lite/util.h"

namespace tflite{

  typedef struct runtime_{
    int id;
    RuntimeState state;
    struct sockaddr_un addr;
    float latency[1000];
    int partitioning_plan[1000][3];
    // the partitioning plan's first idx means first subgraph's idx,
    // second idx means last subgraph's idx in subset.
    // thierd idx means which processor to be used in invoke.
    // 0 - cpu, 1 - gpu, 2 - cpgpu
  }runtime_;

  class TfScheduler{
    public:
      TfScheduler();
      TfScheduler(const char* uds_file_name);

      void PrintRuntimeStates();

      void Work();

      int SendPacketToRuntime(tf_packet& tx_p, struct sockaddr_un& runtime_addr);
      
      int ReceivePacketFromRuntime(tf_packet& rx_p, struct sockaddr_un& runtime_addr);
      
      // refresh runtime state in scheduler.
      void RefreshRuntimeState(tf_packet& rx_p);

      void CreatePartitioningPlan(tf_packet& rx_p, tf_packet& tx_p);

      bool RoundRobin(ResourceType type, int runtime_id);
      void ReleaseResource(ResourceType type);

      ~TfScheduler();
    
    private:
    int scheduler_fd;
    size_t addr_size;
    struct sockaddr_un scheduler_addr;

    std::vector<runtime_*> runtimes;
    int runtimes_created = 0;

    bool reschedule_needed = false;

    // For RR scheduler
    bool cpu_usage_flag = false;
    bool gpu_usage_flag = false;
    bool cpgpu_usage_flag = false;
    std::queue<int> rr_cpu_queue;
    std::queue<int> rr_gpu_queue;
  };

}
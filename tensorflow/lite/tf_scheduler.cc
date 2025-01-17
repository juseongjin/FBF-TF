#include "tensorflow/lite/tf_scheduler.h"

namespace tflite{

TfScheduler::TfScheduler() {};

TfScheduler::TfScheduler(const char* uds_file_name) {
  // delete if sock file already exists.
  if(access(uds_file_name, F_OK) == 0)
    unlink(uds_file_name);
  
  scheduler_fd = socket(PF_FILE, SOCK_DGRAM, 0);
  if(scheduler_fd == -1){
    std::cout << "Socker create ERROR" << "\n";
    exit(-1);
  }
  addr_size = sizeof(scheduler_addr);

  memset(&scheduler_addr, 0, sizeof(scheduler_addr));
  scheduler_addr.sun_family = AF_UNIX;
  strcpy(scheduler_addr.sun_path, uds_file_name);

  if(bind(scheduler_fd, (struct sockaddr*)&scheduler_addr, sizeof(scheduler_addr))
      == -1){
    std::cout << "Socket bind ERROR" << "\n";
    exit(-1);
  }
  std::cout << "Scheduler initializaing done" << "\n";
};

int TfScheduler::SendPacketToRuntime(tf_packet& tx_p,
                                      struct sockaddr_un& runtime_addr){
  int v;
  v = sendto(scheduler_fd, (void*)&tx_p, sizeof(tf_packet), 0,
               (struct sockaddr*)&runtime_addr, sizeof(runtime_addr));
  return v;
}

int TfScheduler::ReceivePacketFromRuntime(tf_packet& rx_p,
                                      struct sockaddr_un& runtime_addr){
   int v;
   v = recvfrom(scheduler_fd, &rx_p, sizeof(tf_packet), 0,
                (struct sockaddr*)&runtime_addr, (socklen_t*)&addr_size);
  return v;
}

void TfScheduler::SysMonitor(){
  std::cout << "[Scheduler] System monitoring started" << "\n";
  // std::thread cpu_monitor_daemon, gpu_monitor_daemon;
  // cpu_monitor_daemon = std::thread(&GetCPUUtilization, cpu_util);
  // gpu_monitor_daemon = std::thread(&GetGPUUtilization, gpu_util);
  // cpu_monitor_daemon.join();
  // gpu_monitor_daemon.join();
}

void TfScheduler::Work(){
  monitor = new LiteSysMonitor(&cpu_util, &gpu_util);
  while(1){
    tf_packet rx_packet;
    struct sockaddr_un runtime_addr;
    memset(&rx_packet, 0, sizeof(tf_packet));
    if(ReceivePacketFromRuntime(rx_packet, runtime_addr) == -1){
      std::cout << "Receive failed" << "\n";
      return;
    }
    //std::cout << "Recieved packet from runtime " << rx_packet.runtime_id << "\n";

    // do next work by received runtime state.
    switch (rx_packet.runtime_current_state)
    {
    case RuntimeState::INITIALIZE :{ 
      for(auto runtime : runtimes){
        if(runtime->id == rx_packet.runtime_id){
          std::cout << "Runtime " << runtime->id << " already registered." << "\n"; 
          break;
        }
      }
      // initializing new_runtime
      runtime_* new_runtime = new runtime_;
      new_runtime->id = runtimes_created;
      runtimes_created++;
      
      new_runtime->addr.sun_family = runtime_addr.sun_family;
      strcpy(new_runtime->addr.sun_path, runtime_addr.sun_path);
      
      tf_packet tx_packet;
      memset(&tx_packet, 0, sizeof(tf_packet));
      tx_packet.runtime_id = new_runtime->id;
      tx_packet.runtime_next_state = RuntimeState::NEED_PROFILE;

      if(SendPacketToRuntime(tx_packet, runtime_addr) == -1){
        std::cout << "Sending packet to " << new_runtime->id << " Failed" << "\n";
        std::cout << "sock : " << runtime_addr.sun_path  << " " << runtime_addr.sun_family << "\n";
        printf("errno : %d \n", errno);
        return;
      }
      runtimes.push_back(new_runtime);
      std::cout << "Registered new runtime " << new_runtime->id << " \n";
      break;
    }
    case RuntimeState::NEED_PROFILE :{
      tf_packet tx_packet;
      RefreshRuntimeState(rx_packet);
      CreatePartitioningPlan(rx_packet, tx_packet);
      
      tx_packet.runtime_id = rx_packet.runtime_id;
      tx_packet.runtime_next_state = RuntimeState::SUBGRAPH_CREATE;
      
      if(SendPacketToRuntime(tx_packet, runtime_addr) == -1){
        std::cout << "sock : " << runtime_addr.sun_path  << " " << runtime_addr.sun_family << "\n";
        printf("errno : %d \n", errno);
        return;
      }
      break;
    }
    case RuntimeState::SUBGRAPH_CREATE :{
      RefreshRuntimeState(rx_packet);
      // What to do here???
      // maybe schedulability check?
      tf_packet tx_packet;
      tx_packet.runtime_id = rx_packet.runtime_id;
      tx_packet.runtime_next_state = RuntimeState::INVOKE_;
      
      if(SendPacketToRuntime(tx_packet, runtime_addr) == -1){
        std::cout << "sock : " << runtime_addr.sun_path  << " " << runtime_addr.sun_family << "\n";
        printf("errno : %d \n", errno);
        return;
      }
      break;
    }
    case RuntimeState::INVOKE_ :{
      RefreshRuntimeState(rx_packet);
      tf_packet tx_packet;
      tx_packet.runtime_id = rx_packet.runtime_id;
      if(RoundRobin(static_cast<ResourceType>(rx_packet.cur_graph_resource), rx_packet.runtime_id)){
        // resource available
        tx_packet.runtime_next_state = RuntimeState::INVOKE_;
        std::cout << "Give resource to runtime " << rx_packet.runtime_id << "\n";
      }else{ // resource not available
        tx_packet.runtime_next_state = RuntimeState::BLOCKED_;
        std::cout << "Block runtime " << rx_packet.runtime_id << "\n";
      }
      if(SendPacketToRuntime(tx_packet, runtime_addr) == -1){
        std::cout << "sock : " << runtime_addr.sun_path  << " " << runtime_addr.sun_family << "\n";
        printf("errno : %d \n", errno);
        return;
      }
      break;
    }
    default:
      break;
    }
  }
  monitoring_thread.join();
}

bool TfScheduler::CheckAllRuntimesReady(){
  if(runtimes.size() != 2){
    return false;
  }
  for(int i=0; i<runtimes.size(); ++i){
    if(runtimes[i]->state != RuntimeState::INVOKE_)
      return false;
  }
  return true;
}

void TfScheduler::RefreshRuntimeState(tf_packet& rx_p){
  for(int i=0; i<runtimes.size(); ++i){
    if(rx_p.runtime_id == runtimes[i]->id){
      runtimes[i]->state = static_cast<RuntimeState>(rx_p.runtime_current_state);
    }
  }
}

bool TfScheduler::RoundRobin(ResourceType type, int runtime_id){
  // if(runtimes.size() < 2){
  //   return true;
  // }
  if(!CheckAllRuntimesReady()){ // Every runtime should be in invoke state to start RR scheduling.
    return false;
  }
  switch (type)
  {
  case ResourceType::CPU:
    if(rr_cpu_queue.empty()){ // initial state. any runtime can take ownership
      rr_cpu_queue.push(runtime_id);
      return true;      
    }
    else if(rr_cpu_queue.front() != runtime_id){ // if last owner was other runtime
      if(cpu_usage_flag) // Resource busy.
        return false;
      else{ // Resource available
        rr_cpu_queue.pop();
        rr_cpu_queue.push(runtime_id);
        cpu_usage_flag = true;
        return true;
      }
    }else
      return false; // if last owner was this runtime
  case ResourceType::GPU:
    if(rr_gpu_queue.empty()){ // initial state. any runtime can take ownership
      rr_gpu_queue.push(runtime_id);
      return true;      
    }
    else if(rr_gpu_queue.front() != runtime_id){ // if last owner was other runtime
      if(gpu_usage_flag) // Resource busy.
        return false;
      else{ // Resource available
        rr_gpu_queue.pop();
        rr_gpu_queue.push(runtime_id);
        cpu_usage_flag = true;
        return true;
      }
    }else
      return false; // if last owner was this runtime
  // case ResourceType::CPUGPU:
  //   /* Not implemented */
  //   break;
  default:
    break;
  }
}

void TfScheduler::ReleaseResource(ResourceType type){
  switch (type)
  {
  case ResourceType::CPU :
    cpu_usage_flag = false;
    break;
  
  case ResourceType::GPU :
    gpu_usage_flag = false;
    break;

  // case ResourceType::CPUGPU :
  //   cpgpu_usage_flag = false;
  //   break;

  default:
    break;
  }
  return;
}

void TfScheduler::PrintRuntimeStates(){
  std::cout << "===================================";
  std::cout << "TfScheduler has " << runtimes.size() << " runtimes" << "\n";
  for(int i=0; i<runtimes.size(); ++i){
  std::cout << "===================================";
    std::cout << "Runtime ID : " << runtimes[i]->id << "\n";
    std::cout << "Runtime State : " << runtimes[i]->state << "\n";
    std::cout << "Socket path :" << runtimes[i]->addr.sun_path << "\n";
  }
}


// STUB METHOD
// STUB METHOD
void TfScheduler::CreatePartitioningPlan(tf_packet& rx_p, tf_packet& tx_p){
  int layers = 0;
  for(int i=0; i<1000; ++i){
    if(rx_p.latency[i] == -1)
      layers++;
    else
      break;
  }
  std::cout << "Runtime [" << rx_p.runtime_id << "] has " << layers << 
    " layers in model" << "\n";
  if(layers == 9){ // MNIST

    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 9;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = TF_P_END_PLAN;
    
    // if want two subgraph
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 1;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 2; // partitioning ratio
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = 1;
    tx_p.partitioning_plan[1][TF_P_IDX_END]      = 9;
    tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[2][TF_P_IDX_START]    = TF_P_END_PLAN;
  } // MNIST
  else if(layers == 124){ // MOBILENET_V3 224 
  //(old, from TF model hub)
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 124;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = TF_P_END_PLAN;
  }else if(layers == 123){ // MOBILENET_V3 224 
  //(from https://github.com/tensorflow/models/tree/master/research/slim/nets/mobilenet)
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 123;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = TF_P_END_PLAN;
  }else if(layers == 31){ // MOBILENET_V1 224 
  //(from https://tfhub.dev/tensorflow/lite-model/mobilenet_v1_1.0_224/1/default/1)
    // TEST PLAN  -- HW & CH
    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 27;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 18; // partitioning ratio
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = 27;
    // tx_p.partitioning_plan[1][TF_P_IDX_END]      = 29;
    // tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    // tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 8; // partitioning ratio
    // tx_p.partitioning_plan[2][TF_P_IDX_START]    = 29;
    // tx_p.partitioning_plan[2][TF_P_IDX_END]      = 31;
    // tx_p.partitioning_plan[2][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[2][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[3][TF_P_IDX_START]    = TF_P_END_PLAN;

    // TEST PLAN  -- HW & CH (Dynamic delegate)
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 27;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 18; // partitioning ratio
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = 27;
    tx_p.partitioning_plan[1][TF_P_IDX_END]      = 29;
    tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 8; // partitioning ratio
    tx_p.partitioning_plan[2][TF_P_IDX_START]    = 29;
    tx_p.partitioning_plan[2][TF_P_IDX_END]      = 31;
    tx_p.partitioning_plan[2][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[2][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[3][TF_P_IDX_START]    = TF_P_END_PLAN;
    // accuracy : (orange, banana) = (93%, 80%) <= gpu delegation
    // accuracy : (orange, banana) = (93%, 80%) <= xnnpack delegation
    // accuracy : (orange, banana) = (Nan, Nan) <= multi delegation
    //////////////////////////////////////////////////////////////////////////////////////////////
    // 1. HW (0  ~ 26) => Resource type : GPU+CPU, Delegate : GPU or XNNPACK  (combination = 2) //
    // 2. CW (27 ~ 29) => Resource type : GPU+CPU, Delegate : GPU or XNNPACK  (combination = 2) //
    // 3. -- (29 ~ 30) => Resource type : CPU    , Delegate : Nan or XNNPACK  (combination = 2) //
    //////////////////////////////////////////////////////////////////////////////////////////////

    // BASELINE for CPU
    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 31;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = TF_P_END_PLAN;
    // accuracy : (orange, banana) = (99%, 88%)
    
    // BASELINE for GPU
    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 29;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = 29;
    // tx_p.partitioning_plan[1][TF_P_IDX_END]      = 31;
    // tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[2][TF_P_IDX_START]    = TF_P_END_PLAN;
    // accuracy : (orange, banana) = (99%, 88%)

    // TEST PLAN (Xavier) 7.2
    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 29;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 18; // partitioning ratio
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = 29;
    // tx_p.partitioning_plan[1][TF_P_IDX_END]      = 31;
    // tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[2][TF_P_IDX_START]    = TF_P_END_PLAN;

  }else if(layers == 118){ // efficientnet lite 4
  // layers == 118 for GPU FP32
  // layers == 120 for CPU UINT8
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 114;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 18; // partitioning ratio
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = 114;
    tx_p.partitioning_plan[1][TF_P_IDX_END]      = 118;
    tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[2][TF_P_IDX_START]    = TF_P_END_PLAN;

    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 118;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = TF_P_END_PLAN;
  }else if(layers == 152){ // yolo_v4_tiny-ieie
    // baselines
    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 152;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = TF_P_END_PLAN;

    // sj
    // TEST HW/CW Multi Delegate
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 8;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 15; // partitioning ratio
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = 8;
    tx_p.partitioning_plan[1][TF_P_IDX_END]      = 9;
    tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[2][TF_P_IDX_START]    = 9;
    tx_p.partitioning_plan[2][TF_P_IDX_END]      = 20;
    tx_p.partitioning_plan[2][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    tx_p.partitioning_plan[2][TF_P_IDX_RATIO]    = 15; // partitioning ratio
    tx_p.partitioning_plan[3][TF_P_IDX_START]    = 20;
    tx_p.partitioning_plan[3][TF_P_IDX_END]      = 21;
    tx_p.partitioning_plan[3][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[3][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[4][TF_P_IDX_START]    = 21;
    tx_p.partitioning_plan[4][TF_P_IDX_END]      = 32;
    tx_p.partitioning_plan[4][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    tx_p.partitioning_plan[4][TF_P_IDX_RATIO]    = 15; // partitioning ratio

    // for Co-C-Co-C-Co-C-G-C
    tx_p.partitioning_plan[5][TF_P_IDX_START]    = 32;
    tx_p.partitioning_plan[5][TF_P_IDX_END]      = 33;
    tx_p.partitioning_plan[5][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[5][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[6][TF_P_IDX_START]    = 33; // problem on node 52
    tx_p.partitioning_plan[6][TF_P_IDX_END]      = 55; // 102?
    tx_p.partitioning_plan[6][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    tx_p.partitioning_plan[6][TF_P_IDX_RATIO]    = 0; // partitioning ratio 17
    tx_p.partitioning_plan[7][TF_P_IDX_START]    = 55;
    tx_p.partitioning_plan[7][TF_P_IDX_END]      = 152;
    tx_p.partitioning_plan[7][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[7][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[8][TF_P_IDX_START]    = TF_P_END_PLAN;

    // for Co-C-Co-C-Co-C
    // tx_p.partitioning_plan[5][TF_P_IDX_START]    = 32;
    // tx_p.partitioning_plan[5][TF_P_IDX_END]      = 152;
    // tx_p.partitioning_plan[5][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[5][TF_P_IDX_RATIO]    = 0;
    // tx_p.partitioning_plan[6][TF_P_IDX_START]    = TF_P_END_PLAN;

    // Minsung
    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 8;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = 8;
    // tx_p.partitioning_plan[1][TF_P_IDX_END]      = 9;
    // tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[2][TF_P_IDX_START]    = 9;
    // tx_p.partitioning_plan[2][TF_P_IDX_END]      = 20;
    // tx_p.partitioning_plan[2][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[2][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[3][TF_P_IDX_START]    = 20;
    // tx_p.partitioning_plan[3][TF_P_IDX_END]      = 21;
    // tx_p.partitioning_plan[3][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[3][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[4][TF_P_IDX_START]    = 21;
    // tx_p.partitioning_plan[4][TF_P_IDX_END]      = 32;
    // tx_p.partitioning_plan[4][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[4][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[5][TF_P_IDX_START]    = 32;
    // tx_p.partitioning_plan[5][TF_P_IDX_END]      = 33;
    // tx_p.partitioning_plan[5][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[5][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[6][TF_P_IDX_START]    = 33; // problem on node 52
    // tx_p.partitioning_plan[6][TF_P_IDX_END]      = 55; // 102?
    // tx_p.partitioning_plan[6][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[6][TF_P_IDX_RATIO]    = 0; // partitioning ratio 17
    // tx_p.partitioning_plan[7][TF_P_IDX_START]    = 55;
    // tx_p.partitioning_plan[7][TF_P_IDX_END]      = 152;
    // tx_p.partitioning_plan[7][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[7][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[8][TF_P_IDX_START]    = TF_P_END_PLAN;
    
    // tx_p.partitioning_plan[9][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[9][TF_P_IDX_END]      = 8;
    // tx_p.partitioning_plan[9][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    // tx_p.partitioning_plan[9][TF_P_IDX_RATIO]    = 15; // partitioning ratio
    // tx_p.partitioning_plan[10][TF_P_IDX_START]    = TF_P_END_PLAN;
    
    // tx_p.partitioning_plan[11][TF_P_IDX_START]    = TF_P_END_MASTER;

    //
  }
  else if(layers == 59){ //yolov4_tiny from pinto
  // for gpu
  // 0 ~ 7
  // 9 ~ 19
  // 21 ~ 31
  // 33 ~ 50  -> testing subgraph
  // for cpu(minimal precision, int8) 38 ~ 57 is co-execution subgraph 
  // node 33 -> conv2d input 1 26 26 128
  //
  // 55 ~ 58

    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 8;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = 8;
    // tx_p.partitioning_plan[1][TF_P_IDX_END]      = 9;
    // tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[2][TF_P_IDX_START]    = 9;
    // tx_p.partitioning_plan[2][TF_P_IDX_END]      = 19;
    // tx_p.partitioning_plan[2][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[2][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[3][TF_P_IDX_START]    = 19;
    // tx_p.partitioning_plan[3][TF_P_IDX_END]      = 21;
    // tx_p.partitioning_plan[3][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[3][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[4][TF_P_IDX_START]    = 21;
    // tx_p.partitioning_plan[4][TF_P_IDX_END]      = 31;
    // tx_p.partitioning_plan[4][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[4][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[5][TF_P_IDX_START]    = 31;
    // tx_p.partitioning_plan[5][TF_P_IDX_END]      = 33;
    // tx_p.partitioning_plan[5][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[5][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    // tx_p.partitioning_plan[6][TF_P_IDX_START]    = 33; 
    // tx_p.partitioning_plan[6][TF_P_IDX_END]      = 50;
    // tx_p.partitioning_plan[6][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    // tx_p.partitioning_plan[6][TF_P_IDX_RATIO]    = 15; // partitioning ratio
    // tx_p.partitioning_plan[7][TF_P_IDX_START]    = 50;
    // tx_p.partitioning_plan[7][TF_P_IDX_END]      = 56;
    // tx_p.partitioning_plan[7][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[7][TF_P_IDX_RATIO]    = 0;
    // tx_p.partitioning_plan[8][TF_P_IDX_START]    = 56;
    // tx_p.partitioning_plan[8][TF_P_IDX_END]      = 59;
    // tx_p.partitioning_plan[8][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[8][TF_P_IDX_RATIO]    = 0;
    // tx_p.partitioning_plan[9][TF_P_IDX_START]    = TF_P_END_PLAN;


    // CPU execution for debugging
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 59;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0;
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = TF_P_END_PLAN;

  }
  else if(layers == 68){ // case of yolo v4 tiny cpu (including quantize layer)
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 8;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = 8;
    tx_p.partitioning_plan[1][TF_P_IDX_END]      = 9;
    tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[2][TF_P_IDX_START]    = 9;
    tx_p.partitioning_plan[2][TF_P_IDX_END]      = 21;
    tx_p.partitioning_plan[2][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[2][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[3][TF_P_IDX_START]    = 21;
    tx_p.partitioning_plan[3][TF_P_IDX_END]      = 23;
    tx_p.partitioning_plan[3][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[3][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[4][TF_P_IDX_START]    = 23;
    tx_p.partitioning_plan[4][TF_P_IDX_END]      = 36;
    tx_p.partitioning_plan[4][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[4][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[5][TF_P_IDX_START]    = 36;
    tx_p.partitioning_plan[5][TF_P_IDX_END]      = 38;
    tx_p.partitioning_plan[5][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[5][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[6][TF_P_IDX_START]    = 38; 
    tx_p.partitioning_plan[6][TF_P_IDX_END]      = 58;
    tx_p.partitioning_plan[6][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[6][TF_P_IDX_RATIO]    = 0; // partitioning ratio
    tx_p.partitioning_plan[7][TF_P_IDX_START]    = 58;
    tx_p.partitioning_plan[7][TF_P_IDX_END]      = 65;
    tx_p.partitioning_plan[7][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[7][TF_P_IDX_RATIO]    = 0;
    tx_p.partitioning_plan[8][TF_P_IDX_START]    = 65;
    tx_p.partitioning_plan[8][TF_P_IDX_END]      = 68;
    tx_p.partitioning_plan[8][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[8][TF_P_IDX_RATIO]    = 0;
    tx_p.partitioning_plan[9][TF_P_IDX_START]    = TF_P_END_PLAN;
  }else if(layers == 52){ // ultra fast lanenet
  // 52 for FP32. 54 for int8
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 47;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 15;
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = 47;
    tx_p.partitioning_plan[1][TF_P_IDX_END]      = 52;
    tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0;
    tx_p.partitioning_plan[2][TF_P_IDX_START]    = TF_P_END_PLAN;     


    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 47;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_GPU;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0;
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = 47;
    // tx_p.partitioning_plan[1][TF_P_IDX_END]      = 52;
    // tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0;
    // tx_p.partitioning_plan[2][TF_P_IDX_START]    = TF_P_END_PLAN;      

    // FOR INT8 
    // tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    // tx_p.partitioning_plan[0][TF_P_IDX_END]      = 54;
    // tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    // tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0;
    // tx_p.partitioning_plan[1][TF_P_IDX_START]    = TF_P_END_PLAN;
    
  }else if(layers == 54){
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 47;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CO_E;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 15;
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = 47;
    tx_p.partitioning_plan[1][TF_P_IDX_END]      = 52;
    tx_p.partitioning_plan[1][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[1][TF_P_IDX_RATIO]    = 0;
    tx_p.partitioning_plan[2][TF_P_IDX_START]    = TF_P_END_PLAN;         
  }
  else{
    tx_p.partitioning_plan[0][TF_P_IDX_START]    = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_END]      = 0;
    tx_p.partitioning_plan[0][TF_P_IDX_RESOURCE] = TF_P_PLAN_CPU;
    tx_p.partitioning_plan[0][TF_P_IDX_RATIO]    = 0;
    tx_p.partitioning_plan[1][TF_P_IDX_START]    = TF_P_END_PLAN;  
  }
}

TfScheduler::~TfScheduler() {};

}


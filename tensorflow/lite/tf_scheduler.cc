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

void TfScheduler::Work(){
  while(1){
    tf_packet rx_packet;
    struct sockaddr_un runtime_addr;
    memset(&rx_packet, 0, sizeof(tf_packet));
    if(ReceivePacketFromRuntime(rx_packet, runtime_addr) == -1){
      std::cout << "Receive failed" << "\n";
      return;
    }
    std::cout << "Recieved packet from runtime " << rx_packet.runtime_id << "\n";

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
}

void TfScheduler::RefreshRuntimeState(tf_packet& rx_p){
  for(int i=0; i<runtimes.size(); ++i){
    if(rx_p.runtime_id == runtimes[i]->id){
      runtimes[i]->state = static_cast<RuntimeState>(rx_p.runtime_current_state);
    }
  }
}

bool TfScheduler::RoundRobin(ResourceType type, int runtime_id){
  if(runtimes.size() < 2){
    return true;
  }
  switch (type)
  {
  case ResourceType::CPU:
    if(rr_cpu_queue.empty() || rr_cpu_queue.front() != runtime_id){
      if(cpu_usage_flag) // cpu not available
        return false;
      else{ // Allocate resource to given runtime.
        rr_cpu_queue.pop();
        rr_cpu_queue.push(runtime_id);
        cpu_usage_flag = true;
        return true;
      }
    }else{
      return false;
    }
    break;
  case ResourceType::GPU:
    if(rr_gpu_queue.empty() || rr_gpu_queue.front() != runtime_id){
      if(gpu_usage_flag) // gpu not available
        return false;
      else{ // Allocate resource to given runtime.
        rr_gpu_queue.pop();
        rr_gpu_queue.push(runtime_id);
        gpu_usage_flag = true;
        return true;
      }
    }else{
      return false;
    }
    break;
  case ResourceType::CPUGPU:
    /* Not implemented */
    break;
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

  case ResourceType::CPUGPU :
    cpgpu_usage_flag = false;
    break;

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
    tx_p.partitioning_plan[0][0] = 0;
    tx_p.partitioning_plan[0][1] = 4;
    tx_p.partitioning_plan[0][2] = 0;
    tx_p.partitioning_plan[1][0] = 4;
    tx_p.partitioning_plan[1][1] = 9;
    tx_p.partitioning_plan[1][2] = 0;
    tx_p.partitioning_plan[2][0] = -1;
  } // MNIST

  
}

TfScheduler::~TfScheduler() {};

}


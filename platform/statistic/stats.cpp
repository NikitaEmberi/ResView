/*
 * Copyright (c) 2019-2022 ExpoLab, UC Davis
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "platform/statistic/stats.h"

#include <glog/logging.h>
#include <ctime>
#include "common/utils/utils.h"
#include "proto/kv/kv.pb.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

namespace resdb {

std::mutex g_mutex;
Stats* Stats::GetGlobalStats(int seconds) {
  std::unique_lock<std::mutex> lk(g_mutex);
  static Stats stats(seconds);
  return &stats;
}

Stats::Stats(int sleep_time) {
  monitor_sleep_time_ = sleep_time;
#ifdef TEST_MODE
  monitor_sleep_time_ = 1;
#endif
  num_call_ = 0;
  num_commit_ = 0;
  run_time_ = 0;
  run_call_ = 0;
  run_call_time_ = 0;
  server_call_ = 0;
  server_process_ = 0;
  run_req_num_ = 0;
  run_req_run_time_ = 0;
  seq_gap_ = 0;
  total_request_ = 0;
  total_geo_request_ = 0;
  geo_request_ = 0;

  stop_ = false;
  begin_ = false;

  socket_recv_ = 0;
  broad_cast_msg_ = 0;
  send_broad_cast_msg_ = 0;

  prometheus_ = nullptr;
  global_thread_ =
      std::thread(&Stats::MonitorGlobal, this);  // pass by reference

  transaction_summary_.port=-1;

  //Setup websocket here
  send_summary_.store(false);
  make_faulty_.store(false);
  transaction_summary_.request_pre_prepare_state_time=std::chrono::system_clock::time_point::min();
  transaction_summary_.prepare_state_time=std::chrono::system_clock::time_point::min();
  transaction_summary_.commit_state_time=std::chrono::system_clock::time_point::min();
  transaction_summary_.execution_time=std::chrono::system_clock::time_point::min();
  transaction_summary_.txn_number=0;
  
}

void Stats::Stop() { stop_ = true; }

Stats::~Stats() {
  stop_ = true;
  if (global_thread_.joinable()) {
    global_thread_.join();
  }
  if(summary_thread_.joinable()){
    summary_thread_.join();
  }
  if(faulty_thread_.joinable()){
    faulty_thread_.join();
  }
}

void Stats::SocketManagementWrite(){
  while(!stop_){
    try{
      LOG(ERROR)<<"Port:" <<transaction_summary_.port;
      asio::io_context io_context;
      tcp::acceptor acceptor(io_context, {{}, (boost::asio::ip::port_type)(11000+transaction_summary_.port)});
      tcp::socket socket(io_context);
      acceptor.accept(socket);
      beast::websocket::stream<tcp::socket> ws(std::move(socket));
      ws.accept();
      while(!stop_){
        if(send_summary_.load()){
          ws.write(asio::buffer(summary_json_.dump()));
          summary_json_={};
          LOG(ERROR)<<"SENT MESSAGE";
          send_summary_.store(false);
        }
        if(!ws.is_open()){
          break;
        }
      }
      sleep(1);
    }
    catch( const std::exception& e){
      LOG(ERROR)<<"Exception: " <<e.what();
    }
  }
}

void Stats::SocketManagementRead(){
  while(!stop_){
    try{
      LOG(ERROR)<<"Read Port:" <<transaction_summary_.port;
      asio::io_context io_context;
      tcp::acceptor acceptor(io_context, {{}, (boost::asio::ip::port_type)(12000+transaction_summary_.port)});
      tcp::socket socket(io_context);
      acceptor.accept(socket);
      beast::websocket::stream<tcp::socket> ws(std::move(socket));
      ws.accept();
      beast::flat_buffer data;
      ws.read(data);
      make_faulty_.store(true);
      LOG(ERROR)<<"Received Message on port "<<transaction_summary_.port;
      ws.close("Message Received");
    }
    catch( const std::exception& e){
      LOG(ERROR)<<"Exception: " <<e.what();
    }
  }
}

void Stats::SetProps(int replica_id, std::string ip, int port){
  transaction_summary_.replica_id=replica_id;
  transaction_summary_.ip=ip;
  transaction_summary_.port=port;
  summary_thread_ = std::thread(&Stats::SocketManagementWrite, this);
  faulty_thread_ = std::thread(&Stats::SocketManagementRead, this);
}

void Stats::SetPrimaryId(int primary_id){
  transaction_summary_.primary_id=primary_id;
}

void Stats::RecordStateTime(std::string state){
  if(state=="request" || state=="pre-prepare"){
    transaction_summary_.request_pre_prepare_state_time=std::chrono::system_clock::now();
  }
  else if(state=="prepare"){
    transaction_summary_.prepare_state_time=std::chrono::system_clock::now();
  }
  else if(state=="commit"){
    transaction_summary_.commit_state_time=std::chrono::system_clock::now();
  }
}

void Stats::GetTransactionDetails(BatchUserRequest batch_request){
  transaction_summary_.txn_command.clear();
  transaction_summary_.txn_key.clear();
  transaction_summary_.txn_value.clear();
  for (auto& sub_request : batch_request.user_requests()) {
    KVRequest kv_request;
    if(!kv_request.ParseFromString(sub_request.request().data())){
      break;
    }
    if (kv_request.cmd() == KVRequest::SET) {
      transaction_summary_.txn_command.push_back("SET");
      transaction_summary_.txn_key.push_back(kv_request.key());
      transaction_summary_.txn_value.push_back(kv_request.value());
    } else if (kv_request.cmd() == KVRequest::GET) {
      transaction_summary_.txn_command.push_back("GET");
      transaction_summary_.txn_key.push_back(kv_request.key());
      transaction_summary_.txn_value.push_back("");
    } else if (kv_request.cmd() == KVRequest::GETALLVALUES) {
      transaction_summary_.txn_command.push_back("GETALLVALUES");
      transaction_summary_.txn_key.push_back(kv_request.key());
      transaction_summary_.txn_value.push_back("");
    } else if (kv_request.cmd() == KVRequest::GETRANGE) {
      transaction_summary_.txn_command.push_back("GETRANGE");
      transaction_summary_.txn_key.push_back(kv_request.key());
      transaction_summary_.txn_value.push_back(kv_request.value());
    }
  }
}

void Stats::SendSummary(){
  transaction_summary_.execution_time=std::chrono::system_clock::now();
  transaction_summary_.txn_number=transaction_summary_.txn_number+1;
  /* Can print stat values
  LOG(ERROR)<<"Replica ID:"<< transaction_summary_.replica_id;
  LOG(ERROR)<<"Primary ID:"<< transaction_summary_.primary_id;
  LOG(ERROR)<<"Propose/pre-prepare time:"<< transaction_summary_.request_pre_prepare_state_time.time_since_epoch().count();
  LOG(ERROR)<<"Prepare time:"<< transaction_summary_.prepare_state_time.time_since_epoch().count();
  LOG(ERROR)<<"Commit time:"<< transaction_summary_.commit_state_time.time_since_epoch().count();
  LOG(ERROR)<<"Execution time:"<< transaction_summary_.execution_time.time_since_epoch().count();
  for(size_t i=0; i<transaction_summary_.prepare_message_count_times_list.size(); i++){
    LOG(ERROR)<<" Prepare Message Count Time: " << transaction_summary_.prepare_message_count_times_list[i].time_since_epoch().count();
  } 
  for(size_t i=0; i<transaction_summary_.commit_message_count_times_list.size(); i++){
    LOG(ERROR)<<" Commit Message Count Time: " << transaction_summary_.commit_message_count_times_list[i].time_since_epoch().count();
  }
  */
 
  //Convert Transaction Summary to JSON
  summary_json_["replica_id"]=transaction_summary_.replica_id;
  summary_json_["ip"]=transaction_summary_.ip;
  summary_json_["port"]=transaction_summary_.port;
  summary_json_["primary_id"]=transaction_summary_.primary_id;
  summary_json_["propose_pre_prepare_time"]=transaction_summary_.request_pre_prepare_state_time.time_since_epoch().count();
  summary_json_["prepare_time"]=transaction_summary_.prepare_state_time.time_since_epoch().count();
  summary_json_["commit_time"]=transaction_summary_.commit_state_time.time_since_epoch().count();
  summary_json_["execution_time"]=transaction_summary_.execution_time.time_since_epoch().count();
  for(size_t i=0; i<transaction_summary_.prepare_message_count_times_list.size(); i++){
    summary_json_["prepare_message_timestamps"].push_back(transaction_summary_.prepare_message_count_times_list[i].time_since_epoch().count());
  }
  for(size_t i=0; i<transaction_summary_.commit_message_count_times_list.size(); i++){
    summary_json_["commit_message_timestamps"].push_back(transaction_summary_.commit_message_count_times_list[i].time_since_epoch().count());
  }
  summary_json_["txn_number"]=transaction_summary_.txn_number;
  for(size_t i=0; i<transaction_summary_.txn_command.size(); i++){
    summary_json_["txn_commands"].push_back(transaction_summary_.txn_command[i]);
  }
  for(size_t i=0; i<transaction_summary_.txn_key.size(); i++){
    summary_json_["txn_keys"].push_back(transaction_summary_.txn_key[i]);
  }
  for(size_t i=0; i<transaction_summary_.txn_value.size(); i++){
    summary_json_["txn_values"].push_back(transaction_summary_.txn_value[i]);
  }

  LOG(ERROR)<<summary_json_.dump();

  //Send Summary via Websocket

  send_summary_.store(true);
  while(!send_summary_.load()){}
  //Reset Transaction Summary Parameters
  transaction_summary_.request_pre_prepare_state_time=std::chrono::system_clock::time_point::min();
  transaction_summary_.prepare_state_time=std::chrono::system_clock::time_point::min();
  transaction_summary_.commit_state_time=std::chrono::system_clock::time_point::min();
  transaction_summary_.execution_time=std::chrono::system_clock::time_point::min();
  transaction_summary_.prepare_message_count_times_list.clear();
  transaction_summary_.commit_message_count_times_list.clear();
}

void Stats::MonitorGlobal() {
  LOG(ERROR) << "monitor:" << name_ << " sleep time:" << monitor_sleep_time_;

  uint64_t seq_fail = 0;
  uint64_t client_call = 0, socket_recv = 0;
  uint64_t num_client_req = 0, num_propose = 0, num_prepare = 0, num_commit = 0,
           pending_execute = 0, execute = 0, execute_done = 0;
  uint64_t broad_cast_msg = 0, send_broad_cast_msg = 0;
  uint64_t send_broad_cast_msg_per_rep = 0;
  uint64_t server_call = 0, server_process = 0;
  uint64_t seq_gap = 0;
  uint64_t total_request = 0, total_geo_request = 0, geo_request = 0;

  // ====== for client proxy ======
  uint64_t run_req_num = 0, run_req_run_time = 0;

  uint64_t last_run_req_num = 0, last_run_req_run_time = 0;
  // =============================

  uint64_t last_seq_fail = 0;
  uint64_t last_num_client_req = 0, last_num_propose = 0, last_num_prepare = 0,
           last_num_commit = 0;
  uint64_t last_pending_execute = 0, last_execute = 0, last_execute_done = 0;
  uint64_t last_client_call = 0, last_socket_recv = 0;
  uint64_t last_broad_cast_msg = 0, last_send_broad_cast_msg = 0;
  uint64_t last_send_broad_cast_msg_per_rep = 0;
  uint64_t last_server_call = 0, last_server_process = 0;
  uint64_t last_total_request = 0, last_total_geo_request = 0,
           last_geo_request = 0;
  uint64_t time = 0;

  while (!stop_) {
    sleep(monitor_sleep_time_);
    time += monitor_sleep_time_;
    seq_fail = seq_fail_;
    socket_recv = socket_recv_;
    client_call = client_call_;
    num_client_req = num_client_req_;
    num_propose = num_propose_;
    num_prepare = num_prepare_;
    num_commit = num_commit_;
    pending_execute = pending_execute_;
    execute = execute_;
    execute_done = execute_done_;
    broad_cast_msg = broad_cast_msg_;
    send_broad_cast_msg = send_broad_cast_msg_;
    send_broad_cast_msg_per_rep = send_broad_cast_msg_per_rep_;
    server_call = server_call_;
    server_process = server_process_;
    seq_gap = seq_gap_;
    total_request = total_request_;
    total_geo_request = total_geo_request_;
    geo_request = geo_request_;

    run_req_num = run_req_num_;
    run_req_run_time = run_req_run_time_;

    LOG(ERROR) << "=========== monitor =========\n"
               << "server call:" << server_call - last_server_call
               << " server process:" << server_process - last_server_process
               << " socket recv:" << socket_recv - last_socket_recv
               << " "
                  "client call:"
               << client_call - last_client_call
               << " "
                  "client req:"
               << num_client_req - last_num_client_req
               << " "
                  "broad_cast:"
               << broad_cast_msg - last_broad_cast_msg
               << " "
                  "send broad_cast:"
               << send_broad_cast_msg - last_send_broad_cast_msg
               << " "
                  "per send broad_cast:"
               << send_broad_cast_msg_per_rep - last_send_broad_cast_msg_per_rep
               << " "
                  "propose:"
               << num_propose - last_num_propose
               << " "
                  "prepare:"
               << (num_prepare - last_num_prepare)
               << " "
                  "commit:"
               << (num_commit - last_num_commit)
               << " "
                  "pending execute:"
               << pending_execute - last_pending_execute
               << " "
                  "execute:"
               << execute - last_execute
               << " "
                  "execute done:"
               << execute_done - last_execute_done << " seq gap:" << seq_gap
               << " total request:" << total_request - last_total_request
               << " txn:" << (total_request - last_total_request) / 5
               << " total geo request:"
               << total_geo_request - last_total_geo_request
               << " total geo request per:"
               << (total_geo_request - last_total_geo_request) / 5
               << " geo request:" << (geo_request - last_geo_request)
               << " "
                  "seq fail:"
               << seq_fail - last_seq_fail << " time:" << time
               << " "
                  "\n--------------- monitor ------------";
    if (run_req_num - last_run_req_num > 0) {
      LOG(ERROR) << "  req client latency:"
                 << static_cast<double>(run_req_run_time -
                                        last_run_req_run_time) /
                        (run_req_num - last_run_req_num) / 1000000000.0;
    }

    last_seq_fail = seq_fail;
    last_socket_recv = socket_recv;
    last_client_call = client_call;
    last_num_client_req = num_client_req;
    last_num_propose = num_propose;
    last_num_prepare = num_prepare;
    last_num_commit = num_commit;
    last_pending_execute = pending_execute;
    last_execute = execute;
    last_execute_done = execute_done;

    last_broad_cast_msg = broad_cast_msg;
    last_send_broad_cast_msg = send_broad_cast_msg;
    last_send_broad_cast_msg_per_rep = send_broad_cast_msg_per_rep;

    last_server_call = server_call;
    last_server_process = server_process;

    last_run_req_num = run_req_num;
    last_run_req_run_time = run_req_run_time;
    last_total_request = total_request;
    last_total_geo_request = total_geo_request;
    last_geo_request = geo_request;
  }
}

void Stats::IncClientCall() {
  if (prometheus_) {
    prometheus_->Inc(CLIENT_CALL, 1);
  }
  client_call_++;
}

void Stats::IncClientRequest() {
  if (prometheus_) {
    prometheus_->Inc(CLIENT_REQ, 1);
  }
  num_client_req_++;
}

void Stats::IncPropose() {
  if (prometheus_) {
    prometheus_->Inc(PROPOSE, 1);
  }
  num_propose_++;
}

void Stats::IncPrepare() {
  if (prometheus_) {
    prometheus_->Inc(PREPARE, 1);
  }
  num_prepare_++;
  transaction_summary_.prepare_message_count_times_list.push_back(std::chrono::system_clock::now());
}

void Stats::IncCommit() {
  if (prometheus_) {
    prometheus_->Inc(COMMIT, 1);
  }
  num_commit_++;
  transaction_summary_.commit_message_count_times_list.push_back(std::chrono::system_clock::now());
}

void Stats::IncPendingExecute() {
  pending_execute_++; 
}

void Stats::IncExecute() { execute_++; }

void Stats::IncExecuteDone() {
  if (prometheus_) {
    prometheus_->Inc(EXECUTE, 1);
  }
  execute_done_++;
}

void Stats::BroadCastMsg() {
  if (prometheus_) {
    prometheus_->Inc(BROAD_CAST, 1);
  }
  broad_cast_msg_++;
}

void Stats::SendBroadCastMsg(uint32_t num) { send_broad_cast_msg_ += num; }

void Stats::SendBroadCastMsgPerRep() { send_broad_cast_msg_per_rep_++; }

void Stats::SeqFail() { seq_fail_++; }

void Stats::IncTotalRequest(uint32_t num) {
  if (prometheus_) {
    prometheus_->Inc(NUM_EXECUTE_TX, num);
  }
  total_request_ += num;
}

void Stats::IncTotalGeoRequest(uint32_t num) { total_geo_request_ += num; }

void Stats::IncGeoRequest() { geo_request_++; }

void Stats::ServerCall() {
  if (prometheus_) {
    prometheus_->Inc(SERVER_CALL_NAME, 1);
  }
  server_call_++;
}

void Stats::ServerProcess() {
  if (prometheus_) {
    prometheus_->Inc(SERVER_PROCESS, 1);
  }
  server_process_++;
}

void Stats::SeqGap(uint64_t seq_gap) { seq_gap_ = seq_gap; }

void Stats::AddLatency(uint64_t run_time) {
  run_req_num_++;
  run_req_run_time_ += run_time;
}

void Stats::SetPrometheus(const std::string& prometheus_address) {
  prometheus_ = std::make_unique<PrometheusHandler>(prometheus_address);
}

}  // namespace resdb

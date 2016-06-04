#ifndef CEPH_MSG_RDMASTACK_H
#define CEPH_MSG_RDMASTACK_H

#include "common/ceph_context.h"
#include "common/debug.h"
#include "common/errno.h"
#include "msg/async/Stack.h"
#include <thread>
#include "Infiniband.h"

class RDMAConnectedSocketImpl : public ConnectedSocketImpl {
  typedef Infiniband::MemoryManager::Chunk Chunk;
  typedef Infiniband::CompletionChannel CompletionChannel;
  typedef Infiniband::CompletionQueue CompletionQueue;
  CephContext *cct;
  Infiniband::QueuePair *qp;
  IBSYNMsg peer_msg;
  IBSYNMsg my_msg;
  int connected;
  Infiniband* infiniband;
  vector<Chunk*> rx_buffers;
  CompletionChannel* cc;
  CompletionQueue* cq;
  bool rearmed;

  public:
  RDMAConnectedSocketImpl(CephContext *cct, Infiniband* ib, IBSYNMsg im = IBSYNMsg()) : cct(cct), peer_msg(im), infiniband(ib) {
    qp = infiniband->create_queue_pair(IBV_QPT_RC);
    cq = qp->get_rx_cq();
    cc = cq->get_cc();
    my_msg.qpn = qp->get_local_qp_number();
    my_msg.psn = qp->get_initial_psn();
    my_msg.lid = infiniband->get_lid();
    my_msg.gid = infiniband->get_gid();
  }

  virtual int is_connected() override {
    return connected;
  }
  virtual ssize_t read(char* buf, size_t len) override;
  virtual ssize_t zero_copy_read(bufferptr &data) override;
  virtual ssize_t send(bufferlist &bl, bool more) override;
  virtual void shutdown() override {
  }
  virtual void close() override {
  }
  virtual int fd() const override {
    return cc->get_fd();
  }
  int activate();
  ssize_t read_buffers(char* buf, size_t len);
  int poll_cq(int num_entries, ibv_wc *ret_wc_array);
  IBSYNMsg get_my_msg() { return my_msg; }
  IBSYNMsg get_peer_msg() { return peer_msg; }
  void set_peer_msg(IBSYNMsg m) { peer_msg = m ;}
};

class RDMAServerSocketImpl : public ServerSocketImpl {
  CephContext *cct;
  int server_setup_socket;
  Infiniband* infiniband;
  entity_addr_t sa;
  public:
  RDMAServerSocketImpl(CephContext *cct, Infiniband* i, entity_addr_t& a) : cct(cct), infiniband(i), sa(a) {}
  int listen(entity_addr_t &sa, const SocketOptions &opt);
  virtual int accept(ConnectedSocket *s, const SocketOptions &opts, entity_addr_t *out) override;
  virtual void abort_accept() override {}
  virtual int fd() const override {
    return server_setup_socket;
  } 
};

class RDMAWorker : public Worker {
  typedef Infiniband::CompletionQueue CompletionQueue;
  typedef Infiniband::CompletionChannel CompletionChannel;
  typedef Infiniband::MemoryManager::Chunk Chunk;
  int client_setup_socket;
  Infiniband* infiniband;
  CompletionQueue* tx_cq;           // common completion queue for all transmits
  CompletionChannel* tx_cc;
  EventCallbackRef tx_handler;
  bool rearmed;
  class C_handle_cq_tx : public EventCallback {
    RDMAWorker *worker;
    public:
    C_handle_cq_tx(RDMAWorker *w): worker(w) {}
    void do_request(int fd) {
      worker->handle_tx_event();
    }
  };

  public:
  explicit RDMAWorker(CephContext *c, unsigned i): Worker(c, i), infiniband(NULL), tx_handler(new C_handle_cq_tx(this))  {}

  virtual int listen(entity_addr_t &addr, const SocketOptions &opts, ServerSocket *) override;
  virtual int connect(const entity_addr_t &addr, const SocketOptions &opts, ConnectedSocket *socket) override;
  void connect(const entity_addr_t &peer_addr);
  void initialize();
  void destroy() {
  }
  void set_ib(Infiniband* ib) { infiniband = ib; }
  void handle_tx_event();
  CompletionQueue* get_tx_cq() { return tx_cq; }
};

class RDMAStack : public NetworkStack {
  vector<int> coreids;
  vector<std::thread> threads;
  Infiniband* infiniband;
  public:
  explicit RDMAStack(CephContext *cct, const string &t);

  int get_cpuid(int id) {
    if (coreids.empty())
      return -1;
    return coreids[id % coreids.size()];
  }

  virtual bool support_zero_copy_read() const override { return true; }
  //virtual bool support_local_listen_table() const { return true; }

  virtual void spawn_workers(std::vector<std::function<void ()>> &funcs) override {
    // used to tests
    for (auto &&func : funcs)
      threads.emplace_back(std::thread(std::move(func))); // this is happen in actual env
  }
  virtual void join_workers() override {
    for (auto &&t : threads)
      t.join();
    threads.clear();
  }
};
#endif

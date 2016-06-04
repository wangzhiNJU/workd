#ifndef CEPH_INFINIBAND_H
#define CEPH_INFINIBAND_H

#include "include/int_types.h"
#include "ResourceManager.h"
#include "common/debug.h"
#include "common/errno.h"
#include "msg/msg_types.h"
#include <vector>
using std::vector;

struct IBSYNMsg {
  uint16_t lid;
  uint32_t qpn;
  uint32_t psn;
  union ibv_gid gid;
} __attribute__((packed));

class RDMAStack;
class RDMAWorker;

class Infiniband {
  private:
    static Infiniband *ib;
  public:
    Infiniband() {}
    explicit Infiniband(RDMAStack* s, CephContext *cct, const char* device_name)
      : stack(s), cct(cct) {
        device = ResourceManager::get_device(device_name);
        assert(device);
        ib_physical_port = device->active_port->get_port_num();
        pd = new ProtectionDomain(cct, device);
        assert(set_nonblocking(device->ctxt->async_fd) == 0);

        max_recv_wr = device->device_attr->max_srq_wr;
        max_cqe = device->device_attr->max_cqe;
        max_send_wr = device->device_attr->max_qp_wr;
        max_inline_data = 128;
        max_sge = 1;

        memory_manager = new MemoryManager(cct, device, pd);
        memory_manager->register_rx_tx(8*1024, 256, 256);//
        srq = create_shared_receive_queue(max_recv_wr, max_sge);
        post_channel_cluster();
      }

    /**
     * Destroy an Infiniband object.
     */
    ~Infiniband() {
      assert(0);
      delete pd;
      delete device;
    }

    int set_nonblocking(int fd);

    static void create_infiniband(RDMAStack* stack, CephContext *cct, const char* device_name) {
      ResourceManager::refresh(cct);
      ib = new Infiniband(stack, cct, device_name);
    }
    static Infiniband* get_infiniband() {
      return ib;
    }

    class ProtectionDomain {
      public:
        explicit ProtectionDomain(CephContext *c, Device *device)
          : cct(c), pd(ibv_alloc_pd(device->ctxt))
        {
          if (pd == NULL) {
            lderr(cct) << __func__ << " failed to allocate infiniband protection domain: " << cpp_strerror(errno) << dendl;
            assert(0);
          }
        }
        ~ProtectionDomain() {
          int rc = ibv_dealloc_pd(pd);
          if (rc != 0) {
            lderr(cct) << __func__ << " ibv_dealloc_pd failed: "
              << cpp_strerror(errno) << dendl;
          }
        }
        CephContext *cct;
        ibv_pd* const pd;
    };

    class CompletionChannel {
      public:
        CompletionChannel(Infiniband &ib): infiniband(ib), channel(NULL), cq(NULL), cq_events_that_need_ack(0) {}
        ~CompletionChannel();
        int init();
        bool get_cq_event();
        int get_fd() { return channel->fd; }
        ibv_comp_channel* get_channel() { return channel; }
        void bind_cq(ibv_cq *c) { cq = c; }
        void ack_events() {
          ibv_ack_cq_events(cq, cq_events_that_need_ack);
          cq_events_that_need_ack = 0;
        }

      private:
        static const uint32_t MAX_ACK_EVENT = 128;
        Infiniband& infiniband;
        ibv_comp_channel *channel;
        ibv_cq *cq;
        uint32_t cq_events_that_need_ack;
    };
    // this class encapsulates the creation, use, and destruction of an RC
    // completion queue.
    //
    // You need to call init and it will create a cq and associate to comp channel
    class CompletionQueue {
      public:
        CompletionQueue(Infiniband &ib, const uint32_t qd, CompletionChannel *cc):infiniband(ib), channel(cc), cq(NULL), queue_depth(qd) {}
        ~CompletionQueue();
        int init();
        int poll_cq(int num_entries, ibv_wc *ret_wc_array);

        ibv_cq* get_cq() const { return cq; }
        int rearm_notify(bool solicited_only=true);
        CompletionChannel* get_cc() const { return channel; }
      private:
        Infiniband&  infiniband;     // Infiniband to which this QP belongs
        CompletionChannel *channel;
        ibv_cq           *cq;
        uint32_t      queue_depth;
    };

    // this class encapsulates the creation, use, and destruction of an RC
    // queue pair.
    //
    // you need call init and it will create a qp and bring it to the INIT state.
    // after obtaining the lid, qpn, and psn of a remote queue pair, one
    // must call plumb() to bring the queue pair to the RTS state.
    class QueuePair {
      public:
        QueuePair(Infiniband& infiniband, ibv_qp_type type,int ib_physical_port,  ibv_srq *srq, Infiniband::CompletionQueue* txcq, Infiniband::CompletionQueue* rxcq, uint32_t max_send_wr, uint32_t max_recv_wr, uint32_t q_key = 0);
        // exists solely as superclass constructor for MockQueuePair derivative
        explicit QueuePair(Infiniband& infiniband):
          infiniband(infiniband), type(IBV_QPT_RC), ctxt(NULL), ib_physical_port(-1),
          pd(NULL), srq(NULL), qp(NULL), txcq(NULL), rxcq(NULL),
          initial_psn(-1) {}
        ~QueuePair();

        int init();

        /**
         * Get the initial packet sequence number for this QueuePair.
         * This is randomly generated on creation. It should not be confused
         * with the remote side's PSN, which is set in #plumb(). 
         */
        uint32_t get_initial_psn() const { return initial_psn; };
        /**
         * Get the local queue pair number for this QueuePair.
         * QPNs are analogous to UDP/TCP port numbers.
         */
        uint32_t get_local_qp_number() const { return qp->qp_num; };
        /**
         * Get the remote queue pair number for this QueuePair, as set in #plumb().
         * QPNs are analogous to UDP/TCP port numbers.
         */
        int get_remote_qp_number(uint32_t *rqp) const {
          ibv_qp_attr qpa;
          ibv_qp_init_attr qpia;

          int r = ibv_query_qp(qp, &qpa, IBV_QP_DEST_QPN, &qpia);
          if (r) {
            lderr(infiniband.cct) << __func__ << " failed to query qp: "
              << cpp_strerror(errno) << dendl;
            return -1;
          }

          if (rqp)
            *rqp = qpa.dest_qp_num;
          return 0;
        }
        /**
         * Get the remote infiniband address for this QueuePair, as set in #plumb().
         * LIDs are "local IDs" in infiniband terminology. They are short, locally
         * routable addresses.
         */
        int get_remote_lid(uint16_t *lid) const {
          ibv_qp_attr qpa;
          ibv_qp_init_attr qpia;

          int r = ibv_query_qp(qp, &qpa, IBV_QP_AV, &qpia);
          if (r) {
            lderr(infiniband.cct) << __func__ << " failed to query qp: "
              << cpp_strerror(errno) << dendl;
            return -1;
          }

          if (lid)
            *lid = qpa.ah_attr.dlid;
          return 0;
        }
        /**
         * Get the state of a QueuePair.
         */
        int get_state() const {
          ibv_qp_attr qpa;
          ibv_qp_init_attr qpia;

          int r = ibv_query_qp(qp, &qpa, IBV_QP_STATE, &qpia);
          if (r) {
            lderr(infiniband.cct) << __func__ << " failed to get state: "
              << cpp_strerror(errno) << dendl;
            return -1;
          }
          return qpa.qp_state;
        }
        /**
         * Return true if the queue pair is in an error state, false otherwise.
         */
        bool is_error() const {
          ibv_qp_attr qpa;
          ibv_qp_init_attr qpia;

          int r = ibv_query_qp(qp, &qpa, -1, &qpia);
          if (r) {
            lderr(infiniband.cct) << __func__ << " failed to get state: "
              << cpp_strerror(errno) << dendl;
            return true;
          }
          return qpa.cur_qp_state == IBV_QPS_ERR;
        }
        ibv_qp* get_qp() const { return qp; }
        Infiniband::CompletionQueue* get_tx_cq() const { return txcq; }
        Infiniband::CompletionQueue* get_rx_cq() const { return rxcq; }
        int to_reset();
        int to_dead();
        int get_fd() { return fd; }
      private:
        Infiniband&  infiniband;     // Infiniband to which this QP belongs
        ibv_qp_type  type;           // QP type (IBV_QPT_RC, etc.)
        ibv_context* ctxt;           // device context of the HCA to use
        int ib_physical_port;
        ibv_pd*      pd;             // protection domain
        ibv_srq*     srq;            // shared receive queue
        ibv_qp*      qp;             // infiniband verbs QP handle
        Infiniband::CompletionQueue* txcq;
        Infiniband::CompletionQueue* rxcq;
        uint32_t     initial_psn;    // initial packet sequence number
        uint32_t     max_send_wr;
        uint32_t     max_recv_wr;
        uint32_t     q_key;
        int fd;
    };

    class MemoryManager{
      public:
        class Chunk{
          public:
            Chunk(char* b, uint32_t len, ibv_mr* m) : buffer(b), bytes(len), offset(0), mr(m) {
            }  
            ~Chunk() {

            }

            void set_offset(uint32_t o) {
              offset = o;
            }

            size_t get_offset() {
              return offset;
            }
            size_t read(char* buf, size_t len) {
              size_t left = bytes - offset;
              if(left >= len) {
                CephContext* cct = Infiniband::get_infiniband()->cct;
                //lderr(cct) << __func__ << " go to read:" << len << ", offset: " << offset << cpp_strerror(errno) << dendl;
                memcpy(buf, buffer+offset, len);
                offset += len;
                return len;
              }
              else{
                memcpy(buf, buffer+offset, left);
                offset = 0;
                return left;
              }
              return offset;
            }

            size_t write(char* buf, size_t len) {
              size_t left = bytes - offset;
              CephContext* cct = Infiniband::get_infiniband()->cct;
          //    lderr(cct) << __func__ << " go to send:" << len << ", left: " << left << cpp_strerror(errno) << dendl;
              if(left >= len) {
                memcpy(buffer+offset, buf, len);
                offset += len;
                return len;
              }
              else{
                memcpy(buffer+offset, buf, left);
                offset = bytes;
                return left;
              }
            }

            bool full() {
              return offset == bytes;
            }
          public:
            char* buffer;
            uint32_t bytes;
            size_t offset;
            ibv_mr* mr;
        };
        class Cluster{
          public:
            Cluster(MemoryManager& m, uint32_t s) : manager(m), chunk_size(s){}
            Cluster(MemoryManager& m, uint32_t s, uint32_t n) : manager(m), chunk_size(s){
              add(n);  
            }

            int add(uint32_t num) {
              uint32_t bytes = chunk_size*num;
              char* base = (char*)malloc(bytes);
              assert(base);
              for(uint32_t offset = 0;offset<bytes;offset+=chunk_size){
                ibv_mr* m = ibv_reg_mr(manager.pd->pd, base+offset, chunk_size, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
                assert(m);
                free_chunks.push_back(new Chunk(base+offset,chunk_size,m));
              }
              return 0;
            }

            int take_back(Chunk* ck){
              free_chunks.push_back(ck);
              return 0;
            }

            vector<Chunk*> get_buffers(size_t bytes) {
              if(bytes == 0)
                return free_chunks;
              vector<Chunk*> ret;
              uint32_t num = bytes / chunk_size + 1;
              if(bytes % chunk_size == 0)
                --num;
              if(free_chunks.size() < num) {
                return ret;	
              }
              else {
                while(ret.size() != num) {
                  ret.push_back(free_chunks.back());
                  free_chunks.pop_back();
                }
                return ret;
              }
            }

            MemoryManager& manager;
            uint32_t chunk_size;
            vector<Chunk*> free_chunks;
        };
        MemoryManager(CephContext *cct, Device *d, ProtectionDomain *p) : cct(cct), device(d), pd(p){}
        int register_rx_tx(uint32_t size,uint32_t rx_num,uint32_t tx_num) {
          assert(device);
          assert(pd);
          channel = new Cluster(*this,size);
          if(channel != NULL){

          }
          channel->add(rx_num);

          send = new Cluster(*this,size);
          if(rdma != NULL){

          }
          send->add(tx_num);
        }
        int register_rdma(uint32_t size, uint32_t num) {
          assert(device);
          assert(pd);

          rdma = new Cluster(*this,size,num);
          if(rdma != NULL)
            return 0;
          else return -1;
        }

        int return_channel(Chunk* c) {
          return send->take_back(c);
        }

        int return_rx(Chunk* c) {
          return channel->take_back(c);
        }

        int return_rdma(Chunk* c) {
          return rdma->take_back(c);
        }

        vector<Chunk*> get_channel_buffers(size_t bytes) {
          return channel->get_buffers(bytes);
        }
      private:
        Cluster* channel;//RECV
        Cluster* rdma;// RDMA READ & WRITE
        Cluster* send;// SEND
        CephContext *cct;
        Device *device;
        ProtectionDomain *pd;
    };
  private:		
    uint32_t max_rx_buffers;
    uint32_t max_tx_buffers;
    uint32_t max_send_wr;
    uint32_t max_recv_wr;
    uint32_t max_cqe;
    uint32_t max_sge;
    uint32_t max_inline_data;
    uint8_t  ib_physical_port;
    MemoryManager* memory_manager;
    ibv_srq*         srq;             // shared receive work queue
    Device *device;
    ProtectionDomain *pd;
    RDMAStack* stack;
    CephContext* cct;
    void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
    void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

  public:
    typedef MemoryManager::Cluster Cluster;
    typedef MemoryManager::Chunk Chunk;
    QueuePair* create_queue_pair(ibv_qp_type type);
    ibv_srq* create_shared_receive_queue(uint32_t max_wr, uint32_t max_sge);
    int post_chunk(Chunk* chunk);
    int post_channel_cluster();
    vector<Chunk*> get_tx_buffers(size_t bytes) {
      return memory_manager->get_channel_buffers(bytes);
    }
    CompletionChannel *create_comp_channel();
    CompletionQueue *create_comp_queue(CompletionChannel *cc=NULL);
    uint8_t get_ib_physical_port() {
      return ib_physical_port;  
    }
    int send_udp_msg(int sd, IBSYNMsg& msg, entity_addr_t &peeraddr);
    int recv_udp_msg(int sd, IBSYNMsg& msg, entity_addr_t *addr);
    uint16_t get_lid() { return device->get_lid(); }
    ibv_gid get_gid() { return device->get_gid(); }
    MemoryManager* get_memory_manager() { return memory_manager; }
};

#endif

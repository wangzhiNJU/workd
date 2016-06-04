#include "rdma_socket.c"
class rdma_portal
{
private:
	int con_num;//
	int sockfd;
public:
	rdma_portal(int cn) : con_num(cn),sockfd(-1) {}
	rdma_session* accept(int port,struct init_params *params);
	int get_sockfd() { return sockfd;}
};

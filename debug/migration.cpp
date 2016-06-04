#include "rdma_session.cpp"
#include "rdma_portal.cpp"

//#define RW_SIZE 1048576
#define RW_SIZE 1073741824

struct rdma_session* connect(const char* url);
int collect(char* pool);
void log(const char* str);

int main(int argc,char* argv[]) {

	const char* url = "192.168.1.3";
	struct rdma_session* session = connect(url);
	
	char data[20];
	int fd = session->ctx->fd;
	while(true) {
		int size = collect(session->get_rw_buf());
		if(size == 0) break;
		else if(size == -1) {
			log(" collect error ");
			break;
		}
		sprintf(data,"%d",size);
		write(fd,data,strlen(data)+1);
		read(fd,data,10);
		if(strcmp(data,"done") != 0) {
			log("got peer error, received:");
			log(data);
			break;
		}
	}
	write(fd,"0",2);
	session->close();
	return 0;
}

struct rdma_session* connect(const char* url) {
        struct rdma_session* rs;
        struct init_params params = {};
        params.mem_size = RW_SIZE;
	params.ib_devname = rs->roce_name();
        params.gidx = 0;
        srand48(getpid() * time(NULL));

        rs = new rdma_session(url,18185,&params);
        if(rs == NULL) {
                log(" null session");
        }
        return rs;
}

int collect(char* pool) {
	/*int index = 0,n;
	int b_size = 4096;
	for(int i=0;i<256;++i) {
		if((n = read(STDIN_FILENO,pool+index,b_size))>0) {
			index += n;
		}
		else if(n == 0) {
			break;
		}
		else {
			return -1;
		}
	}*/
	int ret = 0,tmp = 0;
	while(true) {
		if((tmp = read(STDIN_FILENO,pool+ret,RW_SIZE-ret))>0) {
			ret += tmp;
		}
		else if(tmp == 0) {
			break;
		}
		else {
			return -1;
		}
	}
	/* version 2
	int ret = -1;
	if((ret = read(STDIN_FILENO,pool,RW_SIZE))<0) {
		log("\n read error!\n");
	}*/
	return ret;
}

void log(const char* str) {
	printf("%s",str);
}


#include <hdRDMA.h>

#include <unistd.h>
#include <string.h>
#include <strings.h>

#include <iostream>
#include <atomic>
#include <chrono>
#include <fstream>

using std::cout;
using std::cerr;
using std::endl;
using namespace std::chrono;

//-------------------------------------------------------------
// hdRDMA
//
// hdRDMA constructor. This will look for IB devices and set up
// for RDMA communications on the first one it finds.
//-------------------------------------------------------------
hdRDMA::hdRDMA()
{
	cout << "Looking for IB devices ..." << endl;
	int num_devices = 0;
	struct ibv_device **devs = ibv_get_device_list( &num_devices );
	
	// List devices
	cout << endl << "=============================================" << endl;
	cout << "Found " << num_devices << " devices" << endl;
	cout << "---------------------------------------------" << endl;
	for(int i=0; i<num_devices; i++){
	
		const char *transport_type = "unknown";
		switch( devs[i]->transport_type ){
			case IBV_TRANSPORT_IB:
				transport_type = "IB";
				break;
			case IBV_TRANSPORT_IWARP:
				transport_type = "IWARP";
				break;
			case IBV_EXP_TRANSPORT_SCIF:
				transport_type = "SCIF";
				break;
		}
		
		// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
		// Here we want to check the lid of each device but to do so we 
		// must open the device and get the port attributes. We need to
		// do this to determine which device is actually connected to the
		// IB network since only connected ones will have lid!=0.
		// We remember the last device in the list with a non-zero lid
		// and use that.
		uint64_t lid = 0;

		// Open device
		ctx = ibv_open_device(devs[i]);
		int Nports = 0;
		if( ctx ){
			// Loop over port numbers
			for( uint8_t port_num = 1; port_num<10; port_num++) { // (won't be more than 2!)
				struct ibv_port_attr my_port_attr;
				auto ret = ibv_query_port( ctx, port_num, &my_port_attr);
				if( ret != 0 ) break;
				Nports++;
				if( my_port_attr.lid != 0){
					lid = my_port_attr.lid;
					dev = devs[i];
					this->port_num = port_num;
				}
			}
			ibv_close_device( ctx );
		}
		// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	
		cout << "   device " << i
			<< " : " << devs[i]->name
			<< " : " << devs[i]->dev_name
			<< " : " << transport_type
			<< " : " << ibv_node_type_str(devs[i]->node_type)
			<< " : Num. ports=" << Nports
			<< " : port num=" << port_num
//			<< " : GUID=" << ibv_get_device_guid(devs[i])
			<< " : lid=" << lid
			<< endl;
	}
	cout << "=============================================" << endl << endl;
	
	// Open device
	ctx = ibv_open_device(dev);
	if( !ctx ){
		cout << "Error opening IB device context!" << endl;
		exit(-11);
	}
	
	// Get device and port attributes
	int index = 0;
	ibv_gid gid;
	ibv_query_device( ctx, &attr);
	ibv_query_port( ctx, port_num, &port_attr);
	ibv_query_gid(ctx, port_num, index, &gid);

	cout << "Device " << dev->name << " opened."
		<< " num_comp_vectors=" << ctx->num_comp_vectors
		<< endl;

	// Print some of the port attributes
	cout << "Port attributes:" << endl;
	cout << "           state: " << port_attr.state << endl;
	cout << "         max_mtu: " << port_attr.max_mtu << endl;
	cout << "      active_mtu: " << port_attr.active_mtu << endl;
	cout << "  port_cap_flags: " << port_attr.port_cap_flags << endl;
	cout << "      max_msg_sz: " << port_attr.max_msg_sz << endl;
	cout << "    active_width: " << (uint64_t)port_attr.active_width << endl;
	cout << "    active_speed: " << (uint64_t)port_attr.active_speed << endl;
	cout << "      phys_state: " << (uint64_t)port_attr.phys_state << endl;
	cout << "      link_layer: " << (uint64_t)port_attr.link_layer << endl;

	// Allocate protection domain
	pd = ibv_alloc_pd(ctx);
	if( !pd ){
		cout << "ERROR allocation protection domain!" << endl;
		exit(-12);
	}
	
	// Allocate a 4GB buffer and create a memory region pointing to it.
	// We will split this one memory region among multiple receive requests
	// n.b. initial tests failed on transfer for buffers larger than 1GB
	uint64_t buff_len_GB = 8;
	num_buff_sections = 16;
	buff_section_len = (buff_len_GB*1000000000)/(uint64_t)num_buff_sections;
	buff_len = num_buff_sections*buff_section_len;
	buff = new uint8_t[buff_len];
	if( !buff ){
		cout << "ERROR: Unable to allocate buffer!" << endl;
		exit(-13);
	}
	errno = 0;
	auto access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
	mr = ibv_reg_mr( pd, buff, buff_len, access);
	if( !mr ){
		cout << "ERROR: Unable to register memory region! errno=" << errno << endl;
		exit( -14 );
	}
	
	// Fill in buffers
	for( uint32_t i=0; i<num_buff_sections; i++){
		auto b = &buff[ i*buff_section_len ];
		hdRDMAThread::bufferinfo bi = std::make_tuple( b, buff_section_len );
		buffer_pool.push_back( bi );
	}
	cout << "Created " << buffer_pool.size() << " buffers of " << buff_section_len/1000000 << "MB (" << buff_len/1000000000 << "GB total)" << endl;
}

//-------------------------------------------------------------
// ~hdRDMA
//-------------------------------------------------------------
hdRDMA::~hdRDMA()
{
	// Stop all connection threads
	for( auto t : threads ){
		t.second->stop = true;
		t.first->join();
		delete t.second;
	}

	// Close and free everything
	if(           mr!=nullptr ) ibv_dereg_mr( mr );
	if(         buff!=nullptr ) delete[] buff;
	if(           pd!=nullptr ) ibv_dealloc_pd( pd );
	if(          ctx!=nullptr ) ibv_close_device( ctx );

	if( server_sockfd ) shutdown( server_sockfd, SHUT_RDWR );
}

//-------------------------------------------------------------
// Listen
//
// Set up server and listen for connections from remote hosts
// wanting to trade RDMA connection information.
//-------------------------------------------------------------
void hdRDMA::Listen(int port)
{
	// Create socket, bind it and put it into the listening state.	
	struct sockaddr_in addr;
	bzero( &addr, sizeof(addr) );
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons( port );
	
	server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
	auto ret = bind( server_sockfd, (struct sockaddr*)&addr, sizeof(addr) );
	if( ret != 0 ){
		cout << "ERROR: binding server socket!" << endl;
		exit(-2);
	}
	listen(server_sockfd, 5);
	
	// Create separate thread to accept socket connections so we don't block
	std::atomic<bool> thread_started(false);
	server_thread = new std::thread([&](){
		
		// Loop forever accepting connections
		cout << "Listening for connections on port ... " << port << endl;
		thread_started = true;
		
		while( !done ){
			int peer_sockfd   = 0;
			struct sockaddr_in peer_addr;
			socklen_t peer_addr_len = sizeof(struct sockaddr_in);
			peer_sockfd = accept(server_sockfd, (struct sockaddr *)&peer_addr, &peer_addr_len);
			if( peer_sockfd > 0 ){
				cout << "Connected!" <<endl;
				cout << "Connection from " << inet_ntoa(peer_addr.sin_addr) << endl;
				
				// Create a new thread to handle this connection
				auto hdthr = new hdRDMAThread( this );
				auto thr = new std::thread( &hdRDMAThread::ThreadRun, hdthr, peer_sockfd );
				threads[ thr ] = hdthr;
				Nconnections++;

			}else{
				cout << "Failed connection!" <<endl;
				break;
			}
		} // !done
	
	}); 
	
	// Wait for thread to start up so it's listening message gets printed
	// before rest of program continues.
	while(!thread_started) std::this_thread::yield();
	
}

//-------------------------------------------------------------
// StopListening
//-------------------------------------------------------------
void hdRDMA::StopListening(void)
{
	if( server_thread ){
		cout << "Waiting for server to finish ..." << endl;
		done = true;
		server_thread->join();
		delete server_thread;
		server_thread = nullptr;
		if( server_sockfd ) close( server_sockfd );
		server_sockfd = 0;
	}else{
		cout << "Server not running." <<endl;
	}
}

//-------------------------------------------------------------
// Connect
//-------------------------------------------------------------
void hdRDMA::Connect(std::string host, int port)
{
	// Get IP address based on server hostname
	struct addrinfo hints;
	struct addrinfo *result;
	char addrstr[100];
	void *ptr;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;
	hints.ai_flags |= AI_CANONNAME;

	auto ret = getaddrinfo(host.c_str(), NULL, &hints, &result);
	while( result ){
		inet_ntop( result->ai_family, result->ai_addr->sa_data, addrstr, 100);
		switch( result->ai_family ){
			case AF_INET:
				ptr = &((struct sockaddr_in *)result->ai_addr)->sin_addr;
				break;
			case AF_INET6:
				ptr = &((struct sockaddr_in6 *)result->ai_addr)->sin6_addr;
				break;
		}
		inet_ntop( result->ai_family, ptr, addrstr, 100 );
		
		cout << "IP address: " << addrstr << " (" << result->ai_canonname << ")" << endl;
		
		result = result->ai_next;
	}
	

	// Create socket and connect it to remote host	
	struct sockaddr_in addr;
	bzero( &addr, sizeof(addr) );
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr( addrstr );
	addr.sin_port = htons( port );
	
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	ret = connect( sockfd, (struct sockaddr*)&addr, sizeof(addr) );
	if( ret != 0 ){
		cout << "ERROR: connecting to server: " << host << " (" << inet_ntoa(addr.sin_addr) << ")" << endl;
		exit(-3);
	}else{
		cout << "Connected to " << host << ":" << port << endl;
	}
	
	// Create an hdRDMAThread object to handle the RDMA connection details.
	// (we won't actually run it in a separate thread.)
	hdthr_client = new hdRDMAThread( this );
	hdthr_client->ClientConnect( sockfd );
	
}


//-------------------------------------------------------------
// GetNpeers
//-------------------------------------------------------------
uint32_t hdRDMA::GetNpeers(void)
{
	return threads.size();
}

//-------------------------------------------------------------
// GetBuffers
//-------------------------------------------------------------
void hdRDMA::GetBuffers( std::vector<hdRDMAThread::bufferinfo> &buffers, int Nrequested )
{
	std::lock_guard<std::mutex> grd( buffer_pool_mutex );

cout << "buffer_pool.size()="<<buffer_pool.size() << "  Nrequested=" << Nrequested << endl;
	
	for( int i=buffers.size(); i<Nrequested; i++){
		if( buffer_pool.empty() ) break;
		buffers.push_back( buffer_pool.back() );
		buffer_pool.pop_back();
	}
}

//-------------------------------------------------------------
// ReturnBuffers
//-------------------------------------------------------------
void hdRDMA::ReturnBuffers( std::vector<hdRDMAThread::bufferinfo> &buffers )
{
	std::lock_guard<std::mutex> grd( buffer_pool_mutex );

	for( auto b : buffers ) buffer_pool.push_back( b );
}

//-------------------------------------------------------------
// SendFile
//-------------------------------------------------------------
void hdRDMA::SendFile(std::string srcfilename, std::string dstfilename)
{
	// This just calls the SendFile method of the client hdRDMAThread

	if( hdthr_client == nullptr ){
		cerr << "ERROR: hdRDMA::SendFile called before hdthr_client instantiated." << endl;
		return;
	}
	
	hdthr_client->SendFile( srcfilename, dstfilename );

}

//-------------------------------------------------------------
// Poll
//
// Check for closed connections and release their resources.
// This is called periodically from main().
//-------------------------------------------------------------
void hdRDMA::Poll(void)
{
	// Look for stopped threads and free their resources
	for( auto t : threads ){
		if( t.second->stopped ){
			t.first->join();
			delete t.second;
			threads.erase( t.first );
		}
	}

}


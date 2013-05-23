#include "BitTorrentProtocol.h"

using namespace network::bittorrent;

const char BitTorrentProtocol::question_mark = '?';
const int BitTorrentProtocol::active_thread = 0;
const int BitTorrentProtocol::finish_thread = -1;

BitTorrentProtocol::BitTorrentProtocol(network::bittorrent::io::notifiable* subscriber, 
	network::bittorrent::file_parser file, std::wstring folder) : torrentFile(file){


	download_folder = folder;
    _repeated = false;
    _is_interrupted = false;
	_active_threads = 0;
	_subscriber = subscriber;

	encryption::random_string_generator generator;
	key = generator.ascii_string(8);

	std::vector<char> appendix = generator.random_string(12);
	peer_id = "-UT3300-";
	raw_peer_id.insert(raw_peer_id.end(), peer_id.begin(), peer_id.end());
	raw_peer_id.insert(raw_peer_id.end(), appendix.begin(), appendix.end());

	peer_id += encryption::percent_encoding::encode(appendix);
}

BitTorrentProtocol::~BitTorrentProtocol(){
    for(std::list<network::bittorrent::io::file*>::iterator file_ptr = finalFiles.begin();
        file_ptr != finalFiles.end(); file_ptr++){
        delete (*file_ptr);
    }

    for(std::list<network::http::protocol*>::iterator http_ptr = httpList.begin();
        http_ptr != httpList.end(); http_ptr++){
        delete (*http_ptr);
    }

    delete trackerRequest;
    delete peerConversation;
    delete loadAdapter;
	delete trackerResponse;
    delete pieceControl;
}

std::wstring BitTorrentProtocol::folder(){
    return download_folder;
}

bool BitTorrentProtocol::alive(){
    return !_is_interrupted;
}

void BitTorrentProtocol::yeild(){
	trackerRequest = new boost::thread(boost::bind(&BitTorrentProtocol::announce_request, this));
	trackerRequest->yield();
}

void BitTorrentProtocol::interrupt(){
    for(std::list<network::bittorrent::peer_wire::protocol*>::iterator peer_ptr = peerWireProtocolList.begin();
        peer_ptr != peerWireProtocolList.end(); peer_ptr++){
        (*peer_ptr)->interrupt();
    }

    _is_interrupted = true;
    std::cout << "INTERRUPTED" << std::endl;
}

void BitTorrentProtocol::execute_downloading(){
	std::cout << "EXECUTE" << std::endl;
	std::cout << "PEER " << peerList.size() << std::endl;

	if(peerList.size() == 0){
		return;
	}

	pieceControl = new network::bittorrent::piece::control(torrentFile.pieces(), torrentFile.piece_length());

	for(std::list<network::bittorrent::peer_wire::peer>::iterator it = peerList.begin();
		it != peerList.end(); it++){


		network::bittorrent::peer_wire::protocol* peerWireProtocol;

		try{
            peerWireProtocol = new network::bittorrent::peer_wire::protocol(this, (*it), pieceControl, loadAdapter, info_hash, raw_peer_id);
		}catch(std::exception e){
			continue;
		}

		peerWireProtocolList.push_back(peerWireProtocol);
		peerWireProtocol->yeild();

	}

	_active_threads = peerWireProtocolList.size();
}


void BitTorrentProtocol::resume_downloading(){
    std::cout << "EXECUTE" << std::endl;
    std::cout << "PEER " << peerList.size() << std::endl;

    if(peerList.size() == 0){
        return;
    }

    peerWireProtocolList.clear();

    for(std::list<network::bittorrent::peer_wire::peer>::iterator it = peerList.begin();
        it != peerList.end(); it++){

        network::bittorrent::peer_wire::protocol* peerWireProtocol;

        try{
            peerWireProtocol = new network::bittorrent::peer_wire::protocol(this, (*it), pieceControl, loadAdapter, info_hash, raw_peer_id);
        }catch(std::exception e){
            continue;
        }

        peerWireProtocolList.push_back(peerWireProtocol);
        peerWireProtocol->yeild();

    }

    _active_threads = peerWireProtocolList.size();
}


void BitTorrentProtocol::game_over(){
    _active_threads -= 1;

    if(_active_threads == 0 && pieceControl->left()){
        if(!_repeated){
            _repeated = true;
            restart();
        }
    }
}

void BitTorrentProtocol::response_handle(std::istream& response){
	std::cout << "response handle" << std::endl;
	std::string item;
	responseData.clear();

	threadStack.pop();

	network::http::parser hparser(response);										//parse to HTTP response format
	hparser.parse();

	responseData = hparser.http_data();		

	if(hparser.bad()){																//normal HTTP response status: {"HTTP 1.1", "200", "OK"}
		std::cout << "HTTP BAD" << std::endl;
		return;
	}

	bencode::provider::vector provider(responseData);								//tracker response parse
	bencode::parser parser(provider);
	bencode::element* node;

	try{
		node = parser.node();
		trackerResponse = new network::bittorrent::tracker_response_parser(node);	//tracker response wrapper

		if(trackerResponse->bad()){													//if tracker responce with errors
			throw std::exception();
		}

		std::vector<char> peers = trackerResponse->binary_peers();					//get peers in binary-style
		network::bittorrent::peer_wire::parser peerParser(peers);					//get 'normal' ipv4 addresses
		std::list<network::bittorrent::peer_wire::peer> additionalPeers = peerParser.peers();
		peerList.insert(peerList.end(), additionalPeers.begin(), additionalPeers.end());

	}catch(std::exception e){
		std::cout << e.what();
	}

	std::cout << "peer list " << peerList.size() << std::endl;
	std::cout << "active " << threadStack.size() << std::endl;

	int nextAction = threadStack.top();

	if(nextAction == BitTorrentProtocol::finish_thread){
		peerConversation = new boost::thread(boost::bind(&BitTorrentProtocol::execute_downloading, this));
		peerConversation->yield();
	}
}

void BitTorrentProtocol::restart(){
    peerConversation = new boost::thread(boost::bind(&BitTorrentProtocol::resume_downloading, this));
    peerConversation->yield();

    _is_interrupted = false;
}


void BitTorrentProtocol::announce_request(){
	std::list<std::string> announce = torrentFile.announce_list();
	std::string httpDomain;
	std::string httpUri;
	std::string httpRequest;

	std::cout << "creating files..." << std::endl;
	fileList = torrentFile.files();
	
	for(std::list<DownloadFile>::iterator it = fileList.begin(); it != fileList.end(); it++){
		finalFiles.push_back(new network::bittorrent::io::file(
			download_folder + it->path, it->length, torrentFile.piece_length()));
	}
	
	loadAdapter = new network::bittorrent::io::load_adapter(_subscriber, finalFiles, torrentFile.piece_length());

	threadStack.push(finish_thread);
	
	for(unsigned int i = 0; i < torrentFile.announce_size(); i++){
		httpList.push_back(new network::http::protocol(this));
		threadStack.push(active_thread);
	}

	std::cout << "STACK SIZE " << threadStack.size() << std::endl;

	std::list<network::http::protocol*>::iterator httpItem = httpList.begin();
	info_hash = torrentFile.info_hash();

	std::string percent_info_hash = encryption::percent_encoding::encode(info_hash);

	for(std::list<std::string>::iterator it = announce.begin(); it != announce.end(); it++, httpItem++){
		std::string httpAddress = *it;
		network::http::protocol* http = *httpItem;

		std::stringstream request;

		httpDomain = network::http::address_parser::domain(httpAddress);
		httpUri = network::http::address_parser::uri(httpAddress);
		
		request << "/" << httpUri;

		if(httpUri.find(question_mark) != std::string::npos){
			request << "&info_hash=";
		}else{
			request << "?info_hash=";
		}

		request << percent_info_hash
			<< "&peer_id=" << peer_id
			<< "&port=" << "6881"
			<< "&uploaded=0"
			<< "&downloaded=0"
			<< "&left=" << torrentFile.size()
			<< "&corrupt=0"
			<< "&key=" << key
			<< "&event=started"
			<< "&numwant=30"
			<< "&compact=1"
			<< "&no_peer_id=1";

		std::cout << request.str() << std::endl << std::endl;

		httpRequest = request.str();
		http->get(httpDomain, httpRequest);
	}
}

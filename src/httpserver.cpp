#include "httpserver.h"

HttpServer* HttpServer::ptr_server = nullptr;
HttpServer::HttpServer(uint32_t listenEvent, uint32_t connEvent, int epollTimeoutMilli) 
	:_ssock(INVALID_SOCKET)
	,_ssin({})
	, _listenEvent(listenEvent)
	, _connEvent(connEvent), _epollTimeout(epollTimeoutMilli)
	, _wheelTimer(new WheelTimer(60))
	, _epollManager(new EpollManager())
	, _threadManager(new ThreadManager(20))
{
	HttpServer::ptr_server = this;
	_running = true;

}

HttpServer::~HttpServer()
{
	_running = false;
	if (INVALID_SOCKET != _ssock)
	{
		close(_ssock);
		_ssock = INVALID_SOCKET;
	}
}



int HttpServer::initSocket(int port, std::string addr)
{
	_ssock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	signal(SIGPIPE, SIG_IGN);
	if (INVALID_SOCKET == _ssock)
	{
		LOG_ERROR("生成服务器套接字失败");
		_running = false;
		return SERVER_ERROR;
	}
	if (port > 65535) {
		LOG_ERROR("端口不合法");
		_running = false;
		return SERVER_ERROR;
	}
	_ssin.sin_family = AF_INET;
	_ssin.sin_port = htons(port);
	if (addr == "") _ssin.sin_addr.s_addr = htonl(INADDR_ANY);
	else _ssin.sin_addr.s_addr = inet_addr(addr.c_str());

	//struct linger optLinger = { 0 };
	//if (openLinger_) {
	//	/* 优雅关闭: 直到所剩数据发送完毕或超时 */
	//	optLinger.l_onoff = 1;
	//	optLinger.l_linger = 1;
	//}
	//res = setsockopt(_ssock, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
	//if (ret < 0) {
	//	close(listenFd_);
	//	LOG_ERROR("Init linger error!", port_);
	//	return false;
	//}


	int res = bind(_ssock, reinterpret_cast<sockaddr*>(&_ssin), sizeof(_ssin));
	if (SOCKET_ERROR == res)
	{

		close(_ssock);
		_ssock = INVALID_SOCKET;
		LOG_ERROR("绑定端口失败");
		_running = false;
		return SERVER_ERROR;
	}

	res = listen(_ssock, 5);
	if (SOCKET_ERROR == res)
	{
		close(_ssock);
		_ssock = INVALID_SOCKET;
		LOG_ERROR("监听端口失败");
		_running = false;
		return SERVER_ERROR;
	}

	res = _epollManager->addFd(_ssock, _listenEvent | EPOLLIN | EPOLLRDHUP);
	if (!res)
	{
		close(_ssock);
		_ssock = INVALID_SOCKET;
		LOG_ERROR("添加Epoll监听事件失败");
		_running = false;
		return SERVER_ERROR;
	}
	setNonblock(_ssock);
	LOG_INFO("初始化服务器完成");

	return SERVER_SUCCESS;
}

void HttpServer::alarm_handler(int sig)
{
	if (sig == SIGALRM)
	{
		if (HttpServer::ptr_server != nullptr)
		{
			HttpServer::ptr_server->_wheelTimer->tick();
		}
		alarm(1);
	}

}

void HttpServer::onRun()
{
	LOG_INFO("服务器开始工作");
	signal(SIGALRM, HttpServer::alarm_handler);
	alarm(1);
	while (_running)
	{
		int eventCount = _epollManager->wait(_epollTimeout);
		for (int i = 0; i < eventCount; i++)
		{
			int fd = _epollManager->getEventFd(i);
			auto event = _epollManager->getEvents(i);
			if (fd == _ssock) acceptClient();
			else if (event & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) closeClient(fd);
			else if (event & EPOLLIN) onRead(fd);
			else if (event & EPOLLOUT) onWrite(fd);
			else LOG_ERROR("服务器未知错误，epoll event未定义");
		}
	}

}




int HttpServer::setNonblock(int fd)
{
	if (fd < 0)return SERVER_ERROR;
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}


void HttpServer::acceptClient()
{
	SOCKET csock = INVALID_SOCKET;
	sockaddr_in csin = {};
	socklen_t len = sizeof(csin);
	while (_listenEvent & EPOLLET)
	{
		csock = accept(_ssock, reinterpret_cast<sockaddr*>(&csin), &len);
		if (INVALID_SOCKET == csock)
		{
			//LOG_ERROR("接收到无效socket");
			return;
		}
		else if (clients.size() >= maxClient)
		{
			LOG_WARNING("客户数已达上限，服务器繁忙");
			return;
		}
		else
		{
			LOG_INFO("新客户端连接");
			_epollManager->addFd(csock, EPOLLIN | _connEvent);
			setNonblock(csock);
			_wheelTimer->addTimer(new timer_struct(csock, std::bind(&HttpServer::closeClient, this, csock)));
			clients[csock] = std::shared_ptr<CLIENT>(std::make_shared<CLIENT>(_ssock, csock, csin, csock));
		}
	}
}

void HttpServer::closeClient(SOCKET fd)
{
	if (fd < 0 || clients[fd] == nullptr)return;
	LOG_INFO("客户退出");
	_epollManager->deleteFd(fd);
	clients[fd] = nullptr;
}

void HttpServer::onRead(SOCKET fd)
{
	auto c = clients[fd];
	if (c == nullptr)
	{
		LOG_ERROR("来自无效客户端的请求");
		return;
	}
	_wheelTimer->addTimer(new timer_struct(fd, std::bind(&HttpServer::closeClient, this, fd)));
	_threadManager->addTask(std::bind(&HttpServer::handleRequest, this, c));

}

void HttpServer::onWrite(SOCKET fd)
{
	auto c = clients[fd];
	if (c == nullptr)
	{
		LOG_ERROR("找不到可写对象客户端");
		return;
	}
	_threadManager->addTask(std::bind(&HttpServer::handleResponse, this, c));

}


void HttpServer::handleRequest(std::shared_ptr<CLIENT> client)
{
	if (client == nullptr)return;
	auto res = client->read();
	bool status = std::get<0>(res);
	int statCode = std::get<1>(res);
	if (status == false && statCode != EAGAIN)//客户退出
	{
		closeClient(client->getSock());
		return;
	}
	else if (status == true) // 处理请求
	{
		REQUEST_TYPE requestType = client->process_request();
		if (NO_REQUEST == requestType)
		{
			_epollManager->modFd(client->getSock(), _connEvent | EPOLLIN);
			return;
		}
		bool res = client->process_response(requestType);
		if (!res) closeClient(client->getSock());
		else _epollManager->modFd(client->getSock(), _connEvent | EPOLLOUT);
	}

}

void HttpServer::handleResponse(std::shared_ptr<CLIENT> client)
{
	if (client == nullptr)return;
	auto res = client->write();
	if (!std::get<0>(res)) closeClient(client->getSock());
	_epollManager->modFd(client->getSock(), _connEvent | std::get<1>(res));
}



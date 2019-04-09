#include "id_component_querier.h"
#include "helper/sys_info.h"
#include "common.h"
#include "../../server/machine/machine_interface.h"

namespace KBEngine
{

IDComponentQuerier::IDComponentQuerier():
	Bundle(NULL, Network::PROTOCOL_UDP),
	recvWindowSize_(PACKET_MAX_SIZE_UDP),
	itry_(5),
	good_(false)
{
	epListen_.socket(SOCK_DGRAM);
	epBroadcast_.socket(SOCK_DGRAM);

	if (!epListen_.good() || !epBroadcast_.good())
	{
		ERROR_MSG(fmt::format("IDComponentQuerier::BundleBroadcast: Socket initialization failed! {}\n",
			kbe_strerror()));
	}
	else
	{
		int count = 0;

		while (true)
		{
			srand(KBEngine::getSystemTime());
			uint16 bindPort = KBE_PORT_START * 2 + (rand() % 1000);

			if (epListen_.bind(htons(bindPort), htonl(INADDR_ANY)) != 0)
			{
				good_ = false;
				KBEngine::sleep(10);
				count++;

				if (count > 30)
				{
					ERROR_MSG(fmt::format("IDComponentQuerier::IDComponentQuerier: Cannot bind to port %d, %s\n",
						bindPort, kbe_strerror()));

					break;
				}
			}
			else
			{
				epListen_.addr(htons(bindPort), htonl(INADDR_ANY));
				good_ = true;
				DEBUG_MSG(fmt::format("IDComponentQuerier::IDComponentQuerier: epListen {}\n", epListen_.c_str()));
				break;
			}
		}
	}

	pCurrPacket()->data_resize(recvWindowSize_);
}

IDComponentQuerier::~IDComponentQuerier()
{
	close();
}

void IDComponentQuerier::close()
{
	epListen_.close();
	epBroadcast_.close();
}

COMPONENT_ID IDComponentQuerier::query(COMPONENT_TYPE componentType, int32 uid)
{
	COMPONENT_ID cid = 0;

	send(componentType, uid);

	int32 timeout = 2000000;
	MachineInterface::queryComponentIDArgs5 args;

	while (!receive(&args, 0, timeout, false))
	{
		send(componentType, uid, true);
	}

	bool hasContinue = false;
	do
	{
		if (hasContinue)
		{
			try
			{
				args.createFromStream(*pCurrPacket());
			}
			catch (MemoryStreamException &)
			{
				break;
			}
		}

		hasContinue = true;

		// 如果是未知类型则继续一次
		if (args.componentType == UNKNOWN_COMPONENT_TYPE)
			continue;

		cid = args.componentID;

	} while (pCurrPacket()->length() > 0);
	

	return cid;
}

bool IDComponentQuerier::broadcast(uint16 port /*= 0*/, bool again /* = false*/)
{
	if (!epBroadcast_.good())
		return false;

	if (port == 0)
		port = KBE_MACHINE_BROADCAST_SEND_PORT;

	epBroadcast_.addr(port, Network::LOCALHOST);

	if (epBroadcast_.setbroadcast(true) != 0)
	{
		ERROR_MSG(fmt::format("IDComponentQuerier::broadcast: Cannot broadcast socket on port {}, {}\n",
			port, kbe_strerror()));

		return false;
	}

	this->finiMessage(!again);
	KBE_ASSERT(packets().size() == 1);

	epBroadcast_.sendto(packets()[0]->data(), packets()[0]->length(), htons(port), Network::BROADCAST);
	return true;
}

bool IDComponentQuerier::receive(Network::MessageArgs* recvArgs, sockaddr_in* psin /*= NULL*/, int32 timeout /*= 100000*/, bool showerr /*= true*/)
{
	if (!epListen_.good())
		return false;

	struct timeval tv;
	fd_set fds;

	int icount = 1;
	tv.tv_sec = 0;
	tv.tv_usec = timeout;

	if (!pCurrPacket())
		newPacket();

	while (1)
	{
		FD_ZERO(&fds);
		FD_SET((int)epListen_, &fds);
		int selgot = select(epListen_ + 1, &fds, NULL, NULL, &tv);

		if (selgot == 0)
		{
			if (icount > itry_)
			{
				if (showerr)
				{
					ERROR_MSG("IDComponentQuerier::receive: failed! It can be caused by the firewall, the broadcastaddr, etc."
						"Maybe broadcastaddr is not a LAN ADDR, or the Machine process is not running.\n");
				}

				return false;
			}
				

			icount++;
			continue;
		}
		else if (selgot == -1)
		{
			if (showerr)
			{
				ERROR_MSG(fmt::format("IDComponentQuerier::receive: select error. {}.\n",
					kbe_strerror()));
			}

			return false;
		}
		else
		{
			sockaddr_in	sin;
			pCurrPacket()->resetPacket();

			if (psin == NULL)
				psin = &sin;

			pCurrPacket()->data_resize(recvWindowSize_);

			int len = epListen_.recvfrom(pCurrPacket()->data(), recvWindowSize_, *psin);
			if (len == -1)
			{
				if (showerr)
				{
					ERROR_MSG(fmt::format("IDComponentQuerier::receive: recvfrom error. {}.\n",
						kbe_strerror()));
				}

				continue;
			}

			pCurrPacket()->wpos(len);

			if (recvArgs != NULL)
			{
				try
				{
					recvArgs->createFromStream(*pCurrPacket());
				}
				catch (MemoryStreamException &)
				{
					ERROR_MSG(fmt::format("IDComponentQuerier::receive: data wrong. size=%d, from %s.\n",
						len, inet_ntoa((struct in_addr&)psin->sin_addr.s_addr)));

					continue;
				}
			}

			break;
		}
	}

	return true;
}

void IDComponentQuerier::send(COMPONENT_TYPE componentType, int32 uid, bool again /*= false*/)
{
	pCurrPacket()->clear(false);
	COMPONENT_ID cid = 0;
	uint16 port = epListen_.addr().port;

	int macMD5 = getMacMD5();

	WARNING_MSG(fmt::format("IDComponentQuerier::send: {}[{}], uid={} again={} macMD5={}\n", 
		componentType, COMPONENT_NAME_EX(componentType), uid, again, macMD5));

	newMessage(MachineInterface::queryComponentID);
	MachineInterface::queryComponentIDArgs5::staticAddToBundle(*this, componentType, cid, uid, port, macMD5);
	broadcast(0, again);
}

}



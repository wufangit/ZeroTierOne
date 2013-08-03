/*
 * ZeroTier One - Global Peer to Peer Ethernet
 * Copyright (C) 2012-2013  ZeroTier Networks LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <map>
#include <set>
#include <utility>
#include <algorithm>
#include <list>
#include <vector>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/file.h>
#endif

#include "Condition.hpp"
#include "Node.hpp"
#include "Topology.hpp"
#include "Demarc.hpp"
#include "Packet.hpp"
#include "Switch.hpp"
#include "Utils.hpp"
#include "EthernetTap.hpp"
#include "Logger.hpp"
#include "Constants.hpp"
#include "InetAddress.hpp"
#include "Salsa20.hpp"
#include "HMAC.hpp"
#include "RuntimeEnvironment.hpp"
#include "NodeConfig.hpp"
#include "Defaults.hpp"
#include "SysEnv.hpp"
#include "Network.hpp"
#include "MulticastGroup.hpp"
#include "Mutex.hpp"
#include "Multicaster.hpp"
#include "CMWC4096.hpp"
#include "Service.hpp"

#include "../version.h"

namespace ZeroTier {

struct _LocalClientImpl
{
	unsigned char key[32];
	UdpSocket *sock;
	void (*resultHandler)(void *,unsigned long,const char *);
	void *arg;
	InetAddress localDestAddr;
	Mutex inUseLock;
};

static void _CBlocalClientHandler(UdpSocket *sock,void *arg,const InetAddress &remoteAddr,const void *data,unsigned int len)
{
	_LocalClientImpl *impl = (_LocalClientImpl *)arg;
	if (!impl)
		return;
	if (!impl->resultHandler)
		return; // sanity check
	Mutex::Lock _l(impl->inUseLock);

	try {
		unsigned long convId = 0;
		std::vector<std::string> results;
		if (!NodeConfig::decodeControlMessagePacket(impl->key,data,len,convId,results))
			return;
		for(std::vector<std::string>::iterator r(results.begin());r!=results.end();++r)
			impl->resultHandler(impl->arg,convId,r->c_str());
	} catch ( ... ) {}
}

Node::LocalClient::LocalClient(const char *authToken,void (*resultHandler)(void *,unsigned long,const char *),void *arg)
	throw() :
	_impl((void *)0)
{
	_LocalClientImpl *impl = new _LocalClientImpl;

	UdpSocket *sock = (UdpSocket *)0;
	for(unsigned int i=0;i<5000;++i) {
		try {
			sock = new UdpSocket(true,32768 + (rand() % 20000),false,&_CBlocalClientHandler,impl);
			break;
		} catch ( ... ) {
			sock = (UdpSocket *)0;
		}
	}

	// If socket fails to bind, there's a big problem like missing IPv4 stack
	if (sock) {
		SHA256_CTX sha;
		SHA256_Init(&sha);
		SHA256_Update(&sha,authToken,strlen(authToken));
		SHA256_Final(impl->key,&sha);

		impl->sock = sock;
		impl->resultHandler = resultHandler;
		impl->arg = arg;
		impl->localDestAddr = InetAddress::LO4;
		impl->localDestAddr.setPort(ZT_CONTROL_UDP_PORT);
		_impl = impl;
	} else delete impl;
}

Node::LocalClient::~LocalClient()
{
	if (_impl) {
		((_LocalClientImpl *)_impl)->inUseLock.lock();
		delete ((_LocalClientImpl *)_impl)->sock;
		((_LocalClientImpl *)_impl)->inUseLock.unlock();
		delete ((_LocalClientImpl *)_impl);
	}
}

unsigned long Node::LocalClient::send(const char *command)
	throw()
{
	if (!_impl)
		return 0;
	_LocalClientImpl *impl = (_LocalClientImpl *)_impl;
	Mutex::Lock _l(impl->inUseLock);

	try {
		uint32_t convId = (uint32_t)rand();
		if (!convId)
			convId = 1;

		std::vector<std::string> tmp;
		tmp.push_back(std::string(command));
		std::vector< Buffer<ZT_NODECONFIG_MAX_PACKET_SIZE> > packets(NodeConfig::encodeControlMessage(impl->key,convId,tmp));

		for(std::vector< Buffer<ZT_NODECONFIG_MAX_PACKET_SIZE> >::iterator p(packets.begin());p!=packets.end();++p)
			impl->sock->send(impl->localDestAddr,p->data(),p->size(),-1);

		return convId;
	} catch ( ... ) {
		return 0;
	}
}

struct _NodeImpl
{
	RuntimeEnvironment renv;
	std::string reasonForTerminationStr;
	Node::ReasonForTermination reasonForTermination;
	volatile bool started;
	volatile bool running;
	volatile bool terminateNow;

	// Helper used to rapidly terminate from run()
	inline Node::ReasonForTermination terminateBecause(Node::ReasonForTermination r,const char *rstr)
	{
		RuntimeEnvironment *_r = &renv;
		LOG("terminating: %s",rstr);

		reasonForTerminationStr = rstr;
		reasonForTermination = r;
		running = false;
		return r;
	}
};

#ifndef __WINDOWS__
static void _netconfServiceMessageHandler(void *renv,Service &svc,const Dictionary &msg)
{
	if (!renv)
		return; // sanity check
	const RuntimeEnvironment *_r = (const RuntimeEnvironment *)renv;

	try {
		const std::string &type = msg.get("type");
		if (type == "netconf-response") {
			uint64_t inRePacketId = strtoull(msg.get("requestId").c_str(),(char **)0,16);
			SharedPtr<Network> network = _r->nc->network(strtoull(msg.get("nwid").c_str(),(char **)0,16));
			Address peerAddress(msg.get("peer").c_str());

			if ((network)&&(peerAddress)) {
				if (msg.contains("error")) {
					Packet::ErrorCode errCode = Packet::ERROR_INVALID_REQUEST;
					const std::string &err = msg.get("error");
					if (err == "NOT_FOUND")
						errCode = Packet::ERROR_NOT_FOUND;

					Packet outp(peerAddress,_r->identity.address(),Packet::VERB_ERROR);
					outp.append((unsigned char)Packet::VERB_NETWORK_CONFIG_REQUEST);
					outp.append(inRePacketId);
					outp.append((unsigned char)errCode);
					outp.append(network->id());
					_r->sw->send(outp,true);
				} else if (msg.contains("netconf")) {
					const std::string &netconf = msg.get("netconf");
					if (netconf.length() < 2048) { // sanity check
						Packet outp(peerAddress,_r->identity.address(),Packet::VERB_OK);
						outp.append((unsigned char)Packet::VERB_NETWORK_CONFIG_REQUEST);
						outp.append(inRePacketId);
						outp.append(network->id());
						outp.append((uint16_t)netconf.length());
						outp.append(netconf.data(),netconf.length());
						_r->sw->send(outp,true);
					}
				}
			}
		}
	} catch (std::exception &exc) {
		LOG("unexpected exception parsing response from netconf service: %s",exc.what());
	} catch ( ... ) {
		LOG("unexpected exception parsing response from netconf service: unknown exception");
	}
}
#endif // !__WINDOWS__

Node::Node(const char *hp)
	throw() :
	_impl(new _NodeImpl)
{
	_NodeImpl *impl = (_NodeImpl *)_impl;

	impl->renv.homePath = hp;

	impl->reasonForTermination = Node::NODE_RUNNING;
	impl->started = false;
	impl->running = false;
	impl->terminateNow = false;
}

Node::~Node()
{
	_NodeImpl *impl = (_NodeImpl *)_impl;

#ifndef __WINDOWS__
	delete impl->renv.netconfService;
#endif

	delete impl->renv.sysEnv;
	delete impl->renv.topology;
	delete impl->renv.sw;
	delete impl->renv.multicaster;
	delete impl->renv.demarc;
	delete impl->renv.nc;
	delete impl->renv.prng;
	delete impl->renv.log;

	delete impl;
}

/**
 * Execute node in current thread
 *
 * This does not return until the node shuts down. Shutdown may be caused
 * by an internally detected condition such as a new upgrade being
 * available or a fatal error, or it may be signaled externally using
 * the terminate() method.
 *
 * @return Reason for termination
 */
Node::ReasonForTermination Node::run()
	throw()
{
	_NodeImpl *impl = (_NodeImpl *)_impl;
	RuntimeEnvironment *_r = (RuntimeEnvironment *)&(impl->renv);

	impl->started = true;
	impl->running = true;

	try {
#ifdef ZT_LOG_STDOUT
		_r->log = new Logger((const char *)0,(const char *)0,0);
#else
		_r->log = new Logger((_r->homePath + ZT_PATH_SEPARATOR_S + "node.log").c_str(),(const char *)0,131072);
#endif

		TRACE("initializing...");

		// Create non-crypto PRNG right away in case other code in init wants to use it
		_r->prng = new CMWC4096();

		bool gotId = false;
		std::string identitySecretPath(_r->homePath + ZT_PATH_SEPARATOR_S + "identity.secret");
		std::string identityPublicPath(_r->homePath + ZT_PATH_SEPARATOR_S + "identity.public");
		std::string idser;
		if (Utils::readFile(identitySecretPath.c_str(),idser))
			gotId = _r->identity.fromString(idser);
		if (gotId) {
			// Make sure identity.public matches identity.secret
			idser = std::string();
			Utils::readFile(identityPublicPath.c_str(),idser);
			std::string pubid(_r->identity.toString(false));
			if (idser != pubid) {
				if (!Utils::writeFile(identityPublicPath.c_str(),pubid))
					return impl->terminateBecause(Node::NODE_UNRECOVERABLE_ERROR,"could not write identity.public (home path not writable?)");
			}
		} else {
			LOG("no identity found, generating one... this might take a few seconds...");
			_r->identity.generate();
			LOG("generated new identity: %s",_r->identity.address().toString().c_str());
			idser = _r->identity.toString(true);
			if (!Utils::writeFile(identitySecretPath.c_str(),idser))
				return impl->terminateBecause(Node::NODE_UNRECOVERABLE_ERROR,"could not write identity.secret (home path not writable?)");
			idser = _r->identity.toString(false);
			if (!Utils::writeFile(identityPublicPath.c_str(),idser))
				return impl->terminateBecause(Node::NODE_UNRECOVERABLE_ERROR,"could not write identity.public (home path not writable?)");
		}
		Utils::lockDownFile(identitySecretPath.c_str(),false);

		// Clean up some obsolete files if present -- this will be removed later
		unlink((_r->homePath + ZT_PATH_SEPARATOR_S + "status").c_str());
		unlink((_r->homePath + ZT_PATH_SEPARATOR_S + "thisdeviceismine").c_str());

		// Load or generate config authentication secret
		std::string configAuthTokenPath(_r->homePath + ZT_PATH_SEPARATOR_S + "authtoken.secret");
		std::string configAuthToken;
		if (!Utils::readFile(configAuthTokenPath.c_str(),configAuthToken)) {
			configAuthToken = "";
			unsigned int sr = 0;
			for(unsigned int i=0;i<24;++i) {
				Utils::getSecureRandom(&sr,sizeof(sr));
				configAuthToken.push_back("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[sr % 62]);
			}
			if (!Utils::writeFile(configAuthTokenPath.c_str(),configAuthToken))
				return impl->terminateBecause(Node::NODE_UNRECOVERABLE_ERROR,"could not write authtoken.secret (home path not writable?)");
		}
		Utils::lockDownFile(configAuthTokenPath.c_str(),false);

		// Create the core objects in RuntimeEnvironment: node config, demarcation
		// point, switch, network topology database, and system environment
		// watcher.
		try {
			_r->nc = new NodeConfig(_r,configAuthToken.c_str());
		} catch ( ... ) {
			// An exception here currently means that another instance of ZeroTier
			// One is running.
			return impl->terminateBecause(Node::NODE_UNRECOVERABLE_ERROR,"another instance of ZeroTier One appears to be running, or local control UDP port cannot be bound");
		}
		_r->demarc = new Demarc(_r);
		_r->multicaster = new Multicaster();
		_r->sw = new Switch(_r);
		_r->topology = new Topology(_r,(_r->homePath + ZT_PATH_SEPARATOR_S + "peer.db").c_str());
		_r->sysEnv = new SysEnv(_r);

		// TODO: make configurable
		bool boundPort = false;
		for(unsigned int p=ZT_DEFAULT_UDP_PORT;p<(ZT_DEFAULT_UDP_PORT + 128);++p) {
			if (_r->demarc->bindLocalUdp(p)) {
				boundPort = true;
				break;
			}
		}
		if (!boundPort)
			return impl->terminateBecause(Node::NODE_UNRECOVERABLE_ERROR,"could not bind any local UDP ports");

		// TODO: bootstrap off network so we don't have to update code for
		// changes in supernodes.
		_r->topology->setSupernodes(ZT_DEFAULTS.supernodes);
	} catch (std::bad_alloc &exc) {
		return impl->terminateBecause(Node::NODE_UNRECOVERABLE_ERROR,"memory allocation failure");
	} catch (std::runtime_error &exc) {
		return impl->terminateBecause(Node::NODE_UNRECOVERABLE_ERROR,exc.what());
	} catch ( ... ) {
		return impl->terminateBecause(Node::NODE_UNRECOVERABLE_ERROR,"unknown exception during initialization");
	}

#ifndef __WINDOWS__
	try {
		std::string netconfServicePath(_r->homePath + ZT_PATH_SEPARATOR_S + "services.d" + ZT_PATH_SEPARATOR_S + "netconf.service");
		if (Utils::fileExists(netconfServicePath.c_str())) {
			LOG("netconf.d/netconfi.service appears to exist, starting...");
			_r->netconfService = new Service(_r,"netconf",netconfServicePath.c_str(),&_netconfServiceMessageHandler,_r);
		}
	} catch ( ... ) {
		LOG("unexpected exception attempting to start services");
	}
#endif

	try {
		uint64_t lastPingCheck = 0;
		uint64_t lastClean = Utils::now(); // don't need to do this immediately
		uint64_t lastNetworkFingerprintCheck = 0;
		uint64_t lastAutoconfigureCheck = 0;
		uint64_t networkConfigurationFingerprint = _r->sysEnv->getNetworkConfigurationFingerprint();
		uint64_t lastMulticastCheck = 0;
		uint64_t lastMulticastAnnounceAll = 0;
		long lastDelayDelta = 0;

		LOG("%s starting version %s",_r->identity.address().toString().c_str(),versionString());

		while (!impl->terminateNow) {
			uint64_t now = Utils::now();
			bool pingAll = false; // set to true to force a ping of *all* known direct links

			// Detect sleep/wake by looking for delay loop pauses that are longer
			// than we intended to pause.
			if (lastDelayDelta >= ZT_SLEEP_WAKE_DETECTION_THRESHOLD) {
				lastNetworkFingerprintCheck = 0; // force network environment check
				lastMulticastCheck = 0; // force multicast group check on taps
				pingAll = true;

				LOG("probable suspend/resume detected, pausing a moment for things to settle...");
				Thread::sleep(ZT_SLEEP_WAKE_SETTLE_TIME);
			}

			// Periodically check our network environment, sending pings out to all
			// our direct links if things look like we got a different address.
			if ((now - lastNetworkFingerprintCheck) >= ZT_NETWORK_FINGERPRINT_CHECK_DELAY) {
				lastNetworkFingerprintCheck = now;
				uint64_t fp = _r->sysEnv->getNetworkConfigurationFingerprint();
				if (fp != networkConfigurationFingerprint) {
					LOG("netconf fingerprint change: %.16llx != %.16llx, resyncing with network",networkConfigurationFingerprint,fp);
					networkConfigurationFingerprint = fp;
					pingAll = true;
					lastAutoconfigureCheck = 0; // check autoconf after network config change
					lastMulticastCheck = 0; // check multicast group membership after network config change
					_r->nc->whackAllTaps(); // call whack() on all tap devices
				}
			}

			// Periodically check for changes in our local multicast subscriptions and broadcast
			// those changes to peers.
			if ((now - lastMulticastCheck) >= ZT_MULTICAST_LOCAL_POLL_PERIOD) {
				lastMulticastCheck = now;
				bool announceAll = ((now - lastMulticastAnnounceAll) >= ZT_MULTICAST_LIKE_ANNOUNCE_ALL_PERIOD);
				try {
					std::map< SharedPtr<Network>,std::set<MulticastGroup> > toAnnounce;
					{
						std::vector< SharedPtr<Network> > networks(_r->nc->networks());
						for(std::vector< SharedPtr<Network> >::const_iterator nw(networks.begin());nw!=networks.end();++nw) {
							if (((*nw)->updateMulticastGroups())||(announceAll))
								toAnnounce.insert(std::pair< SharedPtr<Network>,std::set<MulticastGroup> >(*nw,(*nw)->multicastGroups()));
						}
					}

					if (toAnnounce.size()) {
						_r->sw->announceMulticastGroups(toAnnounce);

						// Only update lastMulticastAnnounceAll if we've announced something. This keeps
						// the announceAll condition true during startup when there are no multicast
						// groups until there is at least one. Technically this shouldn't be required as
						// updateMulticastGroups() should return true on any change, but why not?
						if (announceAll)
							lastMulticastAnnounceAll = now;
					}
				} catch (std::exception &exc) {
					LOG("unexpected exception announcing multicast groups: %s",exc.what());
				} catch ( ... ) {
					LOG("unexpected exception announcing multicast groups: (unknown)");
				}
			}

			if ((now - lastPingCheck) >= ZT_PING_CHECK_DELAY) {
				lastPingCheck = now;
				try {
					if (_r->topology->amSupernode()) {
						// Supernodes do not ping anyone but each other. They also don't
						// send firewall openers, since they aren't ever firewalled.
						std::vector< SharedPtr<Peer> > sns(_r->topology->supernodePeers());
						for(std::vector< SharedPtr<Peer> >::const_iterator p(sns.begin());p!=sns.end();++p) {
							if ((now - (*p)->lastDirectSend()) > ZT_PEER_DIRECT_PING_DELAY)
								_r->sw->sendHELLO((*p)->address());
						}
					} else {
						std::vector< SharedPtr<Peer> > needPing,needFirewallOpener;

						if (pingAll) {
							_r->topology->eachPeer(Topology::CollectPeersWithActiveDirectPath(needPing));
						} else {
							_r->topology->eachPeer(Topology::CollectPeersThatNeedPing(needPing));
							_r->topology->eachPeer(Topology::CollectPeersThatNeedFirewallOpener(needFirewallOpener));
						}

						for(std::vector< SharedPtr<Peer> >::iterator p(needPing.begin());p!=needPing.end();++p) {
							try {
								_r->sw->sendHELLO((*p)->address());
							} catch (std::exception &exc) {
								LOG("unexpected exception sending HELLO to %s: %s",(*p)->address().toString().c_str());
							} catch ( ... ) {
								LOG("unexpected exception sending HELLO to %s: (unknown)",(*p)->address().toString().c_str());
							}
						}

						for(std::vector< SharedPtr<Peer> >::iterator p(needFirewallOpener.begin());p!=needFirewallOpener.end();++p) {
							try {
								(*p)->sendFirewallOpener(_r,now);
							} catch (std::exception &exc) {
								LOG("unexpected exception sending firewall opener to %s: %s",(*p)->address().toString().c_str(),exc.what());
							} catch ( ... ) {
								LOG("unexpected exception sending firewall opener to %s: (unknown)",(*p)->address().toString().c_str());
							}
						}
					}
				} catch (std::exception &exc) {
					LOG("unexpected exception running ping check cycle: %s",exc.what());
				} catch ( ... ) {
					LOG("unexpected exception running ping check cycle: (unkonwn)");
				}
			}

			if ((now - lastClean) >= ZT_DB_CLEAN_PERIOD) {
				lastClean = now;
				_r->topology->clean();
				_r->nc->cleanAllNetworks();
			}

			try {
				unsigned long delay = std::min((unsigned long)ZT_MIN_SERVICE_LOOP_INTERVAL,_r->sw->doTimerTasks());
				uint64_t start = Utils::now();
				_r->mainLoopWaitCondition.wait(delay);
				lastDelayDelta = (long)(Utils::now() - start) - (long)delay;
			} catch (std::exception &exc) {
				LOG("unexpected exception running Switch doTimerTasks: %s",exc.what());
			} catch ( ... ) {
				LOG("unexpected exception running Switch doTimerTasks: (unknown)");
			}
		}
	} catch ( ... ) {
		return impl->terminateBecause(Node::NODE_UNRECOVERABLE_ERROR,"unexpected exception during outer main I/O loop");
	}

	return impl->terminateBecause(Node::NODE_NORMAL_TERMINATION,"normal termination");
}

const char *Node::reasonForTermination() const
	throw()
{
	if ((!((_NodeImpl *)_impl)->started)||(((_NodeImpl *)_impl)->running))
		return (const char *)0;
	return ((_NodeImpl *)_impl)->reasonForTerminationStr.c_str();
}

void Node::terminate()
	throw()
{
	((_NodeImpl *)_impl)->terminateNow = true;
	((_NodeImpl *)_impl)->renv.mainLoopWaitCondition.signal();
}

class _VersionStringMaker
{
public:
	char vs[32];
	_VersionStringMaker()
	{
		sprintf(vs,"%d.%d.%d",(int)ZEROTIER_ONE_VERSION_MAJOR,(int)ZEROTIER_ONE_VERSION_MINOR,(int)ZEROTIER_ONE_VERSION_REVISION);
	}
	~_VersionStringMaker() {}
};
static const _VersionStringMaker __versionString;

const char *Node::versionString() throw() { return __versionString.vs; }

unsigned int Node::versionMajor() throw() { return ZEROTIER_ONE_VERSION_MAJOR; }
unsigned int Node::versionMinor() throw() { return ZEROTIER_ONE_VERSION_MINOR; }
unsigned int Node::versionRevision() throw() { return ZEROTIER_ONE_VERSION_REVISION; }

// Scanned for by loader and/or updater to determine a binary's version
const unsigned char EMBEDDED_VERSION_STAMP[20] = {
	0x6d,0xfe,0xff,0x01,0x90,0xfa,0x89,0x57,0x88,0xa1,0xaa,0xdc,0xdd,0xde,0xb0,0x33,
	ZEROTIER_ONE_VERSION_MAJOR,
	ZEROTIER_ONE_VERSION_MINOR,
	(unsigned char)(((unsigned int)ZEROTIER_ONE_VERSION_REVISION) & 0xff), /* little-endian */
	(unsigned char)((((unsigned int)ZEROTIER_ONE_VERSION_REVISION) >> 8) & 0xff)
};

} // namespace ZeroTier

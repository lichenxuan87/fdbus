/*
 * Copyright (C) 2015   Jeremy Chen jeremy_cz@yahoo.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <common_base/CBaseEndpoint.h>
#include <common_base/CFdbContext.h>
#include <common_base/CBaseSocketFactory.h>
#include <common_base/CFdbSession.h>
#include <common_base/CFdbMessage.h>
#include <utils/Log.h>

CBaseEndpoint::CBaseEndpoint(const char *name, CBaseWorker *worker, EFdbEndpointRole role)
    : CFdbBaseObject(name, worker, role)
    , mNsConnStatus(DISCONNECTED)
    , mSessionCnt(0)
    , mSnAllocator(1)
{
    mObjId = FDB_OBJECT_MAIN;
    registerSelf();
}

CBaseEndpoint::~CBaseEndpoint()
{
    enableMigrate(false);
    autoRemove(false);
    
    tObjectContainer::EntryContainer_t &object_tbl = mObjectContainer.getContainer();
    while (!object_tbl.empty())
    {
        tObjectContainer::EntryContainer_t::iterator it = object_tbl.begin();
        CFdbBaseObject *object = it->second;
        removeObject(object);
    }
    
    deleteSocket();
    unregisterSelf();
    //CFdbContext::getInstance()->mEndpointContainer.deleteEntry(mEpid);
}


void CBaseEndpoint::addSocket(CFdbSessionContainer *container)
{
    insertEntry(container->skid(), container);
}

void CBaseEndpoint::deleteSocket(FdbSocketId_t skid)
{
    if (isValidFdbId(skid))
    {
        CFdbSessionContainer *container = 0;
        EntryContainer_t::iterator it = retrieveEntry(skid, container);
        if (container)
        {
            delete container; // also deleted from socket container
        }
    }
    else
    {
        EntryContainer_t &containers = getContainer();
        while (!containers.empty())
        {
            EntryContainer_t::iterator it = containers.begin();
            delete it->second; // also deleted from socket container
        }
    }
}

void CBaseEndpoint::getDefaultSvcUrl(std::string &svc_url)
{
    svc_url = FDB_URL_SVC;
    if (mNsName.empty())
    {
        svc_url += mName;
    }
    else
    {
        svc_url += mNsName;
    }
}

CFdbSession *CBaseEndpoint::preferredPeer()
{
    CFdbSession *session = 0;
    if (isValidFdbId(mSid))
    {
        session = CFdbContext::getInstance()->getSession(mSid);
    }
    if (session)
    {
        return session;
    }
    EntryContainer_t &containers = getContainer();
    EntryContainer_t::iterator it = containers.begin();
    if (it == containers.end())
    {
        session = 0;
    }
    else
    {
        CFdbSessionContainer *sessions = it->second;
        session = sessions->getDefaultSession();
    }

    return session;
}

void CBaseEndpoint::checkAutoRemove()
{
    if (autoRemove() && !getSessionCount())
    {
        delete this;
    }
}

CFdbSessionContainer *CBaseEndpoint::getSocketByUrl(const char *url)
{
    EntryContainer_t &containers = getContainer();
    for (EntryContainer_t::iterator it = containers.begin();
            it != containers.end(); ++it)
    {
        CFdbSessionContainer *container = it->second;
        CFdbSocketInfo info;
        container->getSocketInfo(info);
        if (!info.mAddress->mUrl.compare(url))
        {
            return container;
        }
    }

    return 0;
}

void CBaseEndpoint::getUrlList(std::vector<std::string> &url_list)
{
    EntryContainer_t &containers = getContainer();
    for (EntryContainer_t::iterator it = containers.begin();
            it != containers.end(); ++it)
    {
        CFdbSessionContainer *container = it->second;
        CFdbSocketInfo info;
        container->getSocketInfo(info);
        url_list.push_back(info.mAddress->mUrl);
    }
}

FdbObjectId_t CBaseEndpoint::addObject(CFdbBaseObject *obj)
{
    FdbObjectId_t obj_id = obj->objId();
    if (obj_id == FDB_OBJECT_MAIN)
    {
        return FDB_INVALID_ID;
    }

    if (!isValidFdbId(obj_id))
    {
        do
        {
            obj_id = mObjectContainer.allocateEntityId();
        } while (obj_id == FDB_OBJECT_MAIN);
    }

    if (obj->role() == FDB_OBJECT_ROLE_SERVER)
    {
        if (findObject(obj_id, true))
        {
            LOG_E("CBaseEndpoint: server object %d already exist!\n", obj_id);
            return FDB_INVALID_ID;
        }
    }

    obj_id = FDB_OBJECT_MAKE_ID(mSnAllocator++, obj_id);
    obj->objId(obj_id);
    if (mObjectContainer.insertEntry(obj_id, obj))
    {
        return FDB_INVALID_ID;
    }

    obj->enableMigrate(true);
    bool is_first = true;
    EntryContainer_t &containers = getContainer();
    for (EntryContainer_t::iterator socket_it = containers.begin();
            socket_it != containers.end(); ++socket_it)
    {
        CFdbSessionContainer *container = socket_it->second;
        CFdbSessionContainer::ConnectedSessionTable_t::iterator session_it;
        for (session_it = container->mConnectedSessionTable.begin();
             session_it != container->mConnectedSessionTable.end(); ++session_it)
        {
            CFdbSession *session = *session_it;
            CFdbSessionInfo info;
            session->getSessionInfo(info);

            if (!obj->authentication(info))
            {
                continue;
            }
            obj->notifyOnline(session, is_first);
            is_first = false;
        }
    }

    return obj_id;
}

void CBaseEndpoint::removeObject(CFdbBaseObject *obj)
{
    bool is_last = false;
    uint32_t session_cnt = mSessionCnt;

    EntryContainer_t &containers = getContainer();
    for (EntryContainer_t::iterator socket_it = containers.begin();
            socket_it != containers.end(); ++socket_it)
    {
        CFdbSessionContainer *container = socket_it->second;
        CFdbSessionContainer::ConnectedSessionTable_t::iterator session_it;
        for (session_it = container->mConnectedSessionTable.begin();
             session_it != container->mConnectedSessionTable.end(); ++session_it)
        {
            if (session_cnt-- == 1)
            {
                is_last = true;
            }
        
            obj->notifyOffline(*session_it, is_last);
        }
    }
    
    mObjectContainer.deleteEntry(obj->objId());
    // obj->objId(FDB_INVALID_ID);
    obj->enableMigrate(false);
}

void CBaseEndpoint::unsubscribeSession(CFdbSession *session)
{
    tObjectContainer::EntryContainer_t &object_tbl = mObjectContainer.getContainer();
    tObjectContainer::EntryContainer_t::iterator it;
    for (it = object_tbl.begin(); it != object_tbl.end(); ++it)
    {
        CFdbBaseObject *object = it->second;
        object->unsubscribe(session);
    }
    
    unsubscribe(session);
}

class CKickOutSessionJob : public CBaseJob
{
public:
    CKickOutSessionJob(FdbSessionId_t sid)
        : CBaseJob(JOB_FORCE_RUN)
        , mSid(sid)
    {
    }
protected:
    void run(CBaseWorker *worker, Ptr &ref)
    {
        CFdbContext::getInstance()->deleteSession(mSid);
    }
private:
    FdbSessionId_t mSid;
};

void CBaseEndpoint::kickOut(FdbSessionId_t sid)
{
    CFdbContext::getInstance()->sendAsyncEndeavor(new CKickOutSessionJob(sid));
}

CFdbBaseObject *CBaseEndpoint::getObject(CFdbMessage *msg, bool server_only)
{
    FdbObjectId_t obj_id = msg->objectId();
    if (obj_id == FDB_OBJECT_MAIN)
    {
        return this;
    }
    
    CFdbBaseObject *object = 0;
    bool tried_to_create = false;

    while (1)
    {
        object = findObject(obj_id, server_only);
        if (object || tried_to_create)
        {
            break;
        }
        
        onCreateObject(this, msg);
        tried_to_create = true;
    }

    return object;
}

bool CBaseEndpoint::addConnectedSession(CFdbSessionContainer *socket, CFdbSession *session)
{
    CFdbSessionInfo info;
    session->getSessionInfo(info);
    if (!authentication(info))
    {
        return false;
    }

    bool is_first = !mSessionCnt;

    socket->addSession(session);
    mSessionCnt++;
    
    tObjectContainer::EntryContainer_t &object_tbl = mObjectContainer.getContainer();
    tObjectContainer::EntryContainer_t::iterator it;
    for (it = object_tbl.begin(); it != object_tbl.end(); ++it)
    {
        CFdbBaseObject *object = it->second;
        if (!object->authentication(info))
        {
            continue;
        }
        object->notifyOnline(session, is_first);
    }

    notifyOnline(session, is_first);
    
    return true;
}

void CBaseEndpoint::deleteConnectedSession(CFdbSession *session)
{
    bool is_last = (mSessionCnt == 1);
    
    session->container()->removeSession(session);
    mSessionCnt--;

    notifyOffline(session, is_last);
    
    tObjectContainer::EntryContainer_t &object_tbl = mObjectContainer.getContainer();
    tObjectContainer::EntryContainer_t::iterator it;
    for (it = object_tbl.begin(); it != object_tbl.end(); ++it)
    {
        CFdbBaseObject *object = it->second;
        object->notifyOffline(session, is_last);
    }
}

bool CBaseEndpoint::retrieveIpAddress(std::string &ip_address, CFdbSession *session, bool self)
{
    if (!session)
    {
        session = preferredPeer();
    }
    
    if (session)
    {
        CFdbSessionInfo sinfo;
        session->getSessionInfo(sinfo);
        if (sinfo.mSocketInfo.mAddress->mType == FDB_SOCKET_IPC)
        {
            return false;
        }
        ip_address = self ? sinfo.mConn->mSelfIp : sinfo.mConn->mPeerIp;
        return true;
    }
    return false;
}


bool CBaseEndpoint::hostIp(std::string &host_ip, CFdbSession *session)
{
    return retrieveIpAddress(host_ip, session, true);
}

bool CBaseEndpoint::peerIp(std::string &peer_ip, CFdbSession *session)
{
    return retrieveIpAddress(peer_ip, session, false);
}

bool CBaseEndpoint::replaceUrlIpAddress(std::string &url, CFdbSession *session,
                                        std::string *ip_addr, int32_t *port)
{
    CFdbSocketAddr addr;
    if (CBaseSocketFactory::parseUrl(url.c_str(), addr))
    {
        std::string peer_ip;
        if (addr.mType != FDB_SOCKET_TCP)
        {
            return false;
        }
        if (addr.mAddr == FDB_IP_ALL_INTERFACE)
        {
            peerIp(peer_ip, session);
            if (peer_ip.empty())
            {
                return false;
            }
            CBaseSocketFactory::buildUrl(url, addr.mType, peer_ip.c_str(), addr.mPort);
        }
        else
        {
            url = addr.mUrl;
        }
        
        if (ip_addr)
        {
            *ip_addr = peer_ip;
        }
        if (port)
        {
            *port = addr.mPort;
        }
        return true;
    }
    return false;
}

CFdbBaseObject *CBaseEndpoint::findObject(FdbObjectId_t obj_id, bool server_only)
{
    tObjectContainer::EntryContainer_t &object_tbl = mObjectContainer.getContainer();
    tObjectContainer::EntryContainer_t::iterator it;
    for (it = object_tbl.begin(); it != object_tbl.end(); ++it)
    {
        CFdbBaseObject *object = it->second;
        if (server_only)
        {
            if ((object->role() == FDB_OBJECT_ROLE_SERVER) &&
                (FDB_OBJECT_GET_CLASS(obj_id) == 
                    FDB_OBJECT_GET_CLASS(object->objId())))
            {
                return object;
            }
        }
        else if (obj_id == object->objId())
        {
            return object;
        }
    }
    return 0;
}

//================================== register ==========================================
class CRegisterJob : public CMethodJob<CBaseEndpoint>
{
public:
    CRegisterJob(CBaseEndpoint *object, FdbEndpointId_t &epid)
        : CMethodJob<CBaseEndpoint>(object, &CBaseEndpoint::callRegisterEndpoint, JOB_FORCE_RUN)
        , mEpid(epid)
    {
    }

    FdbEndpointId_t &mEpid;
};

void CBaseEndpoint::callRegisterEndpoint(CBaseWorker *worker, CMethodJob<CBaseEndpoint> *job, CBaseJob::Ptr &ref)
{
    CRegisterJob *the_job = dynamic_cast<CRegisterJob *>(job);
    if (the_job)
    {
        the_job->mEpid = CFdbContext::getInstance()->registerEndpoint(this);
        mNsConnStatus = DISCONNECTED;
        registered(true);
    }
}

FdbEndpointId_t CBaseEndpoint::registerSelf()
{
    FdbEndpointId_t epid = mEpid;
    if (!registered())
    {
        CFdbContext::getInstance()->sendSyncEndeavor(new CRegisterJob(this, epid));
        if (isValidFdbId(epid))
        {
            registered(true);
        }
    }
    return epid;
}

//================================== unregister ==========================================
class CUnregisterJob : public CMethodJob<CBaseEndpoint>
{
public:
    CUnregisterJob(CBaseEndpoint *object)
        : CMethodJob<CBaseEndpoint>(object, &CBaseEndpoint::callUnregisterEndpoint, JOB_FORCE_RUN)
    {
    }
};

void CBaseEndpoint::callUnregisterEndpoint(CBaseWorker *worker, CMethodJob<CBaseEndpoint> *job, CBaseJob::Ptr &ref)
{
    CFdbContext::getInstance()->unregisterEndpoint(this);
    mNsConnStatus = DISCONNECTED;
}

void CBaseEndpoint::unregisterSelf()
{
    if (registered())
    {
        CFdbContext::getInstance()->sendSyncEndeavor(new CUnregisterJob(this));
        registered(false);
    }
}

